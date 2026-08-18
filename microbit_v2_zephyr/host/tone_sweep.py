#!/usr/bin/env python3
"""Play the test tones and check what the board says the peak frequency is.

The other half of ``tones.py``: it makes the known frequencies, this plays them at
the microphone and reads the firmware's answer back over the console shell, one row
per tone. What it exercises is the whole chain -- SAADC sampling, the Hann window,
the 15-block Welch average, the parabolic interpolation in ``peak_frequency()``, and
the sample rate all of it is scaled by -- against a source the host controls.

The board is driven over its USB CDC shell (``/dev/ttyACM0``), not over BLE:

* ``input report 1 30 1`` injects a synthetic button-A press, which is what starts a
  capture. Pressing the real button taps the PCB centimetres from the microphone and
  puts the thump into the very block being measured, so it is the wrong way to do
  this; see the README's "console shell" section.
* ``audio spectrum`` prints the interpolated peak, the top bins, and the mean
  magnitude the peak is standing above.

Both come from ``CONFIG_SHELL``, so the board must be running a build with the
console shell in it. Only one program can hold the serial port -- close any
``west espressif monitor``/``pyserial-miniterm`` first.

Usage, from this directory with speakers on and the board in front of them::

    pixi run sweep                          # the default ladder
    pixi run sweep --freqs 1000,3000
    pixi run sweep --bin 100                # the sample-rate check, see tones.py
    pixi run sweep --repeat 3               # three captures per tone
    pixi run sweep --no-play                # drive an external source yourself

What the columns mean:

    tone Hz     what was played
    peak Hz     the firmware's interpolated peak
    error %     (peak - tone) / tone; the number the whole exercise is about
    bin         the strongest bin, so a half-bin disagreement is visible as such
    margin dB   top bin over the mean magnitude of the searched band -- how far
                the peak stands clear of the floor. Below about 6 dB the peak is
                not really a peak and the error column should not be believed.
    ms          capture wall time. The SAADC falls back to software-timed sampling
                if a precondition slips, so a row much longer than the expected
                figure means the reported frequency is measured against the wrong
                clock. Flagged with a `!`.

A caveat that cost an afternoon, recorded in the README as well: the 5 kHz and
8 kHz rows are far more directional than the low ones and will not reproduce if the
board has been moved. Aim the microphone at the speaker before suspecting the code,
and check the margin column before reading anything into a bad error figure.
"""

import argparse
import math
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from tones import add_tone_arguments, generate, resolve_freqs, tone_path

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200

# Button A is input code 30 on this board; buttons.c listens with
# INPUT_CALLBACK_DEFINE(NULL, ...) and so cannot tell this from a real press.
TRIGGER_CMD = "input report 1 30 1"

# Rows to ask for. One is enough for the peak, but the runners-up are what tell a
# clean tone from a smeared one at a glance.
SPECTRUM_ROWS = 6

# Let the tone reach steady state -- and the player actually open the device --
# before the capture starts.
PLAYER_LEAD_S = 1.0

# A capture is ~1.06 s; the shell prints nothing until it finishes.
CAPTURE_WAIT_S = 3.0

ANSI = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")

PEAK_RE = re.compile(
    r"interpolated peak\s+([-\d.]+)\s+Hz,\s+mean magnitude\s+([-\d.]+)"
)
ELAPSED_RE = re.compile(r"(\d+)\s+ms elapsed \(expected ~(\d+)\)")
ROW_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*$")


class Shell:
    """A line-oriented view of the board's console, quiet-period framed.

    The shell has no end-of-output marker, so every read waits for the port to go
    quiet rather than for a sentinel. That also absorbs the log records the shell
    backend interleaves with command output.
    """

    def __init__(self, port: str, baud: int) -> None:
        self.ser = serial.Serial(port, baud, timeout=0.3)

    def __enter__(self) -> "Shell":
        self.ser.reset_input_buffer()
        return self

    def __exit__(self, *exc: object) -> None:
        self.ser.close()

    def send(self, cmd: str, idle: float = 1.0, cap: float = 15.0) -> str:
        self.ser.write(cmd.encode() + b"\r\n")
        out, deadline, quiet = bytearray(), time.time() + cap, time.time() + idle
        while time.time() < min(deadline, quiet):
            chunk = self.ser.read(4096)
            if chunk:
                out += chunk
                quiet = time.time() + idle
        return ANSI.sub("", out.decode("utf-8", "replace"))


