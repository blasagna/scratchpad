"""Stdlib-only fixtures shared by the test modules.

``readme_example_spec`` is the graph drawn in ``dfg/README.md``, built once here
and reused by the flatten, mermaid, serialize, and determinism tests, so the
document and the code cannot drift apart without a test noticing.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Annotated, Any, NamedTuple

from dfg.blueprint import GraphBuilder, GraphSpec
from dfg.message import Message
from dfg.node import Emit, In, Inputs, Node, Outputs
from dfg.ports import Port, PortSpec
from dfg.registry import Registry


class FakeClock:
    """A monotone counter standing in for a wall clock, so latency is exact."""

    def __init__(self, start: int = 0, step: int = 1) -> None:
        self.now = start
        self.step = step

    def __call__(self) -> int:
        value = self.now
        self.now += self.step
        return value

    def advance(self, ticks: int) -> None:
        self.now += ticks


def first_payload(messages: Sequence[Message[Any]]) -> Any:
    """The payload of the first message, for terse assertions."""
    return messages[0].payload


def payloads(messages: Sequence[Message[Any]]) -> list[Any]:
    """Every payload, in order."""
    return [message.payload for message in messages]


def digest(messages: Sequence[Message[Any]]) -> list[tuple[Any, int]]:
    """Payload and timestamp per message -- what a replay comparison should use.

    Comparing payloads alone would let a run that lost the sample times pass.
    """
    return [(message.payload, message.timestamp) for message in messages]


# --- Node fixtures -----------------------------------------------------------
#
# Most of these use the typed node form. The ones that stay on the declared form
# do so on purpose, and each says why: together they are the standing check that
# the two forms still work side by side in one registry and one graph.


class Passthrough(Node):
    """One in, one out, unchanged."""

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, input: In[Any] = ()) -> Out:
        return self.Out(output=input)


class Double(Node):
    """Doubles a numeric payload, keeping the sample time."""

    class Out(NamedTuple):
        output: Annotated[Emit[int], Port("number")]

    def run(self, *, input: Annotated[In[int], Port("number")] = ()) -> Out:
        return self.Out(output=tuple(m.with_payload(m.payload * 2) for m in input))


class Sum2(Node):
    """Adds one message from each of two ports. The `all`-readiness workhorse."""

    class Out(NamedTuple):
        output: Annotated[Emit[int], Port("number")]

    def run(
        self,
        *,
        a: Annotated[In[int], Port("number")] = (),
        b: Annotated[In[int], Port("number")] = (),
    ) -> Out:
        total = sum(msg.payload for msg in a) + sum(msg.payload for msg in b)
        newest = max(msg.timestamp for msg in (*a, *b))
        return self.Out(output=(Message(total, newest),))


class EmitN(Node):
    """Emits ``n`` messages per firing. The many-outputs case.

    Declared form on purpose: the fixture that keeps a ``PARAMS`` mapping in play,
    so the registry and validate paths that read one stay exercised.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS = {"n": 3}

    def run(self, inputs: Inputs) -> Outputs:
        out: list[Message[Any]] = []
        for msg in inputs.get("input", ()):
            out.extend(
                msg.with_payload((msg.payload, i)) for i in range(self.params["n"])
            )
        return {"output": out}


class EmitNothing(Node):
    """Consumes and emits nothing. The zero-outputs case."""

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, input: In[Any] = ()) -> Out | None:
        del input
        return None


class EmitEmptyMapping(Node):
    """Returns ``{}``, which must mean the same as ``None``.

    Declared form necessarily: an empty mapping is not something an ``Out`` can be.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return {}


class EmitEmptyPort(Node):
    """Returns an ``Out`` whose only port is empty -- also nothing produced."""

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, input: In[Any] = ()) -> Out:
        del input
        return self.Out(output=())


class ReturnBareMessage(Node):
    """Breaks the output contract by returning a message instead of a mapping.

    Declared form necessarily: this is what ``normalize_outputs`` has to reject.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        return next(iter(inputs["input"]))  # type: ignore[return-value]


class ReturnUnknownPort(Node):
    """Breaks the output contract by naming a port it did not declare.

    Declared form necessarily: an ``Out``'s fields *are* the declared ports, so a
    typed node cannot name one it does not have.
    """

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        return {"nope": list(inputs.get("input", ()))}


