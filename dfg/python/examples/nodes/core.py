"""Generic plumbing nodes. Standard library only.

These are the nodes that carry the parts of the contract nothing domain-specific
should have to re-derive: decimation is the zero-output case, windowing is the
many-output case, and merge is what you use instead of wiring two producers into
one input port.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, ClassVar

from dfg.errors import ParamError
from dfg.message import Message
from dfg.node import REQUIRED, Inputs, Node, Outputs
from dfg.ports import PortSpec
from dfg.registry import Registry


class Passthrough(Node):
    """Forwards its input unchanged. Useful as a graph's named boundary."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        return {"output": list(inputs.get("input", ()))}


class Decimate(Node):
    """Forwards every ``factor``-th message and drops the rest.

    The zero-output case: on ``factor - 1`` firings out of ``factor``, ``run``
    returns nothing at all. An API that returned one value per output port could not
    express this without a side channel.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"factor": 2, "phase": 0}

    def __init__(self, **params: Any) -> None:
        super().__init__(**params)
        if self.params["factor"] < 1:
            raise ParamError(f"factor must be at least 1, got {self.params['factor']}")

    def setup(self) -> None:
        self._seen = 0

    def run(self, inputs: Inputs) -> Outputs:
        factor = self.params["factor"]
        phase = self.params["phase"] % factor
        kept: list[Message[Any]] = []
        for message in inputs.get("input", ()):
            if self._seen % factor == phase:
                kept.append(message)
            self._seen += 1
        return {"output": kept}


class Window(Node):
    """Frames a stream into fixed-size windows, optionally overlapping.

    The many-output case: a hop smaller than the size means one firing can complete
    several windows, and a firing that does not complete one emits nothing.

    Attributes:
        size: Messages per window.
        hop: How far the window advances between emissions. ``hop < size`` overlaps.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output", type_tag="window"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"size": REQUIRED, "hop": REQUIRED}

    def __init__(self, **params: Any) -> None:
        super().__init__(**params)
        if self.params["size"] < 1 or self.params["hop"] < 1:
            raise ParamError("size and hop must both be at least 1")

    def setup(self) -> None:
        self._buffer: list[Message[Any]] = []
        self._skip = 0

    def run(self, inputs: Inputs) -> Outputs:
        size, hop = self.params["size"], self.params["hop"]
        windows: list[Message[Any]] = []
        for message in inputs.get("input", ()):
            if self._skip:
                # Only reachable when hop > size, where the frames have gaps.
                self._skip -= 1
                continue
            self._buffer.append(message)
            if len(self._buffer) < size:
                continue
            # The window's sample time is its *first* sample: that is when the data
            # it describes was captured.
            window = tuple(self._buffer)
            windows.append(Message(window, window[0].timestamp))
            if hop >= size:
                self._buffer.clear()
                self._skip = hop - size
            else:
                del self._buffer[:hop]
        return {"output": windows}


class Merge(Node):
    """Interleaves two inputs onto one output, oldest sample time first.

    This is what an input port's one-writer rule pushes you towards, and it is
    better than the alternative: two producers sharing a queue would order messages
    by which node the scheduler happened to fire, and this orders them by the sample
    time they actually carry.
    """

    INPUTS = (PortSpec("a"), PortSpec("b"))
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        merged = [*inputs.get("a", ()), *inputs.get("b", ())]
        merged.sort(key=lambda message: message.timestamp)
        return {"output": merged}


class Resample(Node):
    """Holds the newest value from ``fast`` and pairs it with each ``slow`` message.

    The contract says the framework does not align time: matching a 200 Hz IMU
    against 30 fps video is done by an ordinary node you write. This is that node --
    zero-order hold, the simplest useful version. It emits nothing until a fast
    sample has arrived, which is another reason zero-or-more is the contract.

    **Wire this with ``AnyInput`` readiness.** Under the default ``all`` it would
    wait for a slow message before accepting a fast one, which is precisely backwards
    for a hold: the fast stream has to be free to run ahead. That the right rule
    here is not the default is the point of readiness being a per-node policy chosen
    where the node is wired -- the node itself cannot know how it will be used.
    """

    INPUTS = (PortSpec("slow"), PortSpec("fast"))
    OUTPUTS = (PortSpec("output"),)

    def setup(self) -> None:
        self._held: Message[Any] | None = None

    def run(self, inputs: Inputs) -> Outputs:
        for message in inputs.get("fast", ()):
            self._held = message
        if self._held is None:
            return None
        held = self._held
        return {
            "output": [
                message.with_payload((message.payload, held.payload))
                for message in inputs.get("slow", ())
            ]
        }


class Counter(Node):
    """Counts messages and emits the running total."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output", type_tag="count"),)

    def setup(self) -> None:
        self._count = 0

    def run(self, inputs: Inputs) -> Outputs:
        out: list[Message[int]] = []
        for message in inputs.get("input", ()):
            self._count += 1
            out.append(message.with_payload(self._count))
        return {"output": out}


class Recorder(Node):
    """Appends every message it sees to a list, and forwards it unchanged.

    The list is passed in as a parameter, so a script or test can read what the
    graph carried without subscribing. A tap does the same job without touching the
    blueprint -- this exists to show that a node is allowed to be boring.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"sink": None}

    def run(self, inputs: Inputs) -> Outputs:
        sink = self.params["sink"]
        messages = list(inputs.get("input", ()))
        if sink is not None:
            sink.extend(messages)
        return {"output": messages}


class Trace(Node):
    """Appends ``(label, phase)`` to a shared list on every lifecycle call.

    What ``lifecycle_demo`` prints. ``label`` defaults to nothing useful on purpose:
    a trace of unlabelled nodes is not worth printing.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS: ClassVar[Mapping[str, Any]] = {"trace": None, "label": REQUIRED}

    def setup(self) -> None:
        self._record("setup")

    def run(self, inputs: Inputs) -> Outputs:
        self._record("run")
        return {"output": list(inputs.get("input", ()))}

    def teardown(self) -> None:
        self._record("teardown")

    def _record(self, phase: str) -> None:
        trace = self.params["trace"]
        if trace is not None:
            trace.append((self.params["label"], phase))


class FailSetup(Trace):
    """Raises in ``setup``, so the graph fails to start.

    Traces its own ``setup`` first, so the demo can show that the call was made and
    the matching ``teardown`` still never happens.
    """

    def setup(self) -> None:
        self._record("setup")
        raise RuntimeError(f"{self.params['label']}: could not acquire the device")


class FailRun(Trace):
    """Raises in ``run`` for payloads its policy should reject.

    Fails on any payload that is falsy, so a demo can send a mix and watch the error
    policy decide.
    """

    def run(self, inputs: Inputs) -> Outputs:
        self._record("run")
        for message in inputs.get("input", ()):
            if not message.payload:
                raise ValueError(
                    f"{self.params['label']}: bad sample {message.payload!r}"
                )
        return {"output": list(inputs.get("input", ()))}


def register(registry: Registry) -> Registry:
    """Register every node in this module and return ``registry``."""
    registry.register(Passthrough)
    registry.register(Decimate)
    registry.register(Window)
    registry.register(Merge)
    registry.register(Resample)
    registry.register(Counter)
    registry.register(Recorder)
    registry.register(Trace)
    registry.register(FailSetup)
    registry.register(FailRun)
    return registry
