#!/usr/bin/env python3
"""Generate the test tones the on-device peak-frequency calculation is checked against.

The firmware reports one number per capture -- the interpolated peak of a
2048-point FFT averaged over 15 blocks -- and the only way to know whether that
number is right is to put a known frequency in front of the microphone. These are
those known frequencies, written as WAV files so playback is somebody else's
problem (see ``tone_sweep.py``, which plays them and reads the answers back).

Three things about the waveform matter to the measurement:

* **The fade.** A tone that starts and stops abruptly is a tone multiplied by a
  rectangular window, and that convolves the whole spectrum with a sinc -- energy
  smeared across every bin, which is exactly what the peak search is trying to
  resolve. A raised-cosine fade at each end costs 50 ms and removes it.
* **The amplitude.** 0.35 of full scale by default, well clear of any clipping in
  the host's mixer. The microphone channel's own headroom is a separate matter and
  is set in ``app.overlay``; see the README on ``ADC_GAIN_4``.
* **The sample rate.** 48 kHz, the rate the host's audio stack runs at natively,
  so nothing resamples on the way out and adds its own artefacts. Note that this
  puts the Nyquist limit well above the *board's* 31311 Hz sampling -- tones above
  ~15.6 kHz will alias on the device, which is a property of the test rig worth
  remembering rather than a bug to fix.

Two ladders are provided. ``SWEEP_HZ`` spans the usable band in round numbers, for
"is the reported frequency right". ``bin_centre_hz()`` places a tone exactly on an
FFT bin centre, for "is the *sample rate* right" -- a tone on a bin centre lands on
one bin and stays there, so a peak that shows up one bin low is a sample-rate error
of exactly one bin's worth and nothing else. That is how the HFXO hold and the
``SAMPLE_RATE_HZ`` off-by-one were both found; see the README's "known limitations".

Usage::

    pixi run tones                             # the default ladder, into tones/
    pixi run tones --out-dir /tmp/t --seconds 5
    pixi run tones --freqs 440,880             # something else entirely
    pixi run tones --bin 100                   # exactly bin 100's centre
"""

import argparse
import math
import struct
import wave
from pathlib import Path

# The host audio stack's native rate. Not the board's -- see the module docstring.
DEFAULT_RATE = 48000

DEFAULT_SECONDS = 10.0
DEFAULT_AMPLITUDE = 0.35

# Long enough that the transient is negligible against a 10 s tone, short enough
# not to eat into the ~1 s the board actually captures.
FADE_SECONDS = 0.05

# The firmware's own numbers, from src/audio.c. Only bin_centre_hz() uses them, and
# only to place a tone; nothing here has to agree with the device to measure it.
FFT_SIZE = 2048
SAMPLE_RATE_HZ = 31311

# Round frequencies across the band the microphone actually covers. The element
# rolls off around 10 kHz (README, "known limitations"), so the top two rows are
# expected to be the noisy ones -- they are in the ladder because a peak search
# that fails should fail visibly, not be excluded from the test.
SWEEP_HZ = (500, 1000, 1500, 2000, 3000, 5000, 8000)


def bin_centre_hz(
    bin_index: int, rate: int = SAMPLE_RATE_HZ, size: int = FFT_SIZE
) -> float:
    """The frequency that lands exactly on ``bin_index`` at the device's rate."""
    return bin_index * rate / size


def tone_frames(
    freq_hz: float,
    seconds: float = DEFAULT_SECONDS,
    amplitude: float = DEFAULT_AMPLITUDE,
    rate: int = DEFAULT_RATE,
    channels: int = 2,
) -> bytes:
    """One faded sine, as signed 16-bit little-endian frames."""
    total = int(rate * seconds)
    fade = max(1, int(rate * FADE_SECONDS))
    frame = struct.Struct("<" + "h" * channels)
    out = bytearray()

    for n in range(total):
        # Raised cosine in, raised cosine out, flat between. min() of the two ramps
        # keeps it correct even when the tone is shorter than two fades.
        ramp = min(1.0, n / fade, (total - n) / fade)
        env = 0.5 - 0.5 * math.cos(math.pi * ramp)
        v = int(amplitude * env * math.sin(2.0 * math.pi * freq_hz * n / rate) * 32767)
        out += frame.pack(*([v] * channels))

    return bytes(out)


