"""Local Windows worker for one 48 kHz Inworld Isolator 38 VST3 instance."""
import ctypes as ct
import mmap
import os
from pathlib import Path
import sys
import traceback

CAPACITY = 1 << 18
HEADER = 128
MAGIC, STOP, STATE, EPOCH, ORIGIN, INPUT_END, CONSUMED, WET_EPOCH, WET_END, PID = range(10)


class Shared:
    def __init__(self, binary, name):
        self.mapping = mmap.mmap(-1, HEADER + 8 * CAPACITY, tagname=name)
        self.addr = ct.addressof(ct.c_char.from_buffer(self.mapping))
        self.dll = ct.CDLL(str(binary))
        self.dll.IwiLoad.argtypes = [ct.c_void_p]
        self.dll.IwiLoad.restype = ct.c_int64
        self.dll.IwiStore.argtypes = [ct.c_void_p, ct.c_int64]
        self.dll.IwiStore.restype = None
        self.win = ct.WinDLL("kernel32", use_last_error=True)
        self.win.OpenEventW.argtypes = [ct.c_uint32, ct.c_int, ct.c_wchar_p]
        self.win.OpenEventW.restype = ct.c_void_p
        self.win.WaitForSingleObject.argtypes = [ct.c_void_p, ct.c_uint32]
        self.win.SetEvent.argtypes = [ct.c_void_p]
        self.input_event = self.win.OpenEventW(0x100000, 0, name + "-input")
        self.output_event = self.win.OpenEventW(2, 0, name + "-output")
        if self.get(MAGIC) != 0x495749333801 or not self.input_event or not self.output_event:
            raise RuntimeError("VST3 IPC protocol mismatch")

    def get(self, key):
        return self.dll.IwiLoad(self.addr + 8 * key)

    def set(self, key, value):
        self.dll.IwiStore(self.addr + 8 * key, value)

    def notify(self):
        self.win.SetEvent(self.output_event)


class RateConverter:
    """Centered band-limited interpolation, carrying filter history and absolute phase."""
    radius = 32

    def __init__(self, np, src, dst):
        self.np, self.src, self.dst = np, src, dst
        self.next_sample = 0
        self.start = -self.radius
        self.samples = np.zeros(self.radius, np.float32)
        self.offsets = np.arange(1 - self.radius, self.radius + 1)
        from math import gcd
        self.phases = min(1024, dst // gcd(src, dst))
        delta = np.arange(self.phases)[:, None] / self.phases - self.offsets
        cutoff = min(1., dst / src) * .96
        window = .5 + .5 * np.cos(np.pi * delta / self.radius)
        taps = cutoff * np.sinc(cutoff * delta) * window
        self.taps = (taps / taps.sum(axis=1, keepdims=True)).astype(np.float32)

    def push(self, x):
        if self.src == self.dst:
            return x
        np = self.np
        self.samples = np.concatenate((self.samples, x))
        limit = self.start + len(self.samples) - self.radius
        end = max(self.next_sample, (limit * self.dst + self.src - 1) // self.src)
        indices = np.arange(self.next_sample, end, dtype=np.int64) * self.src
        centers, remainder = np.divmod(indices, self.dst)
        phases = remainder * self.phases // self.dst
        positions = (centers - self.start)[:, None] + self.offsets
        y = (self.samples[positions] * self.taps[phases]).sum(axis=1)
        self.next_sample = end
        keep = max(self.start, end * self.src // self.dst + 1 - self.radius)
        self.samples = self.samples[keep - self.start:]
        self.start = keep
        return y


def warm_up(model, np):
    """Capture the one- and two-lane CUDA graphs before the first live frame.

    The graphs depend on the number of lanes, not on the window length. With
    a 4-frame window and a 2-frame crossfade, eight frames pass through two
    crossfades and two lane swaps. The model's own 600-frame window needs
    six seconds of audio to reach its first swap. The session is created at
    full size, so its attention buffers return to PyTorch's pool for the live
    sessions.
    """
    session = model.stream()
    window = session.window_frames = 4
    session.crossfade_frames = 2
    session.push(np.zeros(1024, np.float32))
    for _ in range(2 * window - 1):
        session.push(np.zeros(480, np.float32))


def run(shared, root, rate, packaged=False):
    sys.path.insert(0, str(root / "inworld"))
    if packaged:
        import rtx_platform
        rtx_platform.configure(root)
    import numpy as np
    import torch
    if packaged:
        rtx_platform.select_device(torch)
    from streaming_v2 import ComparisonModel

    torch.set_num_threads(4)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cudnn.benchmark = False
    model = ComparisonModel(lookahead=0, variant="ordered", advance_hops=1)
    input_ring = np.ndarray(CAPACITY, dtype=np.float32, buffer=shared.mapping, offset=HEADER)
    output_ring = np.ndarray(CAPACITY, dtype=np.float32, buffer=shared.mapping, offset=HEADER + 4*CAPACITY)
    with torch.inference_mode():
        warm_up(model, np)
        session = model.stream()
        torch.cuda.synchronize()
        shared.set(PID, os.getpid())
        shared.set(STATE, 1)
        shared.notify()
        previous = None
        while not shared.get(STOP):
            epoch = shared.get(EPOCH)
            if not epoch or epoch & 1:
                shared.win.WaitForSingleObject(shared.input_event, 10)
                continue
            if previous != epoch:
                origin = shared.get(ORIGIN)
                if shared.get(EPOCH) != epoch:
                    continue
                if previous is not None and used:
                    session = model.stream()
                used = False
                resample_in, resample_out = RateConverter(np, rate, 48000), RateConverter(np, 48000, rate)
                read = emitted = origin
                pending = np.empty(0, np.float32)
                needed = 1024
                previous = epoch
                shared.set(CONSUMED, read)
                shared.set(WET_END, emitted)
                shared.set(WET_EPOCH, epoch)
            available = shared.get(INPUT_END) - read
            if shared.get(EPOCH) != epoch:
                continue
            if available <= 0:
                shared.win.WaitForSingleObject(shared.input_event, 50)
                continue
            n = min(available, 4096, CAPACITY - read % CAPACITY)
            x = input_ring[read % CAPACITY:read % CAPACITY+n].copy()
            if shared.get(EPOCH) != epoch:
                continue
            read += n
            shared.set(CONSUMED, read)
            pending = np.concatenate((pending, resample_in.push(x)))
            while len(pending) >= needed and shared.get(EPOCH) == epoch and not shared.get(STOP):
                y = resample_out.push(session.push(pending[:needed]))
                pending = pending[needed:]
                needed, used = 480, True
                if len(y):
                    pos = emitted % CAPACITY
                    first = min(len(y), CAPACITY - pos)
                    output_ring[pos:pos+first] = y[:first]
                    output_ring[:len(y)-first] = y[first:]
                    emitted += len(y)
                    shared.set(WET_END, emitted)
                    shared.notify()


if __name__ == "__main__":
    root, binary, name, rate = sys.argv[1:5]
    packaged = sys.argv[5:] == ["--rtx"]
    ipc = Shared(Path(binary), name)
    try:
        run(ipc, Path(root), int(rate), packaged)
    except Exception:
        ipc.set(STATE, -1)
        ipc.notify()
        directory = Path(root) / "logs" if packaged else Path(os.environ["LOCALAPPDATA"]) / "InworldIsolator"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f"worker-{os.getpid()}.log").write_text(traceback.format_exc(), encoding="utf-8")
        raise
