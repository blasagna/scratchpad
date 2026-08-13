#!/usr/bin/env python3
"""Host-side reader for the micro:bit V2 BLE streams, over bleak.

Scans for the board, subscribes to all three notify characteristics at once,
decodes each payload, and measures the link: bits/s, bytes/s, notifications/s and
parsed messages/s, per stream and in total. Button presses are not periodic, so
they are printed as they arrive rather than only counted.

Usage, from this directory with the host's Bluetooth adapter up::

    pixi run stream                          # until Ctrl-C
    pixi run stream --seconds 20
    pixi run stream --streams accel          # isolate one stream's throughput
    pixi run stream --print all              # dump every decoded message

The wire format is defined by ``../src/ble.c`` and tabulated in ``../README.md``.
Everything is little-endian.

This is a single script, unlike the two-file arrangement in
``~/code/remapy/adafruit_feather_sense/`` that it is modelled on. That one splits
the stream from the CLI and runs bleak on a background asyncio thread, because its
consumers are synchronous app loops that poll. Nothing consumes this but the
reporting loop below, so it is plain ``asyncio.run`` with no thread bridge.
"""

import argparse
import asyncio
import math
import struct
import sys
import time
from dataclasses import dataclass

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

DEFAULT_NAME = "microbit-v2"

SERVICE_UUID = "f1b70001-9c4e-4a1d-9a6b-2f0c1d4e7a30"
ACCEL_UUID = "f1b70002-9c4e-4a1d-9a6b-2f0c1d4e7a30"
TEMP_UUID = "f1b70003-9c4e-4a1d-9a6b-2f0c1d4e7a30"
BUTTON_UUID = "f1b70004-9c4e-4a1d-9a6b-2f0c1d4e7a30"

# The accelerometer payload is a header (uint32 t_ms of the *first* sample,
# uint8 count) followed by count * {int16 x, y, z} in milli-g.
ACCEL_HDR = struct.Struct("<IB")
ACCEL_SAMPLE = struct.Struct("<hhh")
TEMP_VALUE = struct.Struct("<h")
BUTTON_EVENT = struct.Struct("<BBI")

# Opcode + handle. Payload bytes are what the characteristic value carries; this
# is the rest of what an ATT notification PDU costs, reported separately in the
# summary so the two never get conflated.
ATT_HEADER_LEN = 3

BUTTON_NAMES = {0: "A", 1: "B"}


@dataclass
class Decoded:
    """One notification, unpacked.

    ``messages`` is what the notification actually delivered: a count of samples
    for the batched accelerometer stream, 1 for the other two. ``t_ms`` is the
    device's own uptime stamp where the payload carries one, else None.
    """

    messages: int
    t_ms: int | None
    text: str
    is_event: bool = False


def decode_accel(data: bytes) -> Decoded:
    if len(data) < ACCEL_HDR.size:
        raise ValueError(f"accel payload is {len(data)} B, shorter than its header")
    t_ms, count = ACCEL_HDR.unpack_from(data, 0)
    expected = ACCEL_HDR.size + count * ACCEL_SAMPLE.size
    if len(data) != expected:
        raise ValueError(
            f"accel payload is {len(data)} B, not {expected} B for count={count}"
        )
    if count == 0:
        raise ValueError("accel batch carries no samples")
    x, y, z = ACCEL_SAMPLE.unpack_from(data, ACCEL_HDR.size)
    magnitude = math.sqrt(x * x + y * y + z * z)
    text = (
        f"accel   t={t_ms} ms  n={count}  "
        f"first x={x} y={y} z={z}  |a|={magnitude:.0f} milli-g"
    )
    return Decoded(messages=count, t_ms=t_ms, text=text)


def decode_temp(data: bytes) -> Decoded:
    if len(data) != TEMP_VALUE.size:
        raise ValueError(
            f"temperature payload is {len(data)} B, not {TEMP_VALUE.size} B"
        )
    (centi_c,) = TEMP_VALUE.unpack(data)
    return Decoded(messages=1, t_ms=None, text=f"temp    {centi_c / 100:.2f} C")


