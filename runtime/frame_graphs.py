"""Chained CUDA graphs for one 10 ms frame of the pinned realtime engine.

The engine replays seven graphs per frame. It copies every tensor from one
graph to the next, clones the Mamba states out of each graph and rotates the
attention keys and queries outside them. Here each graph reads the previous
graph's results in place, keeps its Mamba state in its own buffers and
rotates the keys and queries itself. What stays outside is the attention
over the session's key and value history, whose length changes every frame.
The arithmetic and its order are the engine's, so the output is bit-identical.
"""
import weakref

import torch
from inworld.voice_isolation import streaming
from streaming_v2 import NetworkSections, attention_input, finish_attention

ATTENTION_BLOCKS = (0, 2, 4, 6)


def rotate(x, cos, sin):
    """modeling.apply_rope with one table row per attention row."""
    half = x.shape[-1] // 2
    x1, x2 = x[..., :half], x[..., half:]
    return torch.cat((x1 * cos - x2 * sin, x1 * sin + x2 * cos), dim=-1)


class Chain:
    """The five graphs of a frame for a fixed number of attention lanes."""

    def __init__(self, sections, session, lanes):
        self.sections, self.lanes = sections, lanes
        self.bands = len(sections.network.edges)
        cache = session._attention
        dtype = session.autocast_dtype or torch.float32
        self.cos, self.sin = cache.cos.to(dtype), cache.sin.to(dtype)
        self.position = torch.zeros(lanes * self.bands, dtype=torch.long, device=cache.cos.device)
        self.side = torch.cuda.Stream()
        self.graphs = [None] * 5
        self.spectrum = self.raw = self.tables = None
        self.rows, self.value, self.query, self.key = ([None] * 4 for _ in range(4))
        self.attended = [None] * 4
        self.history = self.recurrence = None
        self.bound, self.assigned = [], []

    def part(self, lane):
        return slice(lane * self.bands, (lane + 1) * self.bands)

    def bind(self, lanes):
        """Keep the lanes' Mamba states in this chain's buffers.

        A state lives in the buffers of the chain that ran its lane last, and
        its ``carried`` tuple views them, so the official session still clones
        the right values when a crossfade starts. Lanes change chains only
        where a crossfade starts or ends.
        """
        states = [lane[2 * block + 1] for block in range(3) for lane in lanes]
        if len(states) == len(self.bound) and all(
                state is bound and state.carried is views
                for state, bound, views in zip(states, self.bound, self.assigned)):
            return
        for state, views in zip(self.bound, self.assigned):
            if state.carried is views:
                state.carried = tuple(part.clone() for part in views)
        if self.history is None:
            rows = self.lanes * self.bands
            first = [lanes[0][2 * block + 1].carried for block in range(3)]
            self.history = [h.new_empty((rows, *h.shape[1:])) for h, _ in first]
            self.recurrence = [r.new_empty((rows, *r.shape[1:])) for _, r in first]
        self.bound, self.assigned = states, []
        for block in range(3):
            for index, lane in enumerate(lanes):
                state = lane[2 * block + 1]
                views = self.history[block][self.part(index)], self.recurrence[block][self.part(index)]
                views[0].copy_(state.carried[0])
                views[1].copy_(state.carried[1])
                state.carried = views
                self.assigned.append(views)

    def split(self, rows, block, cos, sin):
        query, key, value = attention_input(block, rows).unbind(2)
        return rows, value, rotate(query.transpose(1, 2), cos, sin), rotate(key.transpose(1, 2), cos, sin)

    def head(self):
        sections = self.sections
        features = sections.packed.encode(sections.network, self.spectrum)
        if self.lanes > 1:
            features = features.repeat(self.lanes, 1, 1, 1)
        rows = features.reshape(-1, 1, features.shape[-1])
        cos = self.cos.index_select(0, self.position)[:, None, None]
        sin = self.sin.index_select(0, self.position)[:, None, None]
        return *self.split(rows, sections.network.blocks[0], cos, sin), cos, sin

    def body(self, index):
        blocks = self.sections.network.blocks
        features = finish_attention(blocks[2 * index], self.rows[index], self.attended[index],
                                    self.value[index], self.bands)
        state = streaming._MambaState()
        state.carried = self.history[index], self.recurrence[index]
        features = streaming._scan(blocks[2 * index + 1], features, [state])
        rows = features.reshape(-1, 1, features.shape[-1])
        result = self.split(rows, blocks[2 * index + 2], *self.tables)
        self.history[index].copy_(state.carried[0])
        self.recurrence[index].copy_(state.carried[1])
        return result

    def tail(self):
        features = finish_attention(self.sections.network.blocks[6], self.rows[3], self.attended[3],
                                    self.value[3], self.bands)
        return self.sections.packed.decode(features)

    def capture(self, function, written=()):
        """Capture ``function`` and run it once, as the engine's FixedSection does.

        The trial runs and the capture itself advance the Mamba state the
        function writes back, so that state is restored before the replay.
        """
        saved = [tensor.clone() for tensor in written]
        current = torch.cuda.current_stream()
        self.side.wait_stream(current)
        with torch.cuda.stream(self.side):
            for _ in range(3):
                function()
                for tensor, value in zip(written, saved):
                    tensor.copy_(value)
        current.wait_stream(self.side)
        graph = torch.cuda.CUDAGraph()
        with torch.cuda.graph(graph, stream=self.side):
            result = function()
        for tensor, value in zip(written, saved):
            tensor.copy_(value)
        graph.replay()
        return graph, result

    def run(self, index):
        if self.graphs[index] is not None:
            self.graphs[index].replay()
        elif index == 0:
            self.graphs[0], result = self.capture(self.head)
            self.rows[0], self.value[0], self.query[0], self.key[0], *self.tables = result
        elif index == 4:
            self.graphs[4], self.raw = self.capture(self.tail)
        else:
            block = index - 1
            self.graphs[index], result = self.capture(lambda: self.body(block),
                                                      (self.history[block], self.recurrence[block]))
            self.rows[index], self.value[index], self.query[index], self.key[index] = result

    def step(self, session, spectrum, lanes):
        """Decoder output [lanes, bins, 1, 4] of one frame. It is overwritten by the next step."""
        cache = session._attention
        self.bind(lanes)
        for lane in lanes:
            for block in ATTENTION_BLOCKS:
                if lane[block].end + 1 > cache.capacity:
                    streaming._compact(lane[block], cache)
        positions = [lane[0].end for lane in lanes]
        if len(lanes) == 1:
            self.position.fill_(positions[0])
        else:
            for index, position in enumerate(positions):
                self.position[self.part(index)].fill_(position)
        if self.spectrum is None:
            self.spectrum = spectrum.clone()
        else:
            self.spectrum.copy_(spectrum)
        self.run(0)
        for index, block in enumerate(ATTENTION_BLOCKS):
            for number, lane in enumerate(lanes):
                state, part = lane[block], self.part(number)
                value = self.value[index][part]
                streaming._reserve(state, state.end + 1, cache.capacity, value, value)
                first = state.end
                if first != positions[number]:
                    raise RuntimeError("The attention blocks of a lane hold different numbers of frames.")
                state.keys[:, :, first:first + 1] = self.key[index][part]
                state.values[:, :, first:first + 1] = value.transpose(1, 2)
                state.end = first + 1
                past = min(first, cache.frames)
                output = torch.nn.functional.scaled_dot_product_attention(
                    self.query[index][part], state.keys[:, :, first - past:state.end],
                    state.values[:, :, first - past:state.end], attn_mask=cache.mask(1, past))
                if self.attended[index] is None:
                    self.attended[index] = output.new_empty((self.lanes * self.bands, *output.shape[1:]))
                self.attended[index][part].copy_(output)
            self.run(index + 1)
        return self.raw


