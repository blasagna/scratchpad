"""Audio through the graph: numpy payloads, many outputs per firing, backpressure.

Run with ``pixi run audio``.

A two-tone signal at 16 kHz arrives in 512-sample float32 blocks and is reframed into
256-sample windows with a 128-sample hop, so **one firing of ``frame`` emits three or
four windows** -- the case that makes zero-or-more the output contract rather than a
convenience. Each window then fans out to an RMS level and an FFT peak, and the two
fan back in::

    blocks -> frame -> hann -> spectrum -> peak -\\
                          \\-> rms ---------------> pack -> summary

The last section deliberately starves an edge: a capacity-4 edge with
``drop_oldest`` under a producer that emits three windows at a time, so the drop
counter earns its keep. ``block`` is not among the choices, on purpose -- it would
deadlock this single-threaded scheduler instantly.
"""

from __future__ import annotations

from dfg.blueprint import GraphBuilder, GraphSpec, Overflow
from dfg.graph import Graph
from dfg.registry import Registry
from examples.nodes import audio, core
from examples.synth_media import DEFAULT_SAMPLE_RATE, synth_noise, synth_tone

BLOCK_SIZE = 512
WINDOW_SIZE = 256
HOP = 128
BLOCK_COUNT = 8
TONES = (440.0, 1_000.0)


def build_registry() -> Registry:
    registry = Registry()
    core.register(registry)
    audio.register(registry)
    return registry


def build_blueprint(
    *, capacity: int | None = None, on_overflow: Overflow = Overflow.ERROR
) -> GraphSpec:
    """The analysis chain. A capacity on one edge exercises backpressure."""
    builder = GraphBuilder("audio", params={"sample_rate": DEFAULT_SAMPLE_RATE})
    builder.add(
        "frame",
        audio.Frame,
        params={
            "size": WINDOW_SIZE,
            "hop": HOP,
            "sample_rate": DEFAULT_SAMPLE_RATE,
        },
    )
    builder.add("hann", audio.Hann)
    builder.add("spectrum", audio.Spectrum)
    builder.add(
        "peak",
        audio.PeakBin,
        params={"sample_rate": DEFAULT_SAMPLE_RATE, "window_size": WINDOW_SIZE},
    )
    builder.add("rms", audio.Rms)
    builder.add("pack", audio.Pack)

    builder.connect(
        "frame.output", "hann.inp", capacity=capacity, on_overflow=on_overflow
    )
    builder.connect("hann.output", "spectrum.inp")
    builder.connect("spectrum.output", "peak.inp")
    builder.connect("frame.output", "rms.inp")
    builder.connect("rms.output", "pack.level")
    builder.connect("peak.output", "pack.peak")

    builder.add_input("blocks", "frame.inp", type_tag=audio.AUDIO_BLOCK)
    builder.add_output("summary", "pack.output")
    return builder.build()


def run(blocks, *, capacity=None, on_overflow=Overflow.ERROR, drain_each=True):
    """Push ``blocks`` through the graph.

    Args:
        blocks: Messages to inject.
        capacity: Bound for the ``frame -> hann`` edge, or ``None`` for unbounded.
        on_overflow: What that edge does when full.
        drain_each: Run to quiescence after each block, as a real-time capture
            would. ``False`` injects everything first, which lets the producer run
            ahead of the consumer -- the only way a bounded edge ever fills up under
            a single-threaded scheduler.

    Returns:
        The summaries, how many windows were framed, and the edge stats.
    """
    spec = build_blueprint(capacity=capacity, on_overflow=on_overflow)
    with Graph.instantiate(spec, build_registry()) as graph:
        for message in blocks:
            graph.inject("blocks", message)
            if drain_each:
                graph.run_until_idle()
        graph.run_until_idle()
        stats = dict(graph.control.edge_stats())
        # How many windows `frame` produced, counted on its *unbounded* branch, so a
        # bound on the other branch cannot change the number.
        windows = stats["frame.output -> rms.inp"].enqueued
        return graph.poll("summary"), windows, stats