def decode_button(data: bytes) -> Decoded:
    if len(data) != BUTTON_EVENT.size:
        raise ValueError(f"button payload is {len(data)} B, not {BUTTON_EVENT.size} B")
    button, state, t_ms = BUTTON_EVENT.unpack(data)
    name = BUTTON_NAMES.get(button, f"?{button}")
    action = "press" if state else "release"
    return Decoded(
        messages=1,
        t_ms=t_ms,
        text=f"button  {name} {action}  t={t_ms} ms",
        is_event=True,
    )


class Counters:
    """Traffic over one window: cumulative for the summary, or one report interval.

    Rates are always computed against a *measured* elapsed time, never against the
    nominal interval. A loop gated on ``>= interval`` overshoots by however long
    the last sleep ran, so dividing a window's count by the interval it was
    supposed to cover consistently over-reports.
    """

    def __init__(self) -> None:
        self.notifications = 0
        self.messages = 0
        self.payload_bytes = 0
        self.malformed = 0
        self.max_payload_bytes = 0
        self.first_t_ms: int | None = None
        self.last_t_ms: int | None = None
        self.last_notification_messages = 0
        self.max_gap_ms = 0

    def add(self, payload_len: int, decoded: Decoded) -> None:
        self.notifications += 1
        self.messages += decoded.messages
        self.payload_bytes += payload_len
        self.max_payload_bytes = max(self.max_payload_bytes, payload_len)

        if decoded.t_ms is None:
            return
        if self.first_t_ms is None:
            self.first_t_ms = decoded.t_ms
        elif self.last_t_ms is not None:
            self.max_gap_ms = max(self.max_gap_ms, decoded.t_ms - self.last_t_ms)
        self.last_t_ms = decoded.t_ms
        self.last_notification_messages = decoded.messages

    @property
    def att_bytes(self) -> int:
        return self.payload_bytes + self.notifications * ATT_HEADER_LEN

    @property
    def device_hz(self) -> float | None:
        """Message rate from the device's own clock, independent of host scheduling.

        Not messages/elapsed: only the first sample of an accelerometer batch is
        timestamped, so between the first and last stamps in the window exactly
        ``messages - last_notification_messages`` messages went by -- the trailing
        batch's remaining samples are after the last stamp we have.
        """
        if self.first_t_ms is None or self.last_t_ms is None:
            return None
        span_ms = self.last_t_ms - self.first_t_ms
        if span_ms <= 0:
            return None
        return (self.messages - self.last_notification_messages) * 1000.0 / span_ms


class Stream:
    """One characteristic: how to decode it, and what it has delivered."""

    def __init__(self, name: str, uuid: str, decode) -> None:
        self.name = name
        self.uuid = uuid
        self.decode = decode
        self.total = Counters()
        self.window = Counters()

    def record(self, payload_len: int, decoded: Decoded) -> None:
        self.total.add(payload_len, decoded)
        self.window.add(payload_len, decoded)

    def record_malformed(self) -> None:
        self.total.malformed += 1
        self.window.malformed += 1


STREAM_SPECS = {
    "accel": (ACCEL_UUID, decode_accel),
    "temp": (TEMP_UUID, decode_temp),
    "button": (BUTTON_UUID, decode_button),
}

HEADER = (
    f"{'t(s)':>7}  {'stream':<7}{'notif/s':>9}{'msg/s':>9}"
    f"{'B/s':>9}{'bit/s':>10}{'dev Hz':>9}{'gap ms':>8}{'bad':>5}"
)


