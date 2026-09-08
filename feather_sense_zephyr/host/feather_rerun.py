#!/usr/bin/env python3
"""Live rerun visualization of the Feather Sense streams, over either transport.

Reuses the readers' own transports and decoders -- ``read_serial.SerialLink``
and ``read_ble.BleLink`` -- so the wire format has exactly one host-side
definition, in ``feather_protocol.py``, and this file has none of it.

**Unit conversion happens here, from the scale table the device reported over
RPC.** There is no hard-coded conversion factor in this viewer: it asks the
board what a raw count means and multiplies. That is the point of the `get
scale` opcode, and the cost of it is that this program cannot plot a capture it
did not ask the scales for.

The board is identified the way the readers identify it -- `get serial` and
`get build id`, printed as a header before anything is plotted -- so a recording
can be tied to the firmware that produced it.

Two tabs, split by what the view has to be rather than by rate class alone: the
continuously streaming signals are plots, and the two streams that report
*events* -- battery and button -- are text. Battery is seeded at startup with a
`get battery` RPC, because the stream itself only emits on a filtered 1 %
change and can be quiet for many minutes on a resting board.

Batched samples are back-dated using the batch's own ``period_us``, so unlike
the micro:bit viewer there is no ``--accel-batch-time`` flag to choose and no
nominal rate to assume.

    pixi run viz --transport ble --window 10
    pixi run viz --transport serial --seconds 30
    pixi run viz --no-spawn --save run.rrd
"""

from __future__ import annotations

import argparse
import asyncio
import math
import sys
import time

import rerun as rr
import rerun.blueprint as rrb

import feather_protocol as fp
from read_ble import BleLink
from read_serial import SerialLink

TIMELINE = "time"

# One colour per axis, shared by the IMU and the magnetometer so the same
# physical direction reads the same way in both views.
AXIS_COLORS = {
    "x": (222, 92, 92),
    "y": (92, 190, 122),
    "z": (92, 140, 222),
    "magnitude": (200, 200, 200),
}

# The three environmental sensors get a plot each rather than one shared view:
# they have no axis in common -- degrees, percent relative humidity, and a raw
# clear-channel count that runs to five figures -- so a single view scaled to
# whichever was largest and flattened the other two.
ENV_SENSORS = {
    "temperature": ("temperature (°C)", (224, 140, 92)),
    "humidity": ("humidity (%RH)", (92, 170, 222)),
    # Not lux: SENSOR_CHAN_LIGHT on the APDS9960 is the raw clear-channel
    # count, which is why the scale table reports it dimensionless.
    "light": ("light (raw count)", (222, 200, 92)),
}


def _window(seconds: float) -> rrb.VisibleTimeRange:
    """A fixed-width window that follows the play cursor rather than the data."""
    return rrb.VisibleTimeRange(
        TIMELINE,
        start=rrb.TimeRangeBoundary.cursor_relative(seconds=-seconds),
        end=rrb.TimeRangeBoundary.cursor_relative(),
    )


def build_blueprint(seconds: float) -> rrb.Blueprint:
    """Two tabs: the motion streams, and the slow ones.

    Neither text view takes a ``time_ranges``: rerun 0.36's viewer gates
    windowing per view class and only the time-series class implements it, so a
    ``VisibleTimeRange`` set on one of these would land in the blueprint and be
    ignored. That costs nothing here -- both show a latest value or a scrolling
    log rather than a span -- but it is why ``--window`` reaches six of the
    eight views and not all of them.
    """
    motion = rrb.Grid(
        rrb.TimeSeriesView(
            origin="accel", name="acceleration (m/s²)", time_ranges=[_window(seconds)]
        ),
        rrb.TimeSeriesView(
            origin="gyro", name="angular rate (rad/s)", time_ranges=[_window(seconds)]
        ),
        rrb.TimeSeriesView(
            origin="magn", name="magnetic field (µT)", time_ranges=[_window(seconds)]
        ),
        # A press is an event, not a signal: the log says when the button
        # changed and to what, where plotting it meant a line that is 0 or 1
        # and a y-axis pinned by hand so the first arriving value did not
        # autoscale the view to itself.
        rrb.TextLogView(origin="button", name="button events"),
        name="motion",
    )
    environment = rrb.Grid(
        *[
            rrb.TimeSeriesView(
                origin=f"env/{name}", name=title, time_ranges=[_window(seconds)]
            )
            for name, (title, _) in ENV_SENSORS.items()
        ],
        # Battery arrives on a filtered 1 % change -- minutes apart, and
        # 1.03-1.09 times per real percent point -- so a plot of it is mostly
        # empty axis. The latest reading is the whole of what there is to say.
        rrb.TextDocumentView(origin="battery", name="battery"),
        name="environment",
    )
    return rrb.Blueprint(rrb.Tabs(motion, environment), collapse_panels=True)