class Player:
    """Whatever plays a WAV file, or nothing at all under --no-play."""

    def __init__(self, command: str | None) -> None:
        self.command = command

    def play(self, path: Path) -> subprocess.Popen | None:
        if self.command is None:
            return None
        return subprocess.Popen(
            [self.command, str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


class Reading:
    """One capture's worth of parsed shell output."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.peak_hz: float | None = None
        self.floor: float | None = None
        self.top_bin: int | None = None
        self.top_mag: float | None = None
        self.elapsed_ms: int | None = None
        self.expected_ms: int | None = None

        if (m := PEAK_RE.search(text)) is not None:
            self.peak_hz, self.floor = float(m.group(1)), float(m.group(2))
        if (m := ELAPSED_RE.search(text)) is not None:
            self.elapsed_ms, self.expected_ms = int(m.group(1)), int(m.group(2))
        for line in text.splitlines():
            if (m := ROW_RE.match(line)) is not None and m.group(1) == "1":
                self.top_bin, self.top_mag = int(m.group(2)), float(m.group(4))
                break

    @property
    def ok(self) -> bool:
        return self.peak_hz is not None and self.peak_hz > 0.0

    @property
    def margin_db(self) -> float | None:
        """Top bin over the mean magnitude of the band the firmware searched."""
        if self.top_mag is None or not self.floor:
            return None
        return 20.0 * math.log10(self.top_mag / self.floor)

    @property
    def slow(self) -> bool:
        """A capture long enough to mean the SAADC's hardware timer did not engage."""
        if self.elapsed_ms is None or not self.expected_ms:
            return False
        return self.elapsed_ms > self.expected_ms * 1.25


def measure(shell: Shell, player: Player, path: Path) -> Reading:
    """Play one tone, trigger one capture, read one answer."""
    proc = player.play(path)
    try:
        time.sleep(PLAYER_LEAD_S)
        shell.send(TRIGGER_CMD, idle=CAPTURE_WAIT_S)
        return Reading(shell.send(f"audio spectrum {SPECTRUM_ROWS}"))
    finally:
        if proc is not None:
            proc.terminate()
            # Wait for it, so the next tone's playback does not start into a device
            # this one has not finished releasing. See tones.generate().
            proc.wait()


def format_row(freq: float, reading: Reading) -> str:
    ms = "-" if reading.elapsed_ms is None else str(reading.elapsed_ms)
    flag = "!" if reading.slow else ""

    if not reading.ok:
        return (
            f"{freq:9.2f}  {'no peak':>10}  {'-':>8}  {'-':>5}  {'-':>9}  {ms:>5}{flag}"
        )

    assert reading.peak_hz is not None
    error = (reading.peak_hz - freq) / freq * 100.0
    bin_text = "-" if reading.top_bin is None else str(reading.top_bin)
    margin = reading.margin_db
    margin_text = "-" if margin is None else f"{margin:.1f}"
    return (
        f"{freq:9.2f}  {reading.peak_hz:10.2f}  {error:+8.3f}  "
        f"{bin_text:>5}  {margin_text:>9}  {ms:>5}{flag}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0] if __doc__ else None,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_tone_arguments(parser)
    parser.add_argument(
        "--port", default=DEFAULT_PORT, help="console shell (default: %(default)s)"
    )
    parser.add_argument(
        "--baud", type=int, default=DEFAULT_BAUD, help="default: %(default)s"
    )
    parser.add_argument(
        "--repeat", type=int, default=1, help="captures per tone (default: %(default)s)"
    )
    parser.add_argument(
        "--player",
        default="paplay",
        help="command that plays a WAV file (default: %(default)s; `aplay` and "
        "`pw-play` take the same one argument)",
    )
    parser.add_argument(
        "--no-play",
        action="store_true",
        help="generate the tones and drive the captures, but play nothing -- for "
        "feeding the board from a signal generator or another machine",
    )
    parser.add_argument(
        "--raw",
        action="store_true",
        help="also print each capture's shell output verbatim",
    )
    args = parser.parse_args(argv)

    freqs = resolve_freqs(args)
    print(f"generating {len(freqs)} tone(s) into {args.out_dir}/ ...", file=sys.stderr)
    generate(freqs, args.out_dir, args.seconds, args.amplitude, args.rate)

    player = Player(None if args.no_play else args.player)
    if args.no_play:
        print("--no-play: start each tone yourself when prompted", file=sys.stderr)

    print()
    print(
        f"{'tone Hz':>9}  {'peak Hz':>10}  {'error %':>8}  {'bin':>5}  {'margin dB':>9}  {'ms':>5}"
    )
    print(f"{'-' * 9}  {'-' * 10}  {'-' * 8}  {'-' * 5}  {'-' * 9}  {'-' * 5}")

    failures = 0
    with Shell(args.port, args.baud) as shell:
        for freq in freqs:
            path = tone_path(args.out_dir, freq)
            for _ in range(args.repeat):
                if args.no_play:
                    input(f"play {path} at the board, then press Enter ")
                reading = measure(shell, player, path)
                print(format_row(freq, reading), flush=True)
                if args.raw:
                    print(reading.text)
                failures += not reading.ok

    print()
    print("error % is (peak - tone) / tone; `!` marks a capture slow enough that the")
    print("SAADC's hardware timer probably did not engage. Distrust any row whose")
    print("margin is under about 6 dB -- see the module docstring.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