class NoInputs(Node):
    """A free-running source, which the contract does not allow.

    Declared form, to keep the rejection about validation rather than about which
    form the author happened to write.
    """

    INPUTS = ()
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return None


class RaiseInSetup(Node):
    """``setup`` raises, so the graph must fail to start."""

    trace: list[tuple[str, str]] | None = None
    label: str = "raiser"

    class Out(NamedTuple):
        output: Emit[Any]

    def setup(self) -> None:
        self._record("setup")
        raise RuntimeError("setup failed on purpose")

    def run(self, *, input: In[Any] = ()) -> Out | None:
        del input
        return None

    def teardown(self) -> None:
        self._record("teardown")

    def _record(self, phase: str) -> None:
        _record(self.trace, self.label, phase)


class RaiseInRun(Node):
    """``run`` raises, so the node's error policy decides what happens."""

    trace: list[tuple[str, str]] | None = None
    label: str = "failing"

    class Out(NamedTuple):
        output: Emit[Any]

    def setup(self) -> None:
        self._record("setup")

    def run(self, *, input: In[Any] = ()) -> Out:
        del input
        self._record("run")
        raise ValueError("run failed on purpose")

    def teardown(self) -> None:
        self._record("teardown")

    def _record(self, phase: str) -> None:
        _record(self.trace, self.label, phase)


class TraceNode(Node):
    """Appends ``(label, phase)`` to a shared list on every lifecycle call."""

    trace: list[tuple[str, str]] | None = None
    label: str = "node"

    class Out(NamedTuple):
        output: Emit[Any]

    def setup(self) -> None:
        self._record("setup")

    def run(self, *, input: In[Any] = ()) -> Out:
        self._record("run")
        return self.Out(output=input)

    def teardown(self) -> None:
        self._record("teardown")

    def _record(self, phase: str) -> None:
        _record(self.trace, self.label, phase)


class ParamWatcher(Node):
    """Records the value of a live-changeable parameter it saw on each firing."""

    gain: int = 1
    seen: list[int] | None = None
    changes: list[dict[str, Any]] | None = None
    MUTABLE_PARAMS = frozenset({"gain"})

    class Out(NamedTuple):
        output: Emit[Any]

    def run(self, *, input: In[Any] = ()) -> Out:
        out: list[Message[Any]] = []
        for msg in input:
            if self.seen is not None:
                self.seen.append(self.gain)
            out.append(msg.with_payload(msg.payload * self.gain))
        return self.Out(output=tuple(out))

    def on_params_changed(self, changes: Mapping[str, Any]) -> None:
        if self.changes is not None:
            self.changes.append(dict(changes))


def _record(trace: list[tuple[str, str]] | None, label: str, phase: str) -> None:
    if trace is not None:
        trace.append((label, phase))


def make_node_type(
    inputs: Sequence[str],
    outputs: Sequence[str],
    *,
    name: str = "Generated",
) -> type[Node]:
    """Build a passthrough node class with the given port names.

    Used for the README example graph, where the port names carry the meaning and
    the computation does not.

    Declared form necessarily, and the clearest case for why that form has to stay:
    the port names are runtime data, so there is no signature to read them from.
    """

    class Generated(Node):
        INPUTS = tuple(PortSpec(port) for port in inputs)
        OUTPUTS = tuple(PortSpec(port) for port in outputs)

        def run(self, node_inputs: Inputs) -> Outputs:  # type: ignore[override]
            newest = max(
                (msg.timestamp for msgs in node_inputs.values() for msg in msgs),
                default=0,
            )
            payload = tuple(
                sorted(
                    (port, msg.payload)
                    for port, msgs in node_inputs.items()
                    for msg in msgs
                )
            )
            return {port.name: [Message(payload, newest)] for port in Generated.OUTPUTS}

    Generated.__name__ = name
    Generated.__qualname__ = name
    return Generated


Calib = make_node_type(["raw"], ["corrected"], name="Calib")
Predict = make_node_type(["imu"], ["state"], name="Predict")
Update = make_node_type(["imu", "state"], ["fused"], name="Update")
Overlay = make_node_type(["frame", "pose"], ["composited"], name="Overlay")


