"""Scheduling is two axes, not one: readiness per node, ordering per graph.

Run with ``pixi run schedule``.

The four policies this design started from are points in one two-dimensional space.
"React to all inputs available" and "react to any inputs available" are *readiness
rules*; "topologically sorted" and "graph-level traversal" are *orderings*. Keeping
them on one list made them look mutually exclusive, and they are not -- ``all`` +
``topological`` and ``all`` + ``level`` are both sensible and behave differently.

The two axes are demonstrated separately, because they differ in kind:

* **Ordering** changes *when* nodes fire and not what comes out. Swapping it is a
  policy change, which is what makes determinism worth requiring.
* **Readiness** changes what a firing *means*, so it does change the output. That is
  not a bug in either rule; it is the choice the rule exists to make.
"""

from __future__ import annotations

from dfg.blueprint import GraphBuilder, GraphSpec
from dfg.graph import Graph
from dfg.message import Message
from dfg.readiness import AllInputs, AnyInput, ReadinessRule
from dfg.registry import Registry
from examples.nodes import core

RECORDING = [Message(i, i * 1_000_000) for i in range(6)]


def build_registry() -> Registry:
    return core.register(Registry())


def ordering_blueprint(trace: list, *, side_priority: int = 0) -> GraphSpec:
    """A graph where ``topological`` and ``level`` genuinely disagree::

        head -> chain1 -> chain2 -> chain3 -> join
             -> side ----------------------> join

    After ``chain1`` fires, ``chain2`` is next in the topological sort but ``side``
    sits at a shallower level, so the two orderings pick differently. A diamond
    would not show this: both orderings agree there.
    """
    builder = GraphBuilder("ordering")
    for node_id in ("head", "chain1", "chain2", "chain3", "side"):
        builder.add(
            node_id,
            core.Trace,
            params={"trace": trace, "label": node_id},
            priority=side_priority if node_id == "side" else 0,
        )
    builder.add("join", core.Merge)
    builder.connect("head.out", "chain1.in")
    builder.connect("chain1.out", "chain2.in")
    builder.connect("chain2.out", "chain3.in")
    builder.connect("head.out", "side.in")
    builder.connect("chain3.out", "join.a")
    builder.connect("side.out", "join.b")
    builder.add_input("source", "head.in")
    builder.add_output("merged", "join.out")
    return builder.build()


def readiness_blueprint(rule: ReadinessRule) -> GraphSpec:
    """A graph whose two branches deliver at different rates::

        head -> thin (1 of every 3) -> join.a
             -> side ---------------> join.b

    ``all`` makes ``join`` wait for the slow branch, so two of every three side
    messages sit in a queue. ``any`` makes it fire on whichever arrived. Both are
    correct; they answer different questions.
    """
    builder = GraphBuilder("readiness")
    builder.add("head", core.Passthrough)
    builder.add("thin", core.Decimate, params={"factor": 3, "phase": 2})
    builder.add("side", core.Passthrough)
    builder.add("join", core.Merge, readiness=rule)
    builder.connect("head.out", "thin.in")
    builder.connect("head.out", "side.in")
    builder.connect("thin.out", "join.a")
    builder.connect("side.out", "join.b")
    builder.add_input("source", "head.in")
    builder.add_output("merged", "join.out")
    return builder.build()


def run(spec: GraphSpec, ordering: str = "topological") -> dict:
    """Fire ``spec`` over the recording. Returns what happened."""
    with Graph.instantiate(spec, build_registry(), ordering=ordering) as graph:
        for message in RECORDING:
            graph.inject("source", message)
            graph.run_until_idle()
        digest = [(m.payload, m.timestamp) for m in graph.poll("merged")]
        return {
            "digest": digest,
            "join_fired": graph.control.node_stats()["join"].fired,
            "left_pending": graph.control.total_pending(),
        }


def show_orderings() -> None:
    print("Axis 1: ordering -- when nodes fire")
    print("=" * 74)
    digests = []
    for label, ordering, priority in (
        ("topological", "topological", 0),
        ("level", "level", 0),
        ("priority (side=10)", "priority", 10),
    ):
        traces = []
        for _ in range(2):
            trace: list[tuple[str, str]] = []
            result = run(ordering_blueprint(trace, side_priority=priority), ordering)
            traces.append([name for name, phase in trace if phase == "run"])
        print(f"\n  {label}")
        print(f"    first pass:  {' -> '.join(traces[0][:6])} ...")
        print(f"    repeatable:  {traces[0] == traces[1]}")
        print(f"    outputs:     {len(result['digest'])} messages")
        digests.append(result["digest"])

    print()
    print(
        f"  All three orderings produced identical output: {digests.count(digests[0]) == 3}"
    )
    print("  Ties break by qualified node ID, in one place, so a repeat cannot")
    print("  differ and neither can a reordered blueprint.")


def show_readiness() -> None:
    print()
    print("Axis 2: readiness -- what a firing means")
    print("=" * 74)
    results = {}
    for label, rule in (("all", AllInputs()), ("any", AnyInput())):
        results[label] = run(readiness_blueprint(rule))
        print(f"\n  {label}")
        print(f"    join fired:    {results[label]['join_fired']} times")
        print(f"    outputs:       {len(results[label]['digest'])} messages")
        print(f"    left in queue: {results[label]['left_pending']}")

    print()
    print("  'all' waits for the slow branch, so the fast branch backs up. 'any'")
    print("  fires on whatever arrived and leaves nothing waiting. This axis does")
    print("  change the output -- that is the point of choosing.")


def main() -> None:
    show_orderings()
    show_readiness()


if __name__ == "__main__":
    main()
