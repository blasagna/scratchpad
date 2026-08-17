#!/usr/bin/env python3
"""Live rerun visualization of the micro:bit V2 BLE streams.

Subscribes to the same three notify characteristics as ``ble_stream.py`` -- and
reuses that module's decoders, so the wire format has one definition -- then logs
each notification to rerun as a time series instead of a throughput table.

Three views scrolling with a rolling window (5 s by default), plus a text log:

* **accelerometer**, x/y/z and magnitude, converted from the wire's milli-g to
  **m/s²**. At rest the magnitude should read about 9.81.
* **temperature**, converted from the wire's centi-°C to **°F**. This is the
  nRF52833 *die* temperature, not the room -- it idles well above ambient, so
  around 90-110 °F is normal and is not a fire.
* **button state**, a staircase per button holding 0 or 1 between events, so a
  press and its matching release read as a held interval.
* **button events**, a text view of each notification as received: the decoded
  fields beside the raw bytes they were unpacked from. This one is deliberately
  left unwindowed, so the full history of presses stays readable.

Usage, from this directory with the host's Bluetooth adapter up::

    pixi run viz                                 # spawn the viewer, until Ctrl-C
    pixi run viz --seconds 30
    pixi run viz --window 10                     # a 10 s window instead of 5
    pixi run viz --streams accel --window 2      # one stream, tighter window
    pixi run viz --no-spawn --save run.rrd       # headless, for later replay

Modelled on ``~/code/remapy/rerun_viewer/viewer.py``. The package is ``rerun-sdk``,
imported as ``rerun``; the PyPI package named plain ``rerun`` is an unrelated file
watcher.
"""

import argparse
import asyncio
import math
import sys
import time

import rerun as rr
import rerun.blueprint as rrb
from bleak import BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic

from ble_stream import (
    DEFAULT_NAME,
    STREAM_SPECS,
    AccelBatch,
    ButtonEvent,
    Decoded,
    TempReading,
    find_device,
    parse_streams,
)

# Standard gravity, and the exact inverse of the firmware's own conversion:
# `to_milli_g()` in ../src/accel.c divides micro-m/s² by 9806650, so multiplying
# milli-g back by 9.80665/1000 recovers what the sensor reported to within the
# int16 milli-g quantisation -- about 0.0098 m/s² per count.
G_M_S2 = 9.80665

# The firmware's accelerometer rate, needed only by --accel-batch-time spread:
# the batch stamps its first sample and leaves the rest implicit.
ACCEL_PERIOD_S = 0.01

TIMELINE = "time"

BUTTON_NAMES = {0: "A", 1: "B"}

# One colour per accelerometer series.
AXIS_COLORS = {
    "x": (222, 92, 92),
    "y": (92, 190, 122),
    "z": (92, 140, 222),
    "magnitude": (200, 200, 200),
}

BUTTON_COLORS = {"A": (232, 160, 72), "B": (150, 120, 222)}


def to_m_s2(milli_g: int) -> float:
    return milli_g * G_M_S2 / 1000.0


def to_fahrenheit(centi_c: int) -> float:
    return centi_c / 100.0 * 9.0 / 5.0 + 32.0


def _window(seconds: float) -> rrb.VisibleTimeRange:
    """A fixed-width window that follows the play cursor rather than the data."""
    return rrb.VisibleTimeRange(
        TIMELINE,
        start=rrb.TimeRangeBoundary.cursor_relative(seconds=-seconds),
        end=rrb.TimeRangeBoundary.cursor_relative(),
    )


def build_blueprint(streams: list[str], seconds: float) -> rrb.Blueprint:
    """One view per plot, only for the streams actually subscribed."""
    # Annotated because the views are not all one class: the button events are a
    # TextLogView, and inference off the first append would pin this to plots.
    views: list[rrb.View] = []
    if "accel" in streams:
        views.append(
            rrb.TimeSeriesView(
                origin="accel",
                name="accelerometer (m/s²)",
                time_ranges=[_window(seconds)],
            )
        )
    if "temp" in streams:
        views.append(
            rrb.TimeSeriesView(
                origin="temp",
                name="temperature (°F)",
                time_ranges=[_window(seconds)],
            )
        )
    if "button" in streams:
        # A plot, not a StateTimelineView. The state timeline draws nicer labelled
        # bands, but rerun 0.36's viewer gates windowing per view class
        # (`ViewClass::supports_visible_time_range`) and that class does not
        # implement it -- setting VisibleTimeRanges on it lands in the blueprint
        # and is then ignored, so the view never scrolls. A plot windows properly.
        # Pin the y-axis: a signal that is only ever 0 or 1 would otherwise
        # autoscale to whichever value happened to arrive.
        views.append(
            rrb.TimeSeriesView(
                origin="button/state",
                name="button state (held)",
                time_ranges=[_window(seconds)],
                axis_y=rrb.ScalarAxis(range=(-0.1, 1.1)),
            )
        )
        # A log list, not a plot: it has no time axis to window, so it keeps the
        # whole session's events and scrolls rather than following the cursor.
        views.append(
            rrb.TextLogView(origin="button/raw", name="button events (received)")
        )
    return rrb.Blueprint(rrb.Grid(*views), collapse_panels=True)