def build_registry() -> Registry:
    """A registry holding every stdlib test fixture."""
    registry = Registry()
    registry.register(Passthrough)
    registry.register(Double)
    registry.register(Sum2)
    registry.register(EmitN)
    registry.register(EmitNothing)
    registry.register(EmitEmptyMapping)
    registry.register(EmitEmptyPort)
    registry.register(ReturnBareMessage)
    registry.register(ReturnUnknownPort)
    registry.register(NoInputs)
    registry.register(RaiseInSetup)
    registry.register(RaiseInRun)
    registry.register(TraceNode)
    registry.register(ParamWatcher)
    # The README example's node types. Only the port names matter.
    registry.register(Calib)
    registry.register(Predict)
    registry.register(Update)
    registry.register(Overlay)
    return registry


# --- The README's example graph ----------------------------------------------


def fusion_subgraph_spec() -> GraphSpec:
    """The ``fusion`` subgraph from ``README.md``.

    Its input ``imu`` fans out to both inner nodes, which is how the document's
    two ``calib.corrected`` arrows are spelled with one parent edge. Its output
    ``pose`` aliases ``update.fused``.
    """
    builder = GraphBuilder("fusion")
    builder.add("predict", Predict)
    builder.add("update", Update)
    builder.connect("predict.state", "update.state")
    builder.add_input("imu", "predict.imu", "update.imu")
    builder.add_output("pose", "update.fused")
    return builder.build()


def readme_example_spec() -> GraphSpec:
    """The whole graph drawn in ``README.md`` -> Naming and namespacing.

    Topics: ``calib.corrected``, ``fusion.predict.state``, ``fusion.update.fused``,
    ``overlay.composited``; aliases ``fusion.pose`` and the root output ``pose``.
    """
    builder = GraphBuilder("tracker", params={"imu_rate_hz": 200.0})
    builder.add("calib", Calib)
    builder.add_subgraph("fusion", fusion_subgraph_spec())
    builder.add("overlay", Overlay)
    builder.connect("calib.corrected", "fusion.imu")
    builder.connect("fusion.pose", "overlay.pose")
    builder.add_input("imu_raw", "calib.raw", type_tag="ImuSample")
    builder.add_input("frames", "overlay.frame")
    builder.add_output("pose", "overlay.composited")
    return builder.build()


def scheduling_spec(trace: list | None = None, *, side_priority: int = 0) -> GraphSpec:
    """A graph where `topological` and `level` genuinely disagree.

    ``head`` feeds a three-node chain and a single side node::

        head -> chain1 -> chain2 -> chain3
             -> side

    After ``chain1`` fires, both ``chain2`` (topological index 2, level 2) and
    ``side`` (index 4, level 1) are ready. Topological takes ``chain2``; level takes
    ``side``. A diamond would not show this -- both orderings agree there.
    """
    builder = GraphBuilder("scheduling")
    for node_id in ("head", "chain1", "chain2", "chain3"):
        builder.add(node_id, TraceNode, params={"trace": trace, "label": node_id})
    builder.add(
        "side",
        TraceNode,
        params={"trace": trace, "label": "side"},
        priority=side_priority,
    )
    builder.connect("head.output", "chain1.input")
    builder.connect("chain1.output", "chain2.input")
    builder.connect("chain2.output", "chain3.input")
    builder.connect("head.output", "side.input")
    builder.add_input("source", "head.input")
    builder.add_output("chained", "chain3.output")
    builder.add_output("aside", "side.output")
    return builder.build()


def chain_spec(*, labels: Sequence[str] = ("a", "b", "c"), trace: list | None = None):
    """A straight chain of :class:`TraceNode`\\ s, for lifecycle-order tests."""
    builder = GraphBuilder("chain")
    for label in labels:
        builder.add("n_" + label, TraceNode, params={"trace": trace, "label": label})
    for left, right in zip(labels, labels[1:]):
        builder.connect(f"n_{left}.output", f"n_{right}.input")
    builder.add_input("source", f"n_{labels[0]}.input")
    builder.add_output("sink", f"n_{labels[-1]}.output")
    return builder.build()