def main() -> None:
    blocks = synth_tone(
        BLOCK_COUNT,
        block_size=BLOCK_SIZE,
        sample_rate=DEFAULT_SAMPLE_RATE,
        frequencies=TONES,
    )
    print(
        f"{BLOCK_COUNT} blocks of {BLOCK_SIZE} float32 samples at "
        f"{DEFAULT_SAMPLE_RATE:.0f} Hz, tones at {TONES[0]:.0f} and {TONES[1]:.0f} Hz"
    )
    print(
        f"payload type: {type(blocks[0].payload).__name__} "
        f"{blocks[0].payload.dtype} {blocks[0].payload.shape}"
    )
    print()

    summaries, windows, stats = run(blocks)
    expected = 1 + (BLOCK_COUNT * BLOCK_SIZE - WINDOW_SIZE) // HOP
    print("Output cardinality: one block in, several windows out")
    print("=" * 74)
    print(f"  reframed to {WINDOW_SIZE} samples, hop {HOP}")
    print(f"  windows produced:  {windows}")
    print(f"  1 + (n - size)//hop = {expected}")
    print(f"  summaries out:     {len(summaries)}")
    print(
        f"  windows per firing of `frame`: {windows / BLOCK_COUNT:.2f} on average "
        f"over {BLOCK_COUNT} firings"
    )
    print()

    print("Detected level and peak frequency, every fourth window")
    print("=" * 74)
    print(f"  {'t (s)':>8}  {'RMS (dB)':>9}  {'peak (Hz)':>9}")
    for message in summaries[::4]:
        level, peak = message.payload
        print(f"  {message.timestamp / 1e9:8.4f}  {level:9.2f}  {peak:9.1f}")
    print()
    peaks = {peak for _, peak in (m.payload for m in summaries)}
    print(f"  distinct peaks found: {sorted(peaks)}")
    print(
        f"  the 1000 Hz tone lands in bin {round(1000.0 * WINDOW_SIZE / DEFAULT_SAMPLE_RATE)}, "
        f"which is {round(1000.0 * WINDOW_SIZE / DEFAULT_SAMPLE_RATE) * DEFAULT_SAMPLE_RATE / WINDOW_SIZE:.1f} Hz"
    )
    print("  -- a 256-point window at 16 kHz has 62.5 Hz bins, so that is the")
    print("  resolution, not an error.")
    print()

    noise_summaries, _, _ = run(
        synth_noise(BLOCK_COUNT, block_size=BLOCK_SIZE, sample_rate=DEFAULT_SAMPLE_RATE)
    )
    noise_peaks = {peak for _, peak in (m.payload for m in noise_summaries)}
    print(
        f"  white noise, by contrast, peaks all over: {len(noise_peaks)} distinct bins"
    )
    print()

    print("Backpressure: a bounded edge with the producer running ahead")
    print("=" * 74)
    print("  Draining after each block never fills a bound -- the consumer always")
    print("  catches up. Injecting every block first lets `frame` run ahead:")
    print()
    unbounded, _, _ = run(blocks, drain_each=False)
    print(f"  unbounded            summaries: {len(unbounded)}")
    for policy in (Overflow.DROP_OLDEST, Overflow.DROP_NEWEST):
        dropped_summaries, _, dropped_stats = run(
            blocks, capacity=4, on_overflow=policy, drain_each=False
        )
        edge = dropped_stats["frame.output -> hann.inp"]
        print(
            f"  capacity 4, {policy:<11} enqueued: {edge.enqueued}  "
            f"dropped: {edge.dropped}  summaries: {len(dropped_summaries)}"
        )
    print()
    print("  Note what dropping costs beyond the lost windows: only the spectrum")
    print("  branch was bounded, so it now has fewer messages than the RMS branch,")
    print("  and `pack`'s `all` readiness pairs whatever lines up. Dropping on one")
    print("  branch of a fan-out desynchronizes it from the other -- which is an")
    print("  argument for bounding at the fan-out, not downstream of it.")
    print()
    print("  'block' is not one of the choices. It is the obvious fourth policy and")
    print("  it deadlocks a single-threaded scheduler instantly: the producer waits")
    print("  for space only the consumer can free, and the consumer only runs when")
    print("  the producer returns.")


if __name__ == "__main__":
    main()
