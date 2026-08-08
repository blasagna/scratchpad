"""Synthetic audio and video as numpy arrays. Deterministic, and no codecs.

Audio is float32 blocks of samples; video is ``H x W x 3`` uint8 frames. Both are
generated from a seeded ``numpy.random.Generator`` where they are random at all, so a
replay demo can hash the output and mean it.
"""

from __future__ import annotations

import numpy as np

from dfg.message import Message, ts_from_sample_index

DEFAULT_SAMPLE_RATE = 16_000.0
DEFAULT_SEED = 20260808


def synth_tone(
    block_count: int,
    *,
    block_size: int = 512,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    frequencies: tuple[float, ...] = (440.0, 1_000.0),
    amplitudes: tuple[float, ...] | None = None,
    noise: float = 0.01,
    seed: int = DEFAULT_SEED,
) -> list[Message[np.ndarray]]:
    """Generate blocks of a sum of sine tones plus a little noise.

    The blocks are contiguous: block ``k`` holds samples ``k*block_size`` onward, so
    the sample times line up with a real capture and framing across a block boundary
    behaves the way it would in one.

    Args:
        block_count: How many blocks.
        block_size: Samples per block.
        sample_rate: Samples per second, used for both the tones and the timestamps.
        frequencies: Tone frequencies in Hz.
        amplitudes: One per frequency, or ``None`` for equal amplitudes summing to 1.
        noise: Gaussian noise standard deviation.
        seed: Fixes the noise.

    Returns:
        Messages whose payloads are contiguous ``float32`` arrays of ``block_size``.
    """
    if amplitudes is None:
        amplitudes = tuple(1.0 / len(frequencies) for _ in frequencies)
    if len(amplitudes) != len(frequencies):
        raise ValueError("frequencies and amplitudes must be the same length")
    rng = np.random.default_rng(seed)
    blocks: list[Message[np.ndarray]] = []
    for k in range(block_count):
        start = k * block_size
        t = (start + np.arange(block_size, dtype=np.float64)) / sample_rate
        signal = np.zeros(block_size, dtype=np.float64)
        for frequency, amplitude in zip(frequencies, amplitudes):
            signal += amplitude * np.sin(2.0 * np.pi * frequency * t)
        if noise:
            signal += rng.normal(0.0, noise, block_size)
        blocks.append(
            Message(
                signal.astype(np.float32),
                ts_from_sample_index(start, sample_rate),
            )
        )
    return blocks


def synth_noise(
    block_count: int,
    *,
    block_size: int = 512,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    seed: int = DEFAULT_SEED,
) -> list[Message[np.ndarray]]:
    """Generate blocks of white noise, for a null case with no spectral peak."""
    rng = np.random.default_rng(seed)
    return [
        Message(
            rng.normal(0.0, 0.25, block_size).astype(np.float32),
            ts_from_sample_index(k * block_size, sample_rate),
        )
        for k in range(block_count)
    ]


def synth_frames(
    count: int,
    *,
    width: int = 64,
    height: int = 48,
    fps: float = 30.0,
    box: int = 8,
    background: int = 24,
) -> list[Message[np.ndarray]]:
    """Generate frames of a bright box moving on a dim background.

    The box follows a deterministic Lissajous-ish path, so a frame's content is a
    pure function of its index -- which is what lets a test assert that an overlay
    touched exactly the pixels it should have.

    Args:
        count: How many frames.
        width: Frame width in pixels.
        height: Frame height in pixels.
        fps: Frames per second, for the timestamps.
        box: Side length of the moving box, in pixels.
        background: Grey level of the background.

    Returns:
        Messages whose payloads are ``height x width x 3`` ``uint8`` arrays.
    """
    frames: list[Message[np.ndarray]] = []
    for i in range(count):
        frame = np.full((height, width, 3), background, dtype=np.uint8)
        cx = int((width - box) * (0.5 + 0.5 * np.sin(2.0 * np.pi * i / 24.0)))
        cy = int((height - box) * (0.5 + 0.5 * np.sin(2.0 * np.pi * i / 17.0)))
        frame[cy : cy + box, cx : cx + box] = (240, 200, 160)
        frames.append(Message(frame, ts_from_sample_index(i, fps)))
    return frames


def frame_index_of(timestamp: int, *, fps: float = 30.0) -> int:
    """Recover a frame index from its sample time. Handy for assertions."""
    return round(timestamp * fps / 1_000_000_000)