def log_styles() -> None:
    """Static styling, logged once rather than per sample."""
    for axis, color in AXIS_COLORS.items():
        for group in ("accel", "gyro", "magn"):
            rr.log(
                f"{group}/{axis}",
                rr.SeriesLines(names=axis, colors=color, widths=1.5),
                static=True,
            )
    for name, (_, color) in ENV_SENSORS.items():
        rr.log(
            f"env/{name}",
            rr.SeriesLines(names=name, colors=color, widths=1.5),
            static=True,
        )


class Viewer:
    """Decodes batches with the device's own scale table and logs them."""

    def __init__(self, scales: fp.ScaleTable) -> None:
        self._scales = scales
        self._t0_ms: float | None = None

    def _set_time(self, device_ms: float) -> None:
        """Device uptime, rebased so the session starts at zero.

        The device timeline is what everything is plotted against -- it needs no
        clock synchronisation, and it is the same timeline the rate numbers in
        the readers are computed on.
        """
        if self._t0_ms is None:
            self._t0_ms = device_ms
        rr.set_time(TIMELINE, duration=(device_ms - self._t0_ms) / 1000.0)

    def _log_battery(self, value: dict[str, float]) -> None:
        """The battery box: percent, the volts behind it, and the supply.

        ``flags`` is deliberately unfiltered on the device, so an unplug shows
        up here at once even though the percent it travels with does not.
        """
        usb = int(value["flags"]) & fp.BATTERY_FLAG_USB
        rr.log(
            "battery",
            rr.TextDocument(
                f"# {value['percent']:.0f} %\n\n"
                f"{value['millivolts']:.3f} V\n\n"
                f"powered from {'USB' if usb else 'the battery'}",
                media_type=rr.MediaType.MARKDOWN,
            ),
        )

    def on_battery_reply(self, response: fp.RpcResponse) -> None:
        """Seed the box from `get battery`, so it is not blank until a change.

        Logged at time zero rather than statically: static data outranks
        temporal data in rerun, so a static seed would shadow every reading the
        stream later sends and the box would never update again.
        """
        if not response.ok:
            print(f"get battery failed with status {response.status}")
            return
        raw = fp.parse_battery_payload(response.payload)
        rr.set_time(TIMELINE, duration=0.0)
        self._log_battery(self._scales.decode_sample(fp.STREAM_BATTERY, raw))

    def on_batch(self, batch: fp.Batch) -> None:
        values = self._scales.decode(batch)
        stamps = batch.timestamps_ms()

        for value, stamp in zip(values, stamps):
            self._set_time(stamp)

            if batch.stream_id == fp.STREAM_IMU:
                for axis in ("x", "y", "z"):
                    rr.log(f"accel/{axis}", rr.Scalars(value[f"a{axis}"]))
                    rr.log(f"gyro/{axis}", rr.Scalars(value[f"g{axis}"]))
                rr.log(
                    "accel/magnitude",
                    rr.Scalars(
                        math.sqrt(
                            value["ax"] ** 2 + value["ay"] ** 2 + value["az"] ** 2
                        )
                    ),
                )
            elif batch.stream_id == fp.STREAM_MAGN:
                # The scale table reports tesla; microtesla is what anyone
                # reading a magnetometer plot expects to see.
                for axis in ("x", "y", "z"):
                    rr.log(f"magn/{axis}", rr.Scalars(value[axis] * 1e6))
                rr.log(
                    "magn/magnitude",
                    rr.Scalars(
                        math.sqrt(value["x"] ** 2 + value["y"] ** 2 + value["z"] ** 2)
                        * 1e6
                    ),
                )
            elif batch.stream_id == fp.STREAM_ENV:
                for name in ENV_SENSORS:
                    rr.log(f"env/{name}", rr.Scalars(value[name]))
            elif batch.stream_id == fp.STREAM_BATTERY:
                self._log_battery(value)
            elif batch.stream_id == fp.STREAM_BUTTON:
                pressed = bool(value["pressed"])
                rr.log(
                    "button/events",
                    rr.TextLog(
                        f"key {int(value['code'])} "
                        f"{'pressed' if pressed else 'released'}",
                        level=rr.TextLogLevel.INFO,
                    ),
                )


