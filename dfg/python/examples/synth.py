"""Synthetic data for the examples. Standard library only, and deterministic.

A fixed seed and integer-nanosecond timestamps mean the same call gives the same
messages every time, which is what a replay demo needs in order to prove anything.
"""

from __future__ import annotations

import math
import random

from dfg.message import Message, ts_from_sample_index
from examples.nodes.reading import Level, Reading
from examples.nodes.signal import Sample

DEFAULT_RATE_HZ = 200.0
DEFAULT_SEED = 20260808

_LABELS = ("alpha", "beta", "gamma", "delta")


def synth_signal(
    count: int,
    *,
    rate_hz: float = DEFAULT_RATE_HZ,
    seed: int = DEFAULT_SEED,
    noise: float = 0.01,
    drift: float = 0.2,
) -> list[Message[Sample]]:
    """Generate ``count`` samples of a slow, noisy three-channel signal.

    The signal is deliberately simple and recognisable: a near-constant ``x`` so a
    running integral of it climbs steadily, and ``y``/``z`` tracing a circle so ``z``
    crosses zero and the vector magnitude stays interesting for the columnar example.

    Args:
        count: How many samples.
        rate_hz: Sample rate, used for the timestamps.
        seed: Fixes the noise.
        noise: Uniform noise half-width, in the signal's own units.
        drift: The constant level of the ``x`` channel.

    Returns:
        Messages whose payloads are :class:`~examples.nodes.signal.Sample`, with sample
        times ``i / rate_hz`` in nanoseconds.
    """
    rng = random.Random(seed)
    messages: list[Message[Sample]] = []
    for i in range(count):
        angle = 0.3 * i
        messages.append(
            Message(
                Sample(
                    x=drift + rng.uniform(-noise, noise),
                    y=math.sin(angle) + rng.uniform(-noise, noise),
                    z=math.cos(angle) + rng.uniform(-noise, noise),
                ),
                ts_from_sample_index(i, rate_hz),
            )
        )
    return messages


def synth_readings(
    count: int,
    *,
    rate_hz: float = DEFAULT_RATE_HZ,
    seed: int = DEFAULT_SEED,
    noise: float = 0.0,
) -> list[Message[Reading]]:
    """Generate ``count`` readings whose ``value`` ramps across the interesting range.

    ``value`` steps through ``0.0 .. 1.1`` and repeats, so that after the pipeline's
    2x scale it spans all three :class:`~examples.nodes.reading.Level` bands and crosses
    the active threshold. ``active`` and ``level`` start neutral -- the graph is what
    sets them -- and the label cycles through a small vocabulary.

    Args:
        count: How many readings.
        rate_hz: Sample rate, used for the timestamps.
        seed: Fixes the optional noise.
        noise: Uniform noise half-width on ``value``. Zero by default, for an exact ramp.
    """
    rng = random.Random(seed)
    messages: list[Message[Reading]] = []
    for i in range(count):
        base = (i % 12) * 0.1
        messages.append(
            Message(
                Reading(
                    value=base + rng.uniform(-noise, noise),
                    active=False,
                    label=_LABELS[i % len(_LABELS)],
                    level=Level.LOW,
                ),
                ts_from_sample_index(i, rate_hz),
            )
        )
    return messages


def reading_recording(
    count: int = 20, *, rate_hz: float = DEFAULT_RATE_HZ, seed: int = DEFAULT_SEED
) -> list[tuple[str, Message]]:
    """A recording for the two-input processor graph in ``reading_pipeline``.

    One tag per reading, interleaved, so ``relabel``'s ``all`` readiness is satisfied on
    every sample.
    """
    recording: list[tuple[str, Message]] = []
    for i, message in enumerate(synth_readings(count, rate_hz=rate_hz, seed=seed)):
        recording.append(("readings", message))
        recording.append(("tags", Message(f"tag{i:04d}", message.timestamp)))
    return recording
