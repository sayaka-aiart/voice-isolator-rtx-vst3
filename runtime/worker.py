"""Local Windows worker for one 48 kHz Inworld Isolator 38 VST3 instance."""
from contextlib import closing
import ctypes as ct
import gc
import mmap
import os
from pathlib import Path
import sys
import time
import traceback

CAPACITY = 1 << 18
HEADER = 128
MAGIC, STOP, STATE, EPOCH, ORIGIN, INPUT_END, CONSUMED, WET_EPOCH, WET_END, PID, ADVANCE_MODE = range(11)
ADVANCE_HOPS = (1.0, 1.5, 2.0)  # Default, 5 ms earlier, 10 ms earlier at a 480-sample hop.
IDLE_BEFORE_SPARE = 0.1  # Seconds without audio before spare frames start.
SPARE_PERIOD = 0.006  # Seconds between spare frames. They are lighter than live ones, so they run more often.
SPARE_LIMIT = 600  # Seconds without audio after which the GPU is left to rest.


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
        if self.get(MAGIC) != 0x495749333802 or not self.input_event or not self.output_event:
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


def raise_priority():
    """Schedule the worker's CPU time ahead of ordinary applications.

    A frame has a few audio blocks to reach the plug-in, and at normal
    priority a busy machine can keep the worker waiting longer than that.
    Priority 15 is the highest below the real-time range, so the host's
    audio threads still run first. The GPU scheduling class keeps its
    default: its realtime class shortened the typical frame beside another
    GPU load but added stalls of 20 to 38 ms.
    """
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = kernel.GetCurrentThread.restype = ct.c_void_p
    kernel.SetPriorityClass.argtypes = [ct.c_void_p, ct.c_uint32]
    kernel.SetThreadPriority.argtypes = [ct.c_void_p, ct.c_int]
    kernel.SetPriorityClass(kernel.GetCurrentProcess(), 0x80)  # HIGH_PRIORITY_CLASS
    kernel.SetThreadPriority(kernel.GetCurrentThread(), 15)  # THREAD_PRIORITY_TIME_CRITICAL


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


class GpuActivity:
    """Share the latest real work and elect one idle worker per CUDA UUID and Windows session."""

    def __init__(self, shared, device_uuid):
        self.dll, self.win = shared.dll, shared.win
        self.mutex = None
        self.keeper = False
        self.store_latest = getattr(self.dll, "IwiStoreMax", None)
        if device_uuid is None or self.store_latest is None:
            return

        name = rf"Local\VoiceIsolatorRTX-GpuWarm-v1-{device_uuid}"
        self.store_latest.argtypes = [ct.c_void_p, ct.c_int64]
        self.store_latest.restype = None
        self.win.CreateMutexW.argtypes = [ct.c_void_p, ct.c_int, ct.c_wchar_p]
        self.win.CreateMutexW.restype = ct.c_void_p
        self.win.ReleaseMutex.argtypes = self.win.CloseHandle.argtypes = [ct.c_void_p]
        mutex = self.win.CreateMutexW(None, False, name + "-keeper")
        if not mutex:
            return
        try:
            # Both plug-ins store a monotonic timestamp in nanoseconds in this one word.
            self.mapping = mmap.mmap(-1, 8, tagname=name + "-activity")
        except OSError:
            self.win.CloseHandle(mutex)
            return  # Host restrictions can prevent sharing. Keep the local idle policy.
        self.address = ct.addressof(ct.c_char.from_buffer(self.mapping))
        self.mutex = mutex

    def release_keeper(self):
        if self.keeper:
            self.win.ReleaseMutex(self.mutex)
            self.keeper = False

    def mark_active(self, now):
        """Publish actual GPU work, including startup warm-up, to other instances."""
        if self.mutex:
            self.store_latest(self.address, round(now * 1_000_000_000))
            self.release_keeper()

    def should_warm(self, now, heard):
        if self.mutex and now - heard >= IDLE_BEFORE_SPARE:
            heard = max(heard, self.dll.IwiLoad(self.address) / 1_000_000_000)
        if not IDLE_BEFORE_SPARE <= now - heard < SPARE_LIMIT:
            self.release_keeper()
            return False
        if self.mutex and not self.keeper:
            # A worker that exits while holding this mutex leaves it available to the next waiter.
            result = self.win.WaitForSingleObject(self.mutex, 0)
            self.keeper = result in (0, 0x80)  # WAIT_OBJECT_0 or WAIT_ABANDONED
        return not self.mutex or self.keeper

    def close(self):
        if self.mutex:
            self.release_keeper()
            self.win.CloseHandle(self.mutex)
            self.mapping.close()
            self.mutex = None


