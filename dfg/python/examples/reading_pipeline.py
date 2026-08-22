"""A nested graph with dataclass payloads, applying simple per-field operations.

Run with ``pixi run reading``.

This is the contract's own example graph, built for real::

    readings --> scale --> [ classify: flag --> grade ] --> relabel --> result
    tags     -----------------------------------------------^

It demonstrates:

* **payloads as small frozen dataclasses of primitive fields** --
  :class:`~examples.nodes.reading.Reading` carries a float, a bool, a str, and a
  :class:`~examples.nodes.reading.Level` enum, and each node touches one of them;
* **namespacing** -- the nodes inside the subgraph are ``classify.flag`` and
  ``classify.grade``, so their topics are ``classify.flag.flagged`` and
  ``classify.grade.graded``;
* **aliases** -- ``classify.classified`` is the subgraph's output name and an alias of
  ``classify.grade.graded``. Subscribing to either observes the same port and the same
  messages, which the script checks rather than asserts in prose;
* **graph parameters** -- the subgraph's ``threshold`` comes from the root graph's
  ``active_threshold`` through a ``$param`` reference, which is what makes the same
  ``classify`` blueprint reusable at another cutoff.

A second graph input, ``tags``, arrives one per reading. Folding a slow side channel
into a fast stream is a real problem and the framework deliberately does not solve it --
see ``video_pipeline`` for the node that aligns two different rates.
"""

from __future__ import annotations

from dfg.blueprint import GraphBuilder, GraphSpec, ParamRef
from dfg.graph import Graph
from dfg.message import Message, ts_to_seconds
from dfg.registry import Registry
from examples.nodes import core, reading
from examples.synth import synth_readings

SAMPLE_COUNT = 40
ACTIVE_THRESHOLD = 1.0


def build_registry() -> Registry:
    """Everything both the root graph and the subgraph need."""
    registry = Registry()
    core.register(registry)
    reading.register(registry)
    return registry


def classify_blueprint() -> GraphSpec:
    """The ``classify`` subgraph: flag then grade, in two nodes.

    Its input ``reading`` fans out to both nodes, which is how ``grade`` sees the same
    reading that ``flag`` did. Its output ``classified`` aliases ``grade.graded``.
    """
    builder = GraphBuilder("classify", params={"threshold": ACTIVE_THRESHOLD})
    builder.add("flag", reading.Threshold, params={"threshold": ParamRef("threshold")})
    builder.add("grade", reading.Grade, params={"low": 1.0, "high": 2.0})
    builder.connect("flag.flagged", "grade.flagged")
    builder.add_input("reading", "flag.reading", "grade.reading", type_tag="Reading")
    builder.add_output("classified", "grade.graded", type_tag="Reading")
    return builder.build()


def build_blueprint() -> GraphSpec:
    """The whole processor, with ``classify`` referenced as a node."""
    builder = GraphBuilder("processor", params={"active_threshold": ACTIVE_THRESHOLD})
    builder.add("scale", reading.Scale, params={"gain": 2.0, "offset": 0.0})
    builder.add_subgraph(
        "classify",
        classify_blueprint(),
        params={"threshold": ParamRef("active_threshold")},
    )
    builder.add("relabel", reading.Relabel)
    builder.connect("scale.scaled", "classify.reading")
    builder.connect("classify.classified", "relabel.reading")
    builder.add_input("readings", "scale.raw", type_tag="Reading")
    builder.add_input("tags", "relabel.tag")
    builder.add_output("result", "relabel.labeled")
    return builder.build()


def main() -> None:
    spec = build_blueprint()
    registry = build_registry()

    via_alias: list[Message] = []
    via_topic: list[Message] = []
    flags: list[Message] = []

    with Graph.instantiate(spec, registry) as graph:
        print("Topics (one per output port, namespaced by the enclosing subgraph)")
        for topic in graph.topics:
            print(f"  {topic}")
        print()
        print("Aliases (a graph output name, resolved within its own graph)")
        for alias in graph.aliases:
            endpoint = graph.flat.aliases[alias]
            print(f"  {alias:<24} -> {endpoint[0]}.{endpoint[1]}")
        print()
        print(f"Qualified IDs in canonical order: {list(graph.order)}")
        print(
            "Resolved classify.flag threshold: "
            f"{graph.flat.nodes['classify.flag'].params['threshold']} "
            "(from the root graph's active_threshold)"
        )
        print()

        graph.subscribe("classify.classified", lambda name, m: via_alias.append(m))
        graph.subscribe("classify.grade.graded", lambda name, m: via_topic.append(m))
        graph.subscribe("classify.flag.flagged", lambda name, m: flags.append(m))

        readings = synth_readings(SAMPLE_COUNT)
        for i, message in enumerate(readings):
            graph.inject("readings", message)
            graph.inject("tags", Message(f"tag{i:04d}", message.timestamp))
            graph.run_until_idle()

        results = graph.poll("result")
        stats = graph.control.node_stats()

    print(f"Injected {SAMPLE_COUNT} readings and {SAMPLE_COUNT} tags")
    print(f"Produced {len(results)} labelled readings on the 'result' output")
    print()
    print("Firings per node")
    for qid, node_stats in stats.items():
        print(f"  {qid:<20} {node_stats.fired}")
    print()

    print("The alias and the topic observed the same messages:")
    print(f"  classify.classified saw    {len(via_alias)} messages")
    print(f"  classify.grade.graded saw  {len(via_topic)} messages")
    print(f"  identical:                 {via_alias == via_topic}")
    print()

    active_count = sum(1 for message in results if message.payload.active)
    print(
        f"Readings marked active (value >= {ACTIVE_THRESHOLD} after scaling): {active_count}"
    )
    print()

    print("Classified readings, every eighth sample")
    print(f"  {'t (s)':>8}  {'value':>8}  {'active':>7}  {'level':>7}  {'label':>16}")
    for message in results[::8]:
        reading_payload = message.payload
        print(
            f"  {ts_to_seconds(message.timestamp):8.3f}  "
            f"{reading_payload.value:8.3f}  {str(reading_payload.active):>7}  "
            f"{reading_payload.level.name:>7}  {reading_payload.label:>16}"
        )
    print()
    print("Each node touched one field: scale doubled the value, flag set active from")
    print("the threshold, grade chose the level, and relabel folded the tag into the")
    print("label. The level and flag agree because both read the same scaled value.")


if __name__ == "__main__":
    main()
