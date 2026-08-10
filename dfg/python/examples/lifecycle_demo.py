"""The three-call lifecycle, including what happens when ``setup`` raises.

Run with ``pixi run lifecycle``.

Two graphs. The first starts cleanly and shows the order: ``setup`` in topological
order, ``run`` per firing, ``teardown`` in *reverse* topological order, so a node
tears down before anything it depends on. The second has a middle node whose
``setup`` raises, and shows the part of the contract that is easiest to get wrong:

* the graph fails to start;
* the raising node does **not** get a ``teardown`` -- it never finished acquiring,
  so it has no business being asked to release;
* every node already set up *does* get one;
* the node after it gets neither, because it never ran ``setup``.
"""

from __future__ import annotations

from dfg.blueprint import GraphBuilder, GraphSpec
from dfg.errors import NodeSetupError
from dfg.graph import Graph
from dfg.message import Message
from dfg.registry import Registry
from examples.nodes import core


def build_registry() -> Registry:
    """A registry with the nodes these two graphs need."""
    return core.register(Registry())


def build_blueprint(trace: list, *, failing: bool = False) -> GraphSpec:
    """A three-node chain. ``failing`` makes the middle node raise in ``setup``."""
    builder = GraphBuilder("lifecycle")
    builder.add("first", core.Trace, params={"trace": trace, "label": "first"})
    builder.add(
        "middle",
        core.FailSetup if failing else core.Trace,
        params={"trace": trace, "label": "middle"},
    )
    builder.add("last", core.Trace, params={"trace": trace, "label": "last"})
    builder.connect("first.out", "middle.in")
    builder.connect("middle.out", "last.in")
    builder.add_input("source", "first.in")
    builder.add_output("sink", "last.out")
    return builder.build()


def show(trace: list[tuple[str, str]]) -> None:
    """Print a trace as one line per lifecycle call."""
    for label, phase in trace:
        print(f"    {phase:<9} {label}")


def phases_of(trace: list[tuple[str, str]], phase: str) -> list[str]:
    """Which labels reached ``phase``, in order."""
    return [label for label, seen in trace if seen == phase]


def run_clean() -> tuple[list[tuple[str, str]], int]:
    """Start, fire twice, stop. Returns the trace and the output count."""
    trace: list[tuple[str, str]] = []
    registry = build_registry()
    produced = 0
    with Graph.instantiate(build_blueprint(trace), registry) as graph:
        for i in range(2):
            graph.inject("source", Message(f"sample{i}", i * 1_000_000))
            graph.run_until_idle()
        produced = len(graph.poll("sink"))
    return trace, produced


def run_failing() -> tuple[list[tuple[str, str]], str]:
    """Try to start a graph whose middle node cannot. Returns the trace and error."""
    trace: list[tuple[str, str]] = []
    registry = build_registry()
    graph = Graph.instantiate(build_blueprint(trace, failing=True), registry)
    try:
        graph.start()
    except NodeSetupError as exc:
        return trace, str(exc)
    raise AssertionError("the failing graph was expected not to start")


def main() -> None:
    print("A clean run: setup in topological order, teardown in reverse")
    print("-" * 70)
    clean, produced = run_clean()
    show(clean)
    print()
    print(f"  produced:       {produced} messages on 'sink'")
    print(f"  setup order:    {phases_of(clean, 'setup')}")
    print(f"  teardown order: {phases_of(clean, 'teardown')}  <- exactly reversed")

    print()
    print("A graph whose middle node raises in setup")
    print("-" * 70)
    failing, error = run_failing()
    show(failing)
    print()
    print(f"  start() raised:  {error}")
    print(f"  setup reached:   {phases_of(failing, 'setup')}")
    print(f"  teardown given:  {phases_of(failing, 'teardown')}")
    print()
    print("  'middle' raised, so it got no teardown: it never finished acquiring.")
    print("  'last' never ran setup, so it got neither call.")
    print("  'first' was already set up, so it was released.")


if __name__ == "__main__":
    main()
