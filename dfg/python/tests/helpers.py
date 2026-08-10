"""Stdlib-only fixtures shared by the test modules.

``readme_example_spec`` is the graph drawn in ``dfg/README.md``, built once here
and reused by the flatten, mermaid, serialize, and determinism tests, so the
document and the code cannot drift apart without a test noticing.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from dfg.blueprint import GraphBuilder, GraphSpec
from dfg.message import Message
from dfg.node import Inputs, Node, Outputs
from dfg.ports import PortSpec
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


class Passthrough(Node):
    """One in, one out, unchanged."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        return {"output": list(inputs.get("input", ()))}


class Double(Node):
    """Doubles a numeric payload, keeping the sample time."""

    INPUTS = (PortSpec("input", type_tag="number"),)
    OUTPUTS = (PortSpec("output", type_tag="number"),)

    def run(self, inputs: Inputs) -> Outputs:
        return {
            "output": [
                msg.with_payload(msg.payload * 2) for msg in inputs.get("input", ())
            ]
        }


class Sum2(Node):
    """Adds one message from each of two ports. The `all`-readiness workhorse."""

    INPUTS = (PortSpec("a", type_tag="number"), PortSpec("b", type_tag="number"))
    OUTPUTS = (PortSpec("output", type_tag="number"),)

    def run(self, inputs: Inputs) -> Outputs:
        a = inputs.get("a", ())
        b = inputs.get("b", ())
        total = sum(msg.payload for msg in a) + sum(msg.payload for msg in b)
        newest = max(msg.timestamp for msg in (*a, *b))
        return {"output": [Message(total, newest)]}


class EmitN(Node):
    """Emits ``n`` messages per firing. The many-outputs case."""

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

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return None


class EmitEmptyMapping(Node):
    """Returns ``{}``, which must mean the same as ``None``."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return {}


class EmitEmptyPort(Node):
    """Returns ``{"output": ()}``, which must also mean nothing was produced."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return {"output": ()}


class ReturnBareMessage(Node):
    """Breaks the output contract by returning a message instead of a mapping."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        return next(iter(inputs["input"]))  # type: ignore[return-value]


class ReturnUnknownPort(Node):
    """Breaks the output contract by naming a port it did not declare."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        return {"nope": list(inputs.get("input", ()))}


class NoInputs(Node):
    """A free-running source, which the contract does not allow."""

    INPUTS = ()
    OUTPUTS = (PortSpec("output"),)

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return None


class RaiseInSetup(Node):
    """``setup`` raises, so the graph must fail to start."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS = {"trace": None, "label": "raiser"}

    def setup(self) -> None:
        _record(self.params, "setup")
        raise RuntimeError("setup failed on purpose")

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        return None

    def teardown(self) -> None:
        _record(self.params, "teardown")


class RaiseInRun(Node):
    """``run`` raises, so the node's error policy decides what happens."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS = {"trace": None, "label": "failing"}

    def setup(self) -> None:
        _record(self.params, "setup")

    def run(self, inputs: Inputs) -> Outputs:
        del inputs
        _record(self.params, "run")
        raise ValueError("run failed on purpose")

    def teardown(self) -> None:
        _record(self.params, "teardown")


class TraceNode(Node):
    """Appends ``(label, phase)`` to a shared list on every lifecycle call."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS = {"trace": None, "label": "node"}

    def setup(self) -> None:
        _record(self.params, "setup")

    def run(self, inputs: Inputs) -> Outputs:
        _record(self.params, "run")
        return {"output": list(inputs.get("input", ()))}

    def teardown(self) -> None:
        _record(self.params, "teardown")


class ParamWatcher(Node):
    """Records the value of a live-changeable parameter it saw on each firing."""

    INPUTS = (PortSpec("input"),)
    OUTPUTS = (PortSpec("output"),)
    PARAMS = {"gain": 1, "seen": None, "changes": None}
    MUTABLE_PARAMS = frozenset({"gain"})

    def run(self, inputs: Inputs) -> Outputs:
        seen = self.params.get("seen")
        gain = self.params["gain"]
        out: list[Message[Any]] = []
        for msg in inputs.get("input", ()):
            if seen is not None:
                seen.append(gain)
            out.append(msg.with_payload(msg.payload * gain))
        return {"output": out}

    def on_params_changed(self, changes: Mapping[str, Any]) -> None:
        recorded = self.params.get("changes")
        if recorded is not None:
            recorded.append(dict(changes))


def _record(params: Mapping[str, Any], phase: str) -> None:
    trace = params.get("trace")
    if trace is not None:
        trace.append((params.get("label"), phase))


def make_node_type(
    inputs: Sequence[str],
    outputs: Sequence[str],
    *,
    name: str = "Generated",
) -> type[Node]:
    """Build a passthrough node class with the given port names.

    Used for the README example graph, where the port names carry the meaning and
    the computation does not.
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