def write_tone(
    path: Path,
    freq_hz: float,
    seconds: float = DEFAULT_SECONDS,
    amplitude: float = DEFAULT_AMPLITUDE,
    rate: int = DEFAULT_RATE,
    channels: int = 2,
) -> Path:
    """Write one tone to ``path``, creating parent directories as needed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(tone_frames(freq_hz, seconds, amplitude, rate, channels))
    return path


def tone_path(out_dir: Path, freq_hz: float) -> Path:
    """A stable name per frequency, so a re-run overwrites rather than accumulates."""
    return out_dir / f"tone_{round(freq_hz)}hz.wav"


def generate(
    freqs: list[float],
    out_dir: Path,
    seconds: float = DEFAULT_SECONDS,
    amplitude: float = DEFAULT_AMPLITUDE,
    rate: int = DEFAULT_RATE,
) -> list[tuple[float, Path]]:
    """Write every tone up front and return ``(frequency, path)`` pairs.

    Generating the whole set before any of it is played is not an optimisation. The
    first version of the sweep interleaved the two, and the several seconds of
    synthesis between playbacks overlapped the previous ``paplay``'s shutdown, so
    the 3 kHz and 8 kHz rows came back as nonsense on the first run of every
    session and were fine on the second. Anything that plays these should call this
    once, before it starts.
    """
    return [
        (f, write_tone(tone_path(out_dir, f), f, seconds, amplitude, rate))
        for f in freqs
    ]


def parse_freqs(text: str) -> list[float]:
    try:
        freqs = [float(part) for part in text.split(",") if part.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"not a comma-separated frequency list: {text}"
        ) from exc
    if not freqs:
        raise argparse.ArgumentTypeError("no frequencies given")
    if any(f <= 0 for f in freqs):
        raise argparse.ArgumentTypeError("frequencies must be positive")
    return freqs


def add_tone_arguments(parser: argparse.ArgumentParser) -> None:
    """The options shared with ``tone_sweep.py``, so both spell them the same way."""
    parser.add_argument(
        "--freqs",
        type=parse_freqs,
        default=list(SWEEP_HZ),
        help="comma-separated tone frequencies in Hz (default: %s)"
        % ", ".join(str(f) for f in SWEEP_HZ),
    )
    parser.add_argument(
        "--bin",
        type=int,
        default=None,
        help="ignore --freqs and use the tone that lands exactly on this FFT bin "
        f"centre at the device's {SAMPLE_RATE_HZ} Hz -- the sample-rate check",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("tones"),
        help="where to write the WAV files (default: %(default)s)",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=DEFAULT_SECONDS,
        help="tone length, seconds (default: %(default)s). Must comfortably exceed "
        "the ~1 s capture plus the settling time before it.",
    )
    parser.add_argument(
        "--amplitude",
        type=float,
        default=DEFAULT_AMPLITUDE,
        help="0..1 of full scale (default: %(default)s)",
    )
    parser.add_argument(
        "--rate",
        type=int,
        default=DEFAULT_RATE,
        help="WAV sample rate (default: %(default)s)",
    )


def resolve_freqs(args: argparse.Namespace) -> list[float]:
    """``--bin`` wins over ``--freqs``, since it names one specific measurement."""
    if args.bin is not None:
        return [bin_centre_hz(args.bin)]
    return list(args.freqs)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0] if __doc__ else None,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_tone_arguments(parser)
    args = parser.parse_args(argv)

    freqs = resolve_freqs(args)
    if args.bin is not None:
        print(
            f"bin {args.bin} of {FFT_SIZE} at {SAMPLE_RATE_HZ} Hz = {freqs[0]:.2f} Hz"
        )

    for freq, path in generate(
        freqs, args.out_dir, args.seconds, args.amplitude, args.rate
    ):
        print(f"{freq:9.2f} Hz  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
