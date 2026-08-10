"""Audio nodes over numpy arrays.

A payload here is a 1-D ``float32`` array of samples. Nothing in the framework knows
that -- a node may use any library available in the environment, and this module is
where that permission is exercised.

Audio is the clearest case for zero-or-more outputs: framing a stream of blocks into
overlapping analysis windows emits several windows from one block and sometimes none
at all, and an API that returned one value per output port could express neither.
"""

from __future__ import annotations

from typing import Annotated, NamedTuple

import numpy as np

from dfg.errors import ParamError
from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import Port
from dfg.registry import Registry

AUDIO_BLOCK = "audio_block"
AUDIO_WINDOW = "audio_window"
SPECTRUM = "spectrum"


class Frame(Node):
    """Reframes a stream of blocks into fixed-size, optionally overlapping windows.

    Blocks arrive at whatever size the capture produced; analysis wants a fixed size
    with a fixed hop. This node buffers samples and emits every window that completes,
    which for a 512-sample block reframed at 256/128 is three windows per block.
    """

    size: int
    hop: int
    sample_rate: float = 16_000.0

    class Out(NamedTuple):
        output: Annotated[Emit[np.ndarray], Port(AUDIO_WINDOW)]

    def __post_init__(self) -> None:
        if self.size < 1 or self.hop < 1:
            raise ParamError("size and hop must both be at least 1")
        if self.hop > self.size:
            raise ParamError(
                "hop must not exceed size: a larger hop would discard samples, "
                "which is decimation and belongs in its own node"
            )

    def setup(self) -> None:
        self._buffer = np.zeros(0, dtype=np.float32)
        # The sample index of buffer[0], so a window's timestamp is its own first
        # sample's capture time rather than the arrival time of the block that
        # happened to complete it.
        self._origin = 0
        self._have_origin = False

    def run(self, *, inp: Annotated[In[np.ndarray], Port(AUDIO_BLOCK)] = ()) -> Out:
        size, hop = self.size, self.hop
        rate = self.sample_rate
        windows: list[Message[np.ndarray]] = []
        for message in inp:
            block = np.asarray(message.payload, dtype=np.float32)
            if not self._have_origin:
                self._origin = round(message.timestamp * rate / 1_000_000_000)
                self._have_origin = True
            self._buffer = np.concatenate([self._buffer, block])
            while self._buffer.size >= size:
                window = self._buffer[:size].copy()
                timestamp = round(self._origin * 1_000_000_000 / rate)
                windows.append(Message(window, timestamp))
                self._buffer = self._buffer[hop:]
                self._origin += hop
        return self.Out(output=tuple(windows))


class Hann(Node):
    """Applies a Hann window in place of a rectangular one.

    Tapering the ends is what stops a tone that does not fit a whole number of cycles
    in the window from smearing across the whole spectrum.
    """

    class Out(NamedTuple):
        output: Annotated[Emit[np.ndarray], Port(AUDIO_WINDOW)]

    def setup(self) -> None:
        self._window: np.ndarray | None = None

    def run(self, *, inp: Annotated[In[np.ndarray], Port(AUDIO_WINDOW)] = ()) -> Out:
        out: list[Message[np.ndarray]] = []
        for message in inp:
            samples = np.asarray(message.payload, dtype=np.float32)
            if self._window is None or self._window.size != samples.size:
                self._window = np.hanning(samples.size).astype(np.float32)
            out.append(message.with_payload(samples * self._window))
        return self.Out(output=tuple(out))


class Rms(Node):
    """Root-mean-square level of a window, in dB relative to full scale."""

    floor_db: float = -120.0

    class Out(NamedTuple):
        output: Annotated[Emit[float], Port("db")]

    def run(self, *, inp: Annotated[In[np.ndarray], Port(AUDIO_WINDOW)] = ()) -> Out:
        floor = self.floor_db
        out: list[Message[float]] = []
        for message in inp:
            samples = np.asarray(message.payload, dtype=np.float64)
            rms = float(np.sqrt(np.mean(np.square(samples))))
            db = 20.0 * np.log10(rms) if rms > 0.0 else floor
            out.append(message.with_payload(max(float(db), floor)))
        return self.Out(output=tuple(out))


class Spectrum(Node):
    """Magnitude spectrum of a real window, via ``numpy.fft.rfft``."""

    class Out(NamedTuple):
        output: Annotated[Emit[np.ndarray], Port(SPECTRUM)]

    def run(self, *, inp: Annotated[In[np.ndarray], Port(AUDIO_WINDOW)] = ()) -> Out:
        out: list[Message[np.ndarray]] = []
        for message in inp:
            samples = np.asarray(message.payload, dtype=np.float32)
            out.append(message.with_payload(np.abs(np.fft.rfft(samples))))
        return self.Out(output=tuple(out))


class PeakBin(Node):
    """Reports the loudest frequency in a magnitude spectrum.

    Ignores bin 0 (DC), which any real signal with an offset would otherwise win with.
    """

    sample_rate: float = 16_000.0
    window_size: int

    class Out(NamedTuple):
        output: Annotated[Emit[float], Port("hz")]

    def run(self, *, inp: Annotated[In[np.ndarray], Port(SPECTRUM)] = ()) -> Out:
        rate = self.sample_rate
        size = self.window_size
        out: list[Message[float]] = []
        for message in inp:
            magnitudes = np.asarray(message.payload)
            if magnitudes.size < 2:
                continue
            index = int(np.argmax(magnitudes[1:])) + 1
            out.append(message.with_payload(index * rate / size))
        return self.Out(output=tuple(out))


class Pack(Node):
    """Combines an RMS level and a peak frequency into one summary tuple.

    Two inputs with ``all`` readiness, both fed from the same window stream, so they
    stay in step with no help from the scheduler -- which is the whole trick to
    keeping a fan-out/fan-in pair aligned without a time model.
    """

    class Out(NamedTuple):
        output: Annotated[Emit[tuple[float, float]], Port("audio_summary")]

    def run(
        self,
        *,
        level: Annotated[In[float], Port("db")] = (),
        peak: Annotated[In[float], Port("hz")] = (),
    ) -> Out:
        return self.Out(
            output=tuple(
                level_message.with_payload(
                    (round(level_message.payload, 3), round(peak_message.payload, 2))
                )
                for level_message, peak_message in zip(level, peak)
            )
        )


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(Frame)
    registry.register(Hann)
    registry.register(Rms)
    registry.register(Spectrum)
    registry.register(PeakBin)
    registry.register(Pack)
    return registry
