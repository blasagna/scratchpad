"""A nested graph with dataclass payloads: the diagram from ``dfg/README.md``.

Run with ``pixi run imu``.

This is the contract's own example graph, built for real::

    imu_raw --> calib --> [ fusion: predict --> update ] --> overlay --> pose
    frames  ------------------------------------------------^

It demonstrates:

* **payloads as small frozen dataclasses** -- :class:`~examples.nodes.imu.ImuSample`
  and :class:`~examples.nodes.imu.Orientation`;
* **namespacing** -- the nodes inside the subgraph are ``fusion.predict`` and
  ``fusion.update``, so their topics are ``fusion.predict.state`` and
  ``fusion.update.fused``;
* **aliases** -- ``fusion.pose`` is the subgraph's output name and an alias of
  ``fusion.update.fused``. Subscribing to either observes the same port and the same
  messages, which the script checks rather than asserts in prose;
* **graph parameters** -- the subgraph's ``rate_hz`` comes from the root graph's
  ``imu_rate_hz`` through a ``$param`` reference, which is what makes the same
  ``fusion`` blueprint reusable at another rate.

The frames here arrive one per IMU sample. Aligning a 200 Hz IMU against 30 fps
video is a real problem and the framework deliberately does not solve it -- see
``video_pipeline`` for the node that does.
"""

from __future__ import annotations

from dfg.blueprint import GraphBuilder, GraphSpec, ParamRef
from dfg.graph import Graph
from dfg.message import Message, ts_to_seconds
from dfg.registry import Registry
from examples.nodes import core, imu
from examples.synth import synth_imu

SAMPLE_COUNT = 40
RATE_HZ = 200.0


def build_registry() -> Registry:
    """Everything both the root graph and the subgraph need."""
    registry = Registry()
    core.register(registry)
    imu.register(registry)
    return registry


def fusion_blueprint() -> GraphSpec:
    """The ``fusion`` subgraph: a complementary filter in two nodes.

    Its input ``imu`` fans out to both nodes, which is how the document's two
    ``calib.corrected`` arrows are spelled with a single parent edge. Its output
    ``pose`` aliases ``update.fused``.
    """
    builder = GraphBuilder("fusion", params={"rate_hz": RATE_HZ})
    builder.add("predict", imu.Predict, params={"rate_hz": ParamRef("rate_hz")})
    builder.add("update", imu.Update, params={"alpha": 0.05})
    builder.connect("predict.state", "update.state")
    builder.add_input("imu", "predict.imu", "update.imu", type_tag="ImuSample")
    builder.add_output("pose", "update.fused", type_tag="Orientation")
    return builder.build()


def build_blueprint() -> GraphSpec:
    """The whole tracker, with ``fusion`` referenced as a node."""
    builder = GraphBuilder("tracker", params={"imu_rate_hz": RATE_HZ})
    builder.add(
        "calib",
        imu.Calibrate,
        params={
            # Lists rather than tuples: JSON has one sequence type, so a tuple
            # would come back a list and the round-trip would not be exact.
            "accel_bias": [0.05, -0.03, 0.10],
            "gyro_bias": [0.0, 0.0, 0.0],
            "rate_hz": ParamRef("imu_rate_hz"),
        },
    )
    builder.add_subgraph(
        "fusion", fusion_blueprint(), params={"rate_hz": ParamRef("imu_rate_hz")}
    )
    builder.add("overlay", imu.Overlay)
    builder.connect("calib.corrected", "fusion.imu")
    builder.connect("fusion.pose", "overlay.pose")
    builder.add_input("imu_raw", "calib.raw", type_tag="ImuSample")
    builder.add_input("frames", "overlay.frame")
    builder.add_output("pose", "overlay.composited")
    return builder.build()


def main() -> None:
    spec = build_blueprint()
    registry = build_registry()

    via_alias: list[Message] = []
    via_topic: list[Message] = []
    states: list[Message] = []

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
            "Resolved fusion.predict rate_hz: "
            f"{graph.flat.nodes['fusion.predict'].params['rate_hz']} Hz "
            "(from the root graph's imu_rate_hz)"
        )
        print()

        graph.subscribe("fusion.pose", lambda name, m: via_alias.append(m))
        graph.subscribe("fusion.update.fused", lambda name, m: via_topic.append(m))
        graph.subscribe("fusion.predict.state", lambda name, m: states.append(m))

        samples = synth_imu(SAMPLE_COUNT, rate_hz=RATE_HZ)
        for i, message in enumerate(samples):
            graph.inject("imu_raw", message)
            graph.inject("frames", Message(f"frame{i:04d}", message.timestamp))
            graph.run_until_idle()

        composited = graph.poll("pose")
        stats = graph.control.node_stats()

    print(f"Injected {SAMPLE_COUNT} IMU samples and {SAMPLE_COUNT} frames")
    print(f"Produced {len(composited)} composited frames on the 'pose' output")
    print()
    print("Firings per node")
    for qid, node_stats in stats.items():
        print(f"  {qid:<20} {node_stats.fired}")
    print()

    print("The alias and the topic observed the same messages:")
    print(f"  fusion.pose saw          {len(via_alias)} messages")
    print(f"  fusion.update.fused saw  {len(via_topic)} messages")
    print(f"  identical:               {via_alias == via_topic}")
    print()

    print("Fused orientation, every eighth sample (predicted vs fused roll)")
    print(f"  {'t (s)':>8}  {'predicted':>10}  {'fused':>10}  {'|accel|':>8}")
    for predicted, fused in list(zip(states, via_alias))[::8]:
        print(
            f"  {ts_to_seconds(fused.timestamp):8.3f}  "
            f"{predicted.payload.roll:+10.5f}  {fused.payload.roll:+10.5f}  "
            f"{fused.payload.accel_magnitude:8.4f}"
        )
    print()
    print("Both estimates track the synthetic 0.20 rad/s roll; the fused one is")
    print("pulled towards the accelerometer, so it does not drift with the gyro.")


if __name__ == "__main__":
    main()