async def run_ble(args: argparse.Namespace) -> int:
    from bleak import BleakClient

    address = args.address or await BleLink.find()
    print(f"connecting to {address}")

    async with BleakClient(address) as client:
        link = BleLink(client)
        print("connected")
        await link.start_rpc()
        device_id, build_id = await link.identify()
        print(f"serial    {device_id}")
        print(f"build     {build_id}")
        scales = await link.fetch_scales()
        print("scales:")
        print(scales.describe())

        viewer = Viewer(scales)
        viewer.on_battery_reply(await link.call(fp.OP_GET_BATTERY))
        await link.subscribe(viewer.on_batch)

        started = time.monotonic()
        while args.seconds is None or time.monotonic() - started < args.seconds:
            await asyncio.sleep(0.2)

    return 0


def run_serial(args: argparse.Namespace) -> int:
    with SerialLink(args.port) as link:
        print(f"data port {link.port}")
        device_id, build_id = link.identify()
        print(f"serial    {device_id}")
        print(f"build     {build_id}")
        scales = link.fetch_scales()
        print("scales:")
        print(scales.describe())

        viewer = Viewer(scales)
        viewer.on_battery_reply(link.call(fp.OP_GET_BATTERY))
        started = time.monotonic()
        while args.seconds is None or time.monotonic() - started < args.seconds:
            for batch in link.read_batches():
                viewer.on_batch(batch)

    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--transport",
        choices=("serial", "ble"),
        default="serial",
        help="which link to read (default: serial)",
    )
    parser.add_argument("--seconds", type=float, help="stop after this long")
    parser.add_argument(
        "--window", type=float, default=5.0, help="plot window, seconds"
    )
    parser.add_argument("--port", help="serial: override the resolved data port")
    parser.add_argument("--address", help="ble: skip the scan and use this address")
    parser.add_argument(
        "--app-id", default="feather_sense", help="rerun application id"
    )
    parser.add_argument("--save", help="write an .rrd instead of spawning a viewer")
    parser.add_argument(
        "--spawn",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="spawn the rerun viewer (default: yes)",
    )
    parser.add_argument(
        "--memory-limit",
        default="2GB",
        help="viewer memory limit, passed to rr.spawn",
    )
    args = parser.parse_args(argv)

    rr.init(args.app_id)
    # Saving and spawning are alternative sinks. Spawn explicitly rather than
    # via rr.init(spawn=True), whose bool form cannot forward the memory limit.
    if args.save is not None:
        rr.save(args.save)
    elif args.spawn:
        rr.spawn(memory_limit=args.memory_limit)
    # make_active overrides whatever layout the viewer last had for this app id,
    # including one rearranged by hand, so --window always takes effect.
    rr.send_blueprint(build_blueprint(args.window), make_active=True, make_default=True)
    log_styles()

    try:
        if args.transport == "ble":
            return asyncio.run(run_ble(args))
        return run_serial(args)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
