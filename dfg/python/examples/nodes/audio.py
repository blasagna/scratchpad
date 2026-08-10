"""Audio nodes over numpy arrays.

A payload here is a 1-D ``float32`` array of samples. Nothing in the framework knows
that -- a node may use any library available in the environment, and this module is
where that permission is exercised.

Audio is the clearest case for zero-or-more outputs: framing a stream of blocks into
overlapping analysis windows emits several windows from one block and sometimes none
at all, and an API that returned one value per output port could express neither.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, ClassVar

import numpy as np

from dfg.errors import ParamError
from dfg.message import Message
from dfg.node import REQUIRED, Inputs, Node, Outputs
from dfg.ports import PortSpec
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

    INPUTS = (PortSpec("input", type_tag=AUDIO_BLOCK),)
    OUTPUTS = (PortSpec("output", type_tag=AUDIO_WINDOW),)
    PARAMS: ClassVar[Mapping[str, Any]] = {
        "size": REQUIRED,
        "hop": REQUIRED,
        "sample_rate": 16_000.0,
    }

    def __init__(self, **params: Any) -> None:
        super().__init__(**params)
        if self.params["size"] < 1 or self.params["hop"] < 1:
            raise ParamError("size and hop must both be at least 1")
        if self.params["hop"] > self.params["size"]:
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

    def run(self, inputs: Inputs) -> Outputs:
        size, hop = self.params["size"], self.params["hop"]
        rate = self.params["sample_rate"]
        windows: list[Message[np.ndarray]] = []
        for message in inputs.get("input", ()):
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
        return {"output": windows}


class Hann(Node):
    """Applies a Hann window in place of a rectangular one.

    Tapering the ends is what stops a tone that does not fit a whole number of cycles
    in the window from smearing across the whole spectrum.
    """

    INPUTS = (PortSpec("input", type_tag=AUDIO_WINDOW),)
    OUTPUTS = (PortSpec("output", type_tag=AUDIO_WINDOW),)

    def setup(self) -> None:
        self._window: np.ndarray | None = None

    def run(self, inputs: Inputs) -> Outputs:
        out: list[Message[np.ndarray]] = []
        for message in inputs.get("input", ()):
            samples = np.asarray(message.payload, dtype=np.float32)
            if self._window is None or self._window.size != samples.size:
                self._window = np.hanning(samples.size).astype(np.float32)
            out.append(message.with_payload(samples * self._window))
        return {"output": out}


class Rms(Node):
    """Root-mean-square level of a window, in dB relative to full scale."""

    INPUTS = (PortSpec("input", type_tag=AUDIO_WINDOW),)
    OUTPUTS = (PortSpec("output", type_tag="db"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"floor_db": -120.0}

    def run(self, inputs: Inputs) -> Outputs:
        floor = self.params["floor_db"]
        out: list[Message[float]] = []
        for message in inputs.get("input", ()):
            samples = np.asarray(message.payload, dtype=np.float64)
            rms = float(np.sqrt(np.mean(np.square(samples))))
            db = 20.0 * np.log10(rms) if rms > 0.0 else floor
            out.append(message.with_payload(max(float(db), floor)))
        return {"output": out}


class Spectrum(Node):
    """Magnitude spectrum of a real window, via ``numpy.fft.rfft``."""

    INPUTS = (PortSpec("input", type_tag=AUDIO_WINDOW),)
    OUTPUTS = (PortSpec("output", type_tag=SPECTRUM),)

    def run(self, inputs: Inputs) -> Outputs:
        out: list[Message[np.ndarray]] = []
        for message in inputs.get("input", ()):
            samples = np.asarray(message.payload, dtype=np.float32)
            out.append(message.with_payload(np.abs(np.fft.rfft(samples))))
        return {"output": out}


class PeakBin(Node):
    """Reports the loudest frequency in a magnitude spectrum.

    Ignores bin 0 (DC), which any real signal with an offset would otherwise win with.
    """

    INPUTS = (PortSpec("input", type_tag=SPECTRUM),)
    OUTPUTS = (PortSpec("output", type_tag="hz"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {
        "sample_rate": 16_000.0,
        "window_size": REQUIRED,
    }

    def run(self, inputs: Inputs) -> Outputs:
        rate = self.params["sample_rate"]
        size = self.params["window_size"]
        out: list[Message[float]] = []
        for message in inputs.get("input", ()):
            magnitudes = np.asarray(message.payload)
            if magnitudes.size < 2:
                continue
            index = int(np.argmax(magnitudes[1:])) + 1
            out.append(message.with_payload(index * rate / size))
        return {"output": out}


class Pack(Node):
    """Combines an RMS level and a peak frequency into one summary tuple.

    Two inputs with ``all`` readiness, both fed from the same window stream, so they
    stay in step with no help from the scheduler -- which is the whole trick to
    keeping a fan-out/fan-in pair aligned without a time model.
    """

    INPUTS = (PortSpec("level", type_tag="db"), PortSpec("peak", type_tag="hz"))
    OUTPUTS = (PortSpec("output", type_tag="audio_summary"),)

    def run(self, inputs: Inputs) -> Outputs:
        pairs = zip(inputs.get("level", ()), inputs.get("peak", ()))
        return {
            "output": [
                level.with_payload((round(level.payload, 3), round(peak.payload, 2)))
                for level, peak in pairs
            ]
        }


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(Frame)
    registry.register(Hann)
    registry.register(Rms)
    registry.register(Spectrum)
    registry.register(PeakBin)
    registry.register(Pack)
    return registry
