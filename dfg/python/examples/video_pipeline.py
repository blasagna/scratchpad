"""Video and IMU at different rates, aligned by a node rather than by the scheduler.

Run with ``pixi run video``.

This is the case the contract is most pointed about. A 200 Hz IMU and a 30 fps camera
do not line up, and **the framework does not align time**: no watermarks, no lateness,
no event-time windows. Matching the two streams is done by an ordinary node you
write::

    imu_raw --> calib --> fusion --> hold.fast -\\
    frames  --> decimate 2:1 ------> hold.slow --> hold -> overlay -> gray -> stats

``hold`` is :class:`examples.nodes.core.Resample`, a zero-order hold wired with
``any`` readiness so the fast stream can run ahead of the slow one. That readiness
rule is not the default, and it has to be chosen here rather than by the node,
because the node cannot know how it will be wired.

Also on show: temporal decimation as the **zero-output** case (two firings in three
produce nothing), numpy ``uint8`` frames as payloads, and a subgraph reused from
``imu_pipeline``.
"""

from __future__ import annotations

import numpy as np

from dfg.blueprint import GraphBuilder, GraphSpec, ParamRef
from dfg.graph import Graph
from dfg.message import Message
from dfg.readiness import AnyInput
from dfg.registry import Registry
from examples.imu_pipeline import fusion_blueprint
from examples.nodes import core, imu, video
from examples.synth import synth_imu
from examples.synth_media import synth_frames

IMU_RATE_HZ = 200.0
FPS = 30.0
FRAME_COUNT = 12
DECIMATE = 2
WIDTH, HEIGHT = 64, 48


def build_registry() -> Registry:
    registry = Registry()
    core.register(registry)
    imu.register(registry)
    video.register(registry)
    return registry


def build_blueprint() -> GraphSpec:
    """The tracker with a real video branch."""
    builder = GraphBuilder("video_tracker", params={"imu_rate_hz": IMU_RATE_HZ})
    builder.add(
        "calib",
        "imu.calibrate",
        params={
            "accel_bias": [0.05, -0.03, 0.10],
            "gyro_bias": [0.0, 0.0, 0.0],
            "rate_hz": ParamRef("imu_rate_hz"),
        },
    )
    builder.add_subgraph(
        "fusion", fusion_blueprint(), params={"rate_hz": ParamRef("imu_rate_hz")}
    )
    builder.add("thin", "core.decimate", params={"factor": DECIMATE})
    # `any`, not the default `all`: a hold must accept a pose with no frame waiting.
    builder.add("hold", "core.resample", readiness=AnyInput())
    builder.add("overlay", "video.overlay_box", params={"size": 6})
    builder.add("gray", "video.to_gray")
    builder.add("stats", "video.frame_stats")

    builder.connect("calib.corrected", "fusion.imu")
    builder.connect("fusion.pose", "hold.fast")
    builder.connect("thin.out", "hold.slow")
    builder.connect("hold.out", "overlay.in")
    builder.connect("overlay.out", "gray.in")
    builder.connect("gray.out", "stats.in")

    builder.add_input("imu_raw", "calib.raw", type_tag="ImuSample")
    builder.add_input("frames", "thin.in", type_tag=video.FRAME_RGB)
    builder.add_output("composited", "overlay.out")
    builder.add_output("stats", "stats.out")
    return builder.build()


def interleave(frames, samples):
    """Merge two timestamped streams into injection order, oldest sample time first.

    This is what a recorder would have written down, and injecting in this order is
    what makes the demo resemble a real capture rather than a contrived one.
    """
    recording = [("frames", message) for message in frames]
    recording += [("imu_raw", message) for message in samples]
    recording.sort(key=lambda pair: (pair[1].timestamp, pair[0]))
    return recording


def main() -> None:
    frames = synth_frames(FRAME_COUNT, width=WIDTH, height=HEIGHT, fps=FPS)
    duration_s = FRAME_COUNT / FPS
    samples = synth_imu(round(duration_s * IMU_RATE_HZ), rate_hz=IMU_RATE_HZ)
    recording = interleave(frames, samples)

    print(
        f"{len(frames)} frames at {FPS:.0f} fps and {len(samples)} IMU samples at "
        f"{IMU_RATE_HZ:.0f} Hz, over {duration_s:.2f} s"
    )
    print(
        f"frame payload: {type(frames[0].payload).__name__} "
        f"{frames[0].payload.dtype} {frames[0].payload.shape}"
    )
    print(f"injections in timestamp order: {len(recording)}")
    print()

    composited: list[Message[np.ndarray]] = []
    stats: list[Message[video.FrameStats]] = []
    poses: list[Message] = []

    with Graph.instantiate(build_blueprint(), build_registry()) as graph:
        graph.subscribe("fusion.pose", lambda name, m: poses.append(m))
        for name, message in recording:
            graph.inject(name, message)
            graph.run_until_idle()
        composited.extend(graph.poll("composited"))
        stats.extend(graph.poll("stats"))
        node_stats = dict(graph.control.node_stats())
        kept_frames = graph.control.edge_stats()["thin.out -> hold.slow"].enqueued

    print("Rate mismatch, handled by a node")
    print("=" * 74)
    print(f"  poses produced by fusion:        {len(poses)}")
    print(f"  frames in:                       {len(frames)}")
    print(f"  frames after {DECIMATE}:1 decimation:     {kept_frames}")
    print(f"  composited frames out:           {len(composited)}")
    print()
    print(
        f"  The hold discarded {len(poses) - len(composited)} poses: it keeps only the"
    )
    print("  newest, and the frame decides when to look. One decimated frame was")
    print("  itself dropped -- it arrived at t=0, before any pose existed, and a hold")
    print("  with nothing held emits nothing.")
    print()

    print("Decimation: the zero-output case")
    print("=" * 74)
    print(
        f"  `thin` fired {node_stats['thin'].fired} times and emitted {kept_frames} frames"
    )
    print(
        f"  {DECIMATE - 1} firing(s) in {DECIMATE} return nothing at all -- no "
        "sentinel, no empty frame,"
    )
    print("  just an empty mapping. That is why `run` returns zero or more.")
    print()

    print("Frame statistics, after the overlay")
    print("=" * 74)
    print(
        f"  {'t (s)':>8}  {'mean luma':>10}  {'peak':>5}  {'bright px':>9}  {'held roll':>9}"
    )
    for stat, frame in zip(stats, composited):
        held = max(
            (p for p in poses if p.timestamp <= frame.timestamp),
            key=lambda p: p.timestamp,
        )
        print(
            f"  {stat.timestamp / 1e9:8.4f}  {stat.payload.mean:10.4f}  "
            f"{stat.payload.peak:5d}  {stat.payload.bright_pixels:9d}  "
            f"{held.payload.roll:+9.4f}"
        )
    print()
    print("  The luma columns are constant on purpose: the synthetic box only moves,")
    print("  so the bright area never changes. The 'held roll' column is the pose the")
    print("  hold was carrying when each frame arrived, and it advances with time --")
    print("  that is the alignment working.")
    print()

    first, last = composited[0].payload, composited[-1].payload
    changed = int(np.count_nonzero(np.any(first != last, axis=2)))
    print(f"  first and last composited frames differ in {changed} pixels")
    print(f"  frame shape and dtype survived the graph: {last.shape} {last.dtype}")
    print()
    print("The scheduler knew nothing about any of this. It fired nodes whose inputs")
    print("were ready, in a deterministic order, and the time alignment was a node.")


if __name__ == "__main__":
    main()
