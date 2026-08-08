"""Synthetic IMU data. Standard library only, and deterministic.

A fixed seed and integer-nanosecond timestamps mean the same call gives the same
messages every time, which is what a replay demo needs in order to prove anything.
"""

from __future__ import annotations

import math
import random

from dfg.message import Message, ts_from_sample_index
from examples.nodes.imu import GRAVITY_M_S2, ImuSample

DEFAULT_RATE_HZ = 200.0
DEFAULT_SEED = 20260808


def synth_imu(
    count: int,
    *,
    rate_hz: float = DEFAULT_RATE_HZ,
    seed: int = DEFAULT_SEED,
    noise: float = 0.02,
    bias: tuple[float, float, float] = (0.05, -0.03, 0.10),
    tilt_rate_rad_s: float = 0.20,
) -> list[Message[ImuSample]]:
    """Generate ``count`` IMU samples of a slow, noisy roll about the x axis.

    The signal is deliberately simple and recognisable: gravity rotating in the
    y/z plane at ``tilt_rate_rad_s``, a constant per-axis bias for
    :class:`~examples.nodes.imu.Calibrate` to remove, and a little noise.

    Args:
        count: How many samples.
        rate_hz: Sample rate, used for the timestamps and the tilt integration.
        seed: Fixes the noise.
        noise: Uniform noise half-width, in the sensors' own units.
        bias: A constant accelerometer offset, in m/s^2.
        tilt_rate_rad_s: How fast the sensor rolls.

    Returns:
        Messages whose payloads are :class:`~examples.nodes.imu.ImuSample`, with
        sample times ``i / rate_hz`` in nanoseconds.
    """
    rng = random.Random(seed)
    bx, by, bz = bias
    messages: list[Message[ImuSample]] = []
    for i in range(count):
        angle = tilt_rate_rad_s * i / rate_hz
        messages.append(
            Message(
                ImuSample(
                    ax=bx + rng.uniform(-noise, noise),
                    ay=by + GRAVITY_M_S2 * math.sin(angle) + rng.uniform(-noise, noise),
                    az=bz + GRAVITY_M_S2 * math.cos(angle) + rng.uniform(-noise, noise),
                    gx=tilt_rate_rad_s + rng.uniform(-noise, noise),
                    gy=rng.uniform(-noise, noise),
                    gz=rng.uniform(-noise, noise),
                ),
                ts_from_sample_index(i, rate_hz),
            )
        )
    return messages


def synth_frame_labels(count: int, *, rate_hz: float = 30.0) -> list[Message[str]]:
    """Generate ``count`` placeholder video frames as labelled strings.

    30 fps against a 200 Hz IMU is the rate mismatch the contract points at, and
    resolving it is an ordinary node's job -- see
    :class:`examples.nodes.core.Resample`.
    """
    return [
        Message(f"frame{i:04d}", ts_from_sample_index(i, rate_hz)) for i in range(count)
    ]


def imu_recording(
    count: int = 20,
    *,
    input_name: str = "imu_raw",
    rate_hz: float = DEFAULT_RATE_HZ,
    seed: int = DEFAULT_SEED,
) -> list[tuple[str, Message[ImuSample]]]:
    """An injection recording: ``(input name, message)`` pairs, ready for replay."""
    return [
        (input_name, message)
        for message in synth_imu(count, rate_hz=rate_hz, seed=seed)
    ]


def tracker_recording(
    count: int = 20, *, rate_hz: float = DEFAULT_RATE_HZ, seed: int = DEFAULT_SEED
) -> list[tuple[str, Message]]:
    """A recording for the two-input tracker graph in ``imu_pipeline``.

    One frame per IMU sample, interleaved, so ``overlay``'s ``all`` readiness is
    satisfied on every sample. A real 30 fps camera would not oblige -- that is what
    ``video_pipeline``'s resample node is for.
    """
    recording: list[tuple[str, Message]] = []
    for i, message in enumerate(synth_imu(count, rate_hz=rate_hz, seed=seed)):
        recording.append(("imu_raw", message))
        recording.append(("frames", Message(f"frame{i:04d}", message.timestamp)))
    return recording
