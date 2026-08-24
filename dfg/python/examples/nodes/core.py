"""Generic plumbing nodes. Standard library only.

These are the nodes that carry the parts of the contract nothing domain-specific
should have to re-derive: decimation is the zero-output case, windowing is the
many-output case, and merge is what you use instead of wiring two producers into
one input port.

They are also the reference for the typed node form -- parameters as annotated
class attributes, input ports as ``run``'s keyword-only parameters, output ports
as the fields of a nested ``Out``.
"""

from __future__ import annotations

from typing import Annotated, Any, NamedTuple

from dfg.errors import ParamError
from dfg.message import Message
from dfg.node import Emit, In, Node
from dfg.ports import Port
from dfg.registry import Registry


class Passthrough(Node):
    """Forwards its input unchanged. Useful as a graph's named boundary."""

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, inp: In[Any] = ()) -> Out:
        return self.Out(output=inp)


class Decimate(Node):
    """Forwards every ``factor``-th message and drops the rest.

    The zero-output case: on ``factor - 1`` firings out of ``factor``, ``run``
    returns nothing at all. An API that returned one value per output port could not
    express this without a side channel.
    """

    factor: int = 2
    phase: int = 0

    class Out(NamedTuple):
        output: Emit[Any]

    def __post_init__(self) -> None:
        if self.factor < 1:
            raise ParamError(f"factor must be at least 1, got {self.factor}")

    def setup(self) -> None:
        self._seen = 0

    def run(self, *, inp: In[Any] = ()) -> Out:
        phase = self.phase % self.factor
        kept: list[Message[Any]] = []
        for message in inp:
            if self._seen % self.factor == phase:
                kept.append(message)
            self._seen += 1
        return self.Out(output=tuple(kept))


class Window(Node):
    """Frames a stream into fixed-size windows, optionally overlapping.

    The many-output case: a hop smaller than the size means one firing can complete
    several windows, and a firing that does not complete one emits nothing.

    Attributes:
        size: Messages per window.
        hop: How far the window advances between emissions. ``hop < size`` overlaps.
    """

    size: int
    hop: int

    class Out(NamedTuple):
        output: Annotated[Emit[tuple[Message[Any], ...]], Port("window")]

    def __post_init__(self) -> None:
        if self.size < 1 or self.hop < 1:
            raise ParamError("size and hop must both be at least 1")

    def setup(self) -> None:
        self._buffer: list[Message[Any]] = []
        self._skip = 0

    def run(self, *, inp: In[Any] = ()) -> Out:
        size, hop = self.size, self.hop
        windows: list[Message[Any]] = []
        for message in inp:
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
        return self.Out(output=tuple(windows))


class Merge(Node):
    """Interleaves two inputs onto one output, oldest sample time first.

    This is what an input port's one-writer rule pushes you towards, and it is
    better than the alternative: two producers sharing a queue would order messages
    by which node the scheduler happened to fire, and this orders them by the sample
    time they actually carry.
    """

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, a: In[Any] = (), b: In[Any] = ()) -> Out:
        merged = [*a, *b]
        merged.sort(key=lambda message: message.timestamp)
        return self.Out(output=tuple(merged))


class Resample(Node):
    """Holds the newest value from ``fast`` and pairs it with each ``slow`` message.

    The contract says the framework does not align time: matching a 200 Hz signal
    against 30 fps video is done by an ordinary node you write. This is that node --
    zero-order hold, the simplest useful version. It emits nothing until a fast
    sample has arrived, which is another reason zero-or-more is the contract.

    **Wire this with ``AnyInput`` readiness.** Under the default ``all`` it would
    wait for a slow message before accepting a fast one, which is precisely backwards
    for a hold: the fast stream has to be free to run ahead. That the right rule
    here is not the default is the point of readiness being a per-node policy chosen
    where the node is wired -- the node itself cannot know how it will be used.
    """

    class Out(NamedTuple):
        output: Emit[tuple[Any, Any]]

    def setup(self) -> None:
        self._held: Message[Any] | None = None

    def run(self, *, slow: In[Any] = (), fast: In[Any] = ()) -> Out | None:
        for message in fast:
            self._held = message
        if self._held is None:
            return None
        held = self._held
        return self.Out(
            output=tuple(
                message.with_payload((message.payload, held.payload))
                for message in slow
            )
        )


class Counter(Node):
    """Counts messages and emits the running total."""

    class Out(NamedTuple):
        output: Annotated[Emit[int], Port("count")]

    def setup(self) -> None:
        self._count = 0

    def run(self, *, inp: In[Any] = ()) -> Out:
        out: list[Message[int]] = []
        for message in inp:
            self._count += 1
            out.append(message.with_payload(self._count))
        return self.Out(output=tuple(out))


class Recorder(Node):
    """Appends every message it sees to a list, and forwards it unchanged.

    The list is passed in as a parameter, so a script or test can read what the
    graph carried without subscribing. A tap does the same job without touching the
    blueprint -- this exists to show that a node is allowed to be boring.
    """

    sink: list[Message[Any]] | None = None

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, inp: In[Any] = ()) -> Out:
        if self.sink is not None:
            self.sink.extend(inp)
        return self.Out(output=inp)


class Trace(Node):
    """Appends ``(label, phase)`` to a shared list on every lifecycle call.

    What ``lifecycle_demo`` prints. ``label`` has no default on purpose: a trace of
    unlabelled nodes is not worth printing.
    """

    trace: list[tuple[str, str]] | None = None
    label: str

    class Out(NamedTuple):
        output: Emit[Any]

    def setup(self) -> None:
        self._record("setup")

    def run(self, *, inp: In[Any] = ()) -> Out:
        self._record("run")
        return self.Out(output=inp)

    def teardown(self) -> None:
        self._record("teardown")

    def _record(self, phase: str) -> None:
        if self.trace is not None:
            self.trace.append((self.label, phase))


class FailSetup(Trace):
    """Raises in ``setup``, so the graph fails to start.

    Traces its own ``setup`` first, so the demo can show that the call was made and
    the matching ``teardown`` still never happens.
    """

    def setup(self) -> None:
        self._record("setup")
        raise RuntimeError(f"{self.label}: could not acquire the device")


class FailRun(Trace):
    """Raises in ``run`` for payloads its policy should reject.

    Fails on any payload that is falsy, so a demo can send a mix and watch the error
    policy decide. Its ``run`` is annotated ``-> Trace.Out`` because a class body's
    scope is not inherited, so a bare ``Out`` would not resolve here.
    """

    def run(self, *, inp: In[Any] = ()) -> Trace.Out:
        self._record("run")
        for message in inp:
            if not message.payload:
                raise ValueError(f"{self.label}: bad sample {message.payload!r}")
        return self.Out(output=inp)


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