def log_styles(streams: list[str]) -> None:
    """Series names, colours, and interpolation, logged once for the session.

    ``interpolation_mode="StepAfter"`` is what makes the button state a real
    staircase: the value holds until the next event instead of being interpolated
    into a ramp, so no synthetic point is needed before a transition. Only
    ``button/raw`` needs nothing here -- ``rr.TextLog`` carries its own text.
    """
    if "accel" in streams:
        for axis, color in AXIS_COLORS.items():
            rr.log(
                f"accel/{axis}",
                rr.SeriesLines(names=axis, colors=color, widths=1.5),
                static=True,
            )
    if "temp" in streams:
        rr.log(
            "temp/fahrenheit",
            rr.SeriesLines(names="die temp", colors=(224, 140, 92), widths=1.5),
            static=True,
        )
    if "button" in streams:
        for name, color in BUTTON_COLORS.items():
            rr.log(
                f"button/state/{name}",
                rr.SeriesLines(
                    names=name,
                    colors=color,
                    widths=2.0,
                    interpolation_mode="StepAfter",
                ),
                static=True,
            )


class Viewer:
    """Scan, subscribe, and log every notification to rerun."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.streams = list(args.streams)
        self.started: float | None = None
        # Last known state of each button. A plotted line has no notion of "holds
        # until the next event on this entity", so every event restates both
        # buttons and neither staircase is left with a gap.
        self.button_states: dict[int, int] = dict.fromkeys(BUTTON_NAMES, 0)
        self.malformed = 0

    # --- logging -------------------------------------------------------------
    def _elapsed(self) -> float:
        return 0.0 if self.started is None else time.monotonic() - self.started

    def _log_accel(self, batch: AccelBatch, arrival: float) -> None:
        # The whole batch arrives in one notification, so with --accel-batch-time
        # arrival every sample shares one instant and the 100 Hz trace stacks up
        # vertically. `spread` back-dates within the batch using the firmware's
        # nominal period -- still only the host clock, just un-batched.
        count = len(batch.samples)
        for i, (x_mg, y_mg, z_mg) in enumerate(batch.samples):
            if self.args.accel_batch_time == "spread":
                rr.set_time(
                    TIMELINE, duration=arrival - (count - 1 - i) * ACCEL_PERIOD_S
                )
            else:
                rr.set_time(TIMELINE, duration=arrival)
            x, y, z = to_m_s2(x_mg), to_m_s2(y_mg), to_m_s2(z_mg)
            rr.log("accel/x", rr.Scalars(x))
            rr.log("accel/y", rr.Scalars(y))
            rr.log("accel/z", rr.Scalars(z))
            rr.log("accel/magnitude", rr.Scalars(math.sqrt(x * x + y * y + z * z)))

    def _log_temp(self, reading: TempReading, arrival: float) -> None:
        rr.set_time(TIMELINE, duration=arrival)
        rr.log("temp/fahrenheit", rr.Scalars(to_fahrenheit(reading.centi_c)))

    def _log_initial_states(self) -> None:
        """Start both staircases at 0, so a button never touched still has a line."""
        rr.set_time(TIMELINE, duration=0.0)
        self._log_states()

    def _log_states(self) -> None:
        for button, name in BUTTON_NAMES.items():
            rr.log(
                f"button/state/{name}",
                rr.Scalars(float(self.button_states.get(button, 0))),
            )

    def _log_button(self, event: ButtonEvent, payload: bytes, arrival: float) -> None:
        rr.set_time(TIMELINE, duration=arrival)
        self.button_states[event.button] = int(event.pressed)
        self._log_states()
        # The received notification, spelled out: the decoded fields next to the
        # bytes they came from, so the text view doubles as a wire-format check.
        name = BUTTON_NAMES.get(event.button, f"?{event.button}")
        action = "press" if event.pressed else "release"
        rr.log(
            "button/raw",
            rr.TextLog(
                f"{name} {action}  "
                f"button={event.button} state={int(event.pressed)} "
                f"t={event.t_ms} ms  raw={payload.hex(' ')}",
                level="INFO",
            ),
        )
        if self.args.print_events:
            print(f"[{arrival:7.3f}] {name} {action}", flush=True)

    def _log(self, decoded: Decoded, payload: bytes) -> None:
        arrival = self._elapsed()
        value = decoded.value
        if isinstance(value, AccelBatch):
            self._log_accel(value, arrival)
        elif isinstance(value, TempReading):
            self._log_temp(value, arrival)
        else:
            self._log_button(value, payload, arrival)

    def _handler(self, name: str, decode):
        def on_notify(
            _characteristic: BleakGATTCharacteristic, data: bytearray
        ) -> None:
            payload = bytes(data)
            try:
                decoded = decode(payload)
            except ValueError as exc:
                self.malformed += 1
                print(f"BAD {name}: {exc}  ({payload.hex()})", file=sys.stderr)
                return
            self._log(decoded, payload)

        return on_notify

    # --- the session ---------------------------------------------------------
    async def run(self) -> int:
        device = await find_device(
            name=self.args.name,
            address=self.args.address,
            timeout=self.args.scan_timeout,
        )
        if device is None:
            return 1

        async with BleakClient(device) as client:
            # Deliberately not reading client.mtu_size -- on the BlueZ backend it
            # is a placeholder 23 that also raises a UserWarning. See
            # ble_stream.py and ../CLAUDE.md.
            print(
                f"Connected to {device.address}  streams: {', '.join(self.streams)}",
                file=sys.stderr,
            )
            for name in self.streams:
                uuid, decode = STREAM_SPECS[name]
                await client.start_notify(uuid, self._handler(name, decode))

            self.started = time.monotonic()
            if "button" in self.streams:
                self._log_initial_states()
            try:
                await self._pump(client)
            finally:
                for name in self.streams:
                    try:
                        await client.stop_notify(STREAM_SPECS[name][0])
                    except Exception:  # noqa: BLE001 - teardown is best effort
                        pass
        return 0

    async def _pump(self, client: BleakClient) -> None:
        """Idle while the notification callbacks do the logging."""
        assert self.started is not None
        deadline = (
            None if self.args.seconds is None else self.started + self.args.seconds
        )
        while client.is_connected:
            await asyncio.sleep(0.05)
            if deadline is not None and time.monotonic() >= deadline:
                return
        print("Disconnected by the peer.", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0] if __doc__ else None
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
        "--streams",
        type=parse_streams,
        default=list(STREAM_SPECS),
        help=f"comma-separated subset of {', '.join(STREAM_SPECS)} (default: all)",
    )
    parser.add_argument(
        "--window",
        type=float,
        default=5.0,
        help="width of the rolling time window, seconds (default: %(default)s). "
        "Applies to the two plots and the button state timeline; the button "
        "event log is left unwindowed.",
    )
    parser.add_argument(
        "--accel-batch-time",
        choices=("arrival", "spread"),
        default="spread",
        help="place every sample of an accel batch at the notification's arrival "
        "time (default), or spread it back over the batch at the firmware's "
        "100 Hz so the waveform is continuous",
    )
    parser.add_argument(
        "--app-id",
        default="microbit v2",
        help='rerun application id (default: "%(default)s")',
    )
    parser.add_argument(
        "--spawn",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="spawn the rerun viewer (default: yes; --no-spawn for headless)",
    )
    parser.add_argument(
        "--save",
        default=None,
        help="also write the session to this .rrd file for later replay",
    )
    parser.add_argument(
        "--memory-limit",
        default="75%",
        help="viewer in-memory store limit (default: %(default)s)",
    )
    parser.add_argument(
        "--print-events",
        action="store_true",
        help="also print button events to stdout as they arrive",
    )
    args = parser.parse_args(argv)

    rr.init(args.app_id)
    # Saving and spawning are alternative sinks. Spawn explicitly rather than via
    # rr.init(spawn=True), whose bool form cannot forward the memory limit.
    if args.save is not None:
        rr.save(args.save)
    elif args.spawn:
        rr.spawn(memory_limit=args.memory_limit)
    # make_active overrides whatever layout the viewer last had for this app id,
    # including one the user rearranged by hand, so --window always takes effect.
    rr.send_blueprint(
        build_blueprint(args.streams, args.window),
        make_active=True,
        make_default=True,
    )
    log_styles(args.streams)

    viewer = Viewer(args)
    try:
        return asyncio.run(viewer.run())
    except KeyboardInterrupt:
        # asyncio.run cancels the task first, so the client has already torn down.
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