class Reader:
    """Scan, subscribe, decode, and report."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.streams = [Stream(name, *STREAM_SPECS[name]) for name in args.streams]
        self.started: float | None = None
        self.window_started: float | None = None
        # Set the moment the stream stops, before the notifications are torn
        # down. stop_notify can take a second or more against BlueZ, and timing
        # the summary at print time folds that into the denominator -- enough to
        # under-report a 20 s run by more than 3 %.
        self.stopped: float | None = None

    # --- notifications -------------------------------------------------------
    def _stamp(self, text: str) -> str:
        elapsed = 0.0 if self.started is None else time.monotonic() - self.started
        return f"[{elapsed:7.3f}] {text}"

    def _handler(self, stream: Stream):
        def on_notify(
            _characteristic: BleakGATTCharacteristic, data: bytearray
        ) -> None:
            payload = bytes(data)
            try:
                decoded = stream.decode(payload)
            except ValueError as exc:
                stream.record_malformed()
                print(self._stamp(f"BAD  {exc}  ({payload.hex()})"), file=sys.stderr)
                return
            stream.record(len(payload), decoded)
            if self.args.print == "all" or (
                self.args.print == "events" and decoded.is_event
            ):
                print(self._stamp(decoded.text), flush=True)

        return on_notify

    # --- reporting -----------------------------------------------------------
    def _report_window(self) -> None:
        now = time.monotonic()
        assert self.started is not None and self.window_started is not None
        elapsed = now - self.window_started
        if elapsed <= 0:
            return

        rows = []
        for stream in self.streams:
            rows.append(_format_row(stream.name, stream.window, elapsed))
        if len(self.streams) > 1:
            rows.append(
                _format_row("TOTAL", _merge(s.window for s in self.streams), elapsed)
            )

        prefix = f"{now - self.started:7.1f}  "
        blank = " " * len(prefix)
        for i, row in enumerate(rows):
            print(f"{prefix if i == 0 else blank}{row}", flush=True)

        for stream in self.streams:
            stream.window = Counters()
        self.window_started = now

    def print_summary(self) -> None:
        if self.started is None:
            return
        elapsed = (self.stopped or time.monotonic()) - self.started
        print(f"\n=== summary over {elapsed:.2f} s ===")
        print(
            f"{'stream':<8}{'notif':>8}{'msg':>8}{'payload B':>11}{'ATT B':>9}"
            f"{'notif/s':>9}{'msg/s':>9}{'B/s':>9}{'bit/s':>10}{'dev Hz':>9}"
            f"{'max gap':>9}{'max B':>7}{'bad':>5}"
        )
        for stream in self.streams:
            print(_format_summary_row(stream.name, stream.total, elapsed))
        if len(self.streams) > 1:
            print(
                _format_summary_row(
                    "TOTAL", _merge(s.total for s in self.streams), elapsed
                )
            )
        print(
            "bit/s is the characteristic payload; ATT B adds the "
            f"{ATT_HEADER_LEN}-byte notification header."
        )
        accel = next((s for s in self.streams if s.name == "accel"), None)
        if accel is not None and accel.total.max_payload_bytes:
            largest = accel.total.max_payload_bytes
            samples = (largest - ACCEL_HDR.size) // ACCEL_SAMPLE.size
            print(
                f"largest accel batch: {samples} samples in {largest} B, so the "
                f"negotiated ATT MTU is at least {largest + ATT_HEADER_LEN}."
            )

    # --- the session ---------------------------------------------------------
    async def run(self) -> int:
        target = self.args.address or f'"{self.args.name}"'
        print(f"Scanning for {target} ...", file=sys.stderr)
        if self.args.address:
            device = await BleakScanner.find_device_by_address(
                self.args.address, timeout=self.args.scan_timeout
            )
        else:
            device = await BleakScanner.find_device_by_name(
                self.args.name, timeout=self.args.scan_timeout
            )
        if device is None:
            print(
                "micro:bit not found. Is it powered and advertising?", file=sys.stderr
            )
            return 1

        async with BleakClient(device) as client:
            # Deliberately not reporting client.mtu_size. On the BlueZ backend it
            # is a placeholder 23 until the characteristic is acquired, and
            # acquiring needs a writable one -- all three of these are notify-only.
            # Reading it also raises a UserWarning. The negotiated MTU is 247 (the
            # firmware logs it), and the summary's `max B` measures its effect
            # directly, which is the part that matters. See print_summary.
            print(
                f"Connected to {device.address}  "
                f"streams: {', '.join(s.name for s in self.streams)}",
                file=sys.stderr,
            )
            for stream in self.streams:
                await client.start_notify(stream.uuid, self._handler(stream))

            self.started = self.window_started = time.monotonic()
            print(HEADER, flush=True)
            try:
                await self._pump(client)
            finally:
                self.stopped = time.monotonic()
                for stream in self.streams:
                    try:
                        await client.stop_notify(stream.uuid)
                    except Exception:  # noqa: BLE001 - teardown is best effort
                        pass
        return 0

    async def _pump(self, client: BleakClient) -> None:
        """Idle while the notification callbacks work, reporting on the interval."""
        assert self.started is not None
        deadline = (
            None if self.args.seconds is None else self.started + self.args.seconds
        )
        next_report = self.started + self.args.interval

        while client.is_connected:
            await asyncio.sleep(0.05)
            now = time.monotonic()
            if now >= next_report:
                self._report_window()
                next_report = now + self.args.interval
            if deadline is not None and now >= deadline:
                return
        print("Disconnected by the peer.", file=sys.stderr)


def _merge(counters) -> Counters:
    """Sum several streams into one row. Device-clock fields are per stream only."""
    total = Counters()
    for counter in counters:
        total.notifications += counter.notifications
        total.messages += counter.messages
        total.payload_bytes += counter.payload_bytes
        total.malformed += counter.malformed
        total.max_payload_bytes = max(
            total.max_payload_bytes, counter.max_payload_bytes
        )
    return total


def _rate_fields(
    counters: Counters, elapsed: float
) -> tuple[float, float, float, float]:
    return (
        counters.notifications / elapsed,
        counters.messages / elapsed,
        counters.payload_bytes / elapsed,
        counters.payload_bytes * 8 / elapsed,
    )


def _format_row(name: str, counters: Counters, elapsed: float) -> str:
    notif_s, msg_s, byte_s, bit_s = _rate_fields(counters, elapsed)
    device_hz = counters.device_hz
    dev = "-" if device_hz is None else f"{device_hz:.1f}"
    gap = "-" if counters.first_t_ms is None else str(counters.max_gap_ms)
    return (
        f"{name:<7}{notif_s:9.1f}{msg_s:9.1f}{byte_s:9.1f}{bit_s:10.1f}"
        f"{dev:>9}{gap:>8}{counters.malformed:5d}"
    )


def _format_summary_row(name: str, counters: Counters, elapsed: float) -> str:
    notif_s, msg_s, byte_s, bit_s = _rate_fields(counters, elapsed)
    device_hz = counters.device_hz
    dev = "-" if device_hz is None else f"{device_hz:.1f}"
    gap = "-" if counters.first_t_ms is None else str(counters.max_gap_ms)
    return (
        f"{name:<8}{counters.notifications:8d}{counters.messages:8d}"
        f"{counters.payload_bytes:11d}{counters.att_bytes:9d}"
        f"{notif_s:9.1f}{msg_s:9.1f}{byte_s:9.1f}{bit_s:10.1f}"
        f"{dev:>9}{gap:>9}{counters.max_payload_bytes:7d}{counters.malformed:5d}"
    )


def parse_streams(value: str) -> list[str]:
    names = [name.strip() for name in value.split(",") if name.strip()]
    unknown = [name for name in names if name not in STREAM_SPECS]
    if unknown:
        raise argparse.ArgumentTypeError(
            f"unknown stream(s) {', '.join(unknown)}; choose from {', '.join(STREAM_SPECS)}"
        )
    if not names:
        raise argparse.ArgumentTypeError("no streams selected")
    return names


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read and measure the micro:bit V2 BLE notification streams."
    )
    parser.add_argument(
        "--address", help="BLE address of the board (default: scan by name)"
    )
    parser.add_argument(
        "--name",
        default=DEFAULT_NAME,
        help='advertised name to scan for (default: "%(default)s")',
    )
    parser.add_argument(
        "--scan-timeout", type=float, default=10.0, help="scan timeout, seconds"
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=None,
        help="stop after N seconds (default: until Ctrl-C)",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="seconds between stats lines (default: %(default)s)",
    )
    parser.add_argument(
        "--streams",
        type=parse_streams,
        default=list(STREAM_SPECS),
        help=f"comma-separated subset of {', '.join(STREAM_SPECS)} (default: all)",
    )
    parser.add_argument(
        "--print",
        choices=("events", "all", "none"),
        default="events",
        help="which decoded messages to print (default: %(default)s -- button events only)",
    )
    args = parser.parse_args(argv)

    reader = Reader(args)
    try:
        status = asyncio.run(reader.run())
    except KeyboardInterrupt:
        # asyncio.run cancels the task first, so the client has already torn down.
        status = 0
    reader.print_summary()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