class KeepWarm:
    """Run frames nobody hears while no audio arrives, so the GPU keeps its working speed.

    A GPU with nothing to do lowers its clocks within seconds. On an RTX 4070
    the first frames after five idle seconds took 25 to 70 ms instead of
    2.5 ms, for about a quarter of a second. A host that stops calling the
    plug-in during silence then loses the start of the next phrase. Light work
    does not prevent this, and neither do frames of the model at a slow pace:
    one every 50 ms left the next live frames at 20 ms. The spare frames run
    on a session of their own with a short attention window (76 MiB), which
    makes them lighter than live frames. One every 10 ms still let the clock
    fall to 950 MHz and doubled the time of the next live frames. One every
    6 ms holds about 2200 MHz, and live frames resume at their usual time.
    The live session's output stays bit-identical. The GPU draws about 42 W
    this way, against 49 W with live audio and 21 to 28 W at rest, so the
    spare frames stop after ten minutes without audio.
    """

    def __init__(self, model, np, activity):
        self.activity = activity
        # A short window needs no full-size attention buffers.
        preallocate, model.preallocate = model.preallocate, False
        try:
            self.session = model.stream()
        finally:
            model.preallocate = preallocate
        self.session.window_frames, self.session.crossfade_frames = 32, 2
        self.session.push(np.zeros(1024, np.float32))
        self.silence = np.zeros(480, np.float32)
        self.heard = self.due = time.perf_counter()
        self.activity.mark_active(self.due)
        # Short waits end on time instead of at the next 15.6 ms system tick.
        ct.WinDLL("winmm").timeBeginPeriod(1)

    def wait(self, shared, milliseconds):
        """Wait this long for audio at most, or run a spare frame if one is due."""
        now = time.perf_counter()
        if self.activity.should_warm(now, self.heard):
            if now >= self.due:
                self.session.push(self.silence)
                self.due += SPARE_PERIOD
                if self.due <= now:
                    # Skip missed ticks when entering idle or resuming after a stall.
                    self.due = now + SPARE_PERIOD
                return
            milliseconds = min(milliseconds, max(1, round((self.due - now) * 1000)))
        shared.win.WaitForSingleObject(shared.input_event, milliseconds)


def run(shared, root, rate, packaged=False):
    raise_priority()
    sys.path.insert(0, str(root / "inworld"))
    if packaged:
        # The installed engine holds sources only and the plug-in starts it
        # with -B. Compile each module once into the engine's cache folder,
        # which the uninstaller removes, and load the compiled copies after.
        sys.pycache_prefix = str(root / "cache" / "pycache")
        sys.dont_write_bytecode = False
        import rtx_platform
        rtx_platform.configure(root)
    import numpy as np
    import torch
    if packaged:
        rtx_platform.select_device(torch)
    from streaming_v2 import ComparisonModel
    sys.path.append(str(Path(__file__).resolve().parent))
    from frame_graphs import ChainedSections

    torch.set_num_threads(4)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cudnn.benchmark = False
    model = ComparisonModel(lookahead=0, variant="ordered", advance_hops=ADVANCE_HOPS[0])
    model.sections = ChainedSections(model.base.vi.model, model.optimized_bands)
    input_ring = np.ndarray(CAPACITY, dtype=np.float32, buffer=shared.mapping, offset=HEADER)
    output_ring = np.ndarray(CAPACITY, dtype=np.float32, buffer=shared.mapping, offset=HEADER + 4*CAPACITY)
    device = torch.cuda.get_device_properties(torch.cuda.current_device())
    with torch.inference_mode(), closing(GpuActivity(shared, getattr(device, "uuid", None))) as activity:
        warm_up(model, np)
        session = model.stream()
        spare = KeepWarm(model, np, activity)
        torch.cuda.synchronize()
        # A full garbage collection walks every object created while loading and stops
        # this thread for tens of milliseconds. Later collections skip the frozen objects,
        # and sessions are still released by their reference counts.
        gc.freeze()
        shared.set(PID, os.getpid())
        shared.set(STATE, 1)
        shared.notify()
        previous = None
        used = False
        while not shared.get(STOP):
            epoch = shared.get(EPOCH)
            if not epoch or epoch & 1:
                spare.wait(shared, 10)
                continue
            if previous != epoch:
                origin = shared.get(ORIGIN)
                mode = shared.get(ADVANCE_MODE)
                if shared.get(EPOCH) != epoch:
                    continue
                if not 0 <= mode < len(ADVANCE_HOPS):
                    raise ValueError(f"Unsupported synthesis advance mode {mode}")
                if used or model.advance_hops != ADVANCE_HOPS[mode]:
                    model.advance_hops = ADVANCE_HOPS[mode]
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
                spare.wait(shared, 50)
                continue
            spare.heard = time.perf_counter()
            n = min(available, 4096, CAPACITY - read % CAPACITY)
            x = input_ring[read % CAPACITY:read % CAPACITY+n].copy()
            if shared.get(EPOCH) != epoch:
                continue
            read += n
            shared.set(CONSUMED, read)
            pending = np.concatenate((pending, resample_in.push(x)))
            while len(pending) >= needed and shared.get(EPOCH) == epoch and not shared.get(STOP):
                activity.mark_active(time.perf_counter())
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