class ChainedSections(NetworkSections):
    """The engine's sections, with every one-frame mono block run by a Chain."""

    def __init__(self, network, packed):
        super().__init__(network, packed)
        self.network, self.packed = network, packed
        self.chains = {}

    def chain(self, session, lanes):
        cache = session._attention
        key = lanes, cache.capacity, session.autocast_dtype
        if key not in self.chains:
            self.chains[key] = Chain(self, session, lanes)
        return self.chains[key]

    def attach(self, session):
        super().attach(session)
        if self.network.config.mode_film:
            return
        bands = session._bands
        encode, decode, backbone = bands.encode, bands.decode, session._backbone
        current = weakref.proxy(session)
        # The official session passes the encoder's result to the backbone and
        # the backbone's to the decoder. A chain runs all three at once, so the
        # spectrum and the finished decoder output pass through unchanged.
        spectrum, finished = [None], [None]

        def chained_encode(model, x):
            if x.shape[0] != 1 or x.shape[2] != 1 or current.states[1].carried is None:
                return encode(model, x)
            spectrum[0] = x
            return x

        def chained_backbone(features, lanes, film):
            if features is not spectrum[0]:
                return backbone(features, lanes, film)
            spectrum[0] = None
            finished[0] = self.chain(current, len(lanes)).step(current, features, lanes)
            return finished[0]

        def chained_decode(x):
            if x is not finished[0]:
                return decode(x)
            finished[0] = None
            # Frames wait in the session for their look-ahead; a chain reuses its output.
            return x.clone() if current.lookahead else x

        bands.encode, bands.decode, session._backbone = chained_encode, chained_decode, chained_backbone
