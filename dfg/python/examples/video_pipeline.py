"""Two streams at different rates, aligned by a node rather than by the scheduler.

Run with ``pixi run video``.

This is the case the contract is most pointed about. A 200 Hz signal and a 30 fps camera
do not line up, and **the framework does not align time**: no watermarks, no lateness,
no event-time windows. Matching the two streams is done by an ordinary node you
write::

    signal_raw --> integrate ------> hold.fast -\\
    frames     --> decimate 2:1 ---> hold.slow --> hold -> overlay -> gray -> stats

``hold`` is :class:`examples.nodes.core.Resample`, a zero-order hold wired with
``any`` readiness so the fast stream can run ahead of the slow one. That readiness
rule is not the default, and it has to be chosen here rather than by the node,
because the node cannot know how it will be wired.

Also on show: temporal decimation as the **zero-output** case (two firings in three
produce nothing), numpy ``uint8`` frames as payloads, and a running scalar
(:class:`examples.nodes.signal.Integrate`) driving the overlay's position.
"""

from __future__ import annotations

import numpy as np

from dfg.blueprint import GraphBuilder, GraphSpec
from dfg.graph import Graph
from dfg.message import Message
from dfg.readiness import AnyInput
from dfg.registry import Registry
from examples.nodes import core, signal, video
from examples.synth import synth_signal
from examples.synth_media import synth_frames

SAMPLE_RATE_HZ = 200.0
FPS = 30.0
FRAME_COUNT = 12
DECIMATE = 2
WIDTH, HEIGHT = 64, 48


def build_registry() -> Registry:
    registry = Registry()
    core.register(registry)
    signal.register(registry)
    video.register(registry)
    return registry


def build_blueprint() -> GraphSpec:
    """The signal-driven overlay with a real video branch."""
    builder = GraphBuilder("signal_video")
    builder.add("integrate", signal.Integrate, params={"rate_hz": SAMPLE_RATE_HZ})
    builder.add("thin", core.Decimate, params={"factor": DECIMATE})
    # `any`, not the default `all`: a hold must accept a track with no frame waiting.
    builder.add("hold", core.Resample, readiness=AnyInput())
    builder.add("overlay", video.OverlayBox, params={"size": 6})
    builder.add("gray", video.ToGray)
    builder.add("stats", video.FrameStatsNode)

    builder.connect("integrate.track", "hold.fast")
    builder.connect("thin.output", "hold.slow")
    builder.connect("hold.output", "overlay.inp")
    builder.connect("overlay.output", "gray.inp")
    builder.connect("gray.output", "stats.inp")

    builder.add_input("signal_raw", "integrate.sample", type_tag="Sample")
    builder.add_input("frames", "thin.inp", type_tag=video.FRAME_RGB)
    builder.add_output("composited", "overlay.output")
    builder.add_output("stats", "stats.output")
    return builder.build()


def interleave(frames, samples):
    """Merge two timestamped streams into injection order, oldest sample time first.

    This is what a recorder would have written down, and injecting in this order is
    what makes the demo resemble a real capture rather than a contrived one.
    """
    recording = [("frames", message) for message in frames]
    recording += [("signal_raw", message) for message in samples]
    recording.sort(key=lambda pair: (pair[1].timestamp, pair[0]))
    return recording


def main() -> None:
    frames = synth_frames(FRAME_COUNT, width=WIDTH, height=HEIGHT, fps=FPS)
    duration_s = FRAME_COUNT / FPS
    samples = synth_signal(round(duration_s * SAMPLE_RATE_HZ), rate_hz=SAMPLE_RATE_HZ)
    recording = interleave(frames, samples)

    print(
        f"{len(frames)} frames at {FPS:.0f} fps and {len(samples)} signal samples at "
        f"{SAMPLE_RATE_HZ:.0f} Hz, over {duration_s:.2f} s"
    )
    print(
        f"frame payload: {type(frames[0].payload).__name__} "
        f"{frames[0].payload.dtype} {frames[0].payload.shape}"
    )
    print(f"injections in timestamp order: {len(recording)}")
    print()

    composited: list[Message[np.ndarray]] = []
    stats: list[Message[video.FrameStats]] = []
    tracks: list[Message] = []

    with Graph.instantiate(build_blueprint(), build_registry()) as graph:
        graph.subscribe("integrate.track", lambda name, m: tracks.append(m))
        for name, message in recording:
            graph.inject(name, message)
            graph.run_until_idle()
        composited.extend(graph.poll("composited"))
        stats.extend(graph.poll("stats"))
        node_stats = dict(graph.control.node_stats())
        kept_frames = graph.control.edge_stats()["thin.output -> hold.slow"].enqueued

    print("Rate mismatch, handled by a node")
    print("=" * 74)
    print(f"  tracks produced by integrate:    {len(tracks)}")
    print(f"  frames in:                       {len(frames)}")
    print(f"  frames after {DECIMATE}:1 decimation:     {kept_frames}")
    print(f"  composited frames out:           {len(composited)}")
    print()
    print(
        f"  The hold discarded {len(tracks) - len(composited)} tracks: it keeps only the"
    )
    print("  newest, and the frame decides when to look. One decimated frame was")
    print("  itself dropped -- it arrived at t=0, before any track existed, and a hold")
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
        f"  {'t (s)':>8}  {'mean luma':>10}  {'peak':>5}  {'bright px':>9}  {'held track':>10}"
    )
    for stat, frame in zip(stats, composited):
        held = max(
            (p for p in tracks if p.timestamp <= frame.timestamp),
            key=lambda p: p.timestamp,
        )
        print(
            f"  {stat.timestamp / 1e9:8.4f}  {stat.payload.mean:10.4f}  "
            f"{stat.payload.peak:5d}  {stat.payload.bright_pixels:9d}  "
            f"{held.payload:+10.4f}"
        )
    print()
    print("  The luma columns are constant on purpose: the synthetic box only moves,")
    print(
        "  so the bright area never changes. The 'held track' column is the scalar the"
    )
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
