#!/usr/bin/env python3
"""Measure the Feather Sense's USB CDC link, and own the serial transport.

The board presents two CDC ACM ports: the Zephyr console/shell/log, and a
second one carrying nothing but binary sample frames. Which ``/dev/ttyACM*`` is
which follows enumeration order, so this resolves the data port by its **USB
interface string descriptor** rather than guessing -- the ``label`` properties
in ``../app.overlay`` name them "Feather Sense data" and "Feather Sense
console", and Linux exposes that string at
``/sys/class/tty/ttyACM*/device/interface``.

``feather_rerun.py`` imports :class:`SerialLink` from here rather than opening
its own port, the same way ``microbit_v2_zephyr/host/ble_rerun.py`` imports
``ble_stream``.

    pixi run serial --seconds 20
    pixi run serial --seconds 5400 --window 60   # a soak; see "a long run" in ../README.md

Every windowed figure is also accumulated over the run and printed as a total
when the run ends, including on a Ctrl-C. The windowed lines say whether the
link is healthy now; only the totals say whether it stayed healthy.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Iterator

import serial

import feather_protocol as fp

DATA_INTERFACE = "Feather Sense data"  # Must match ../app.overlay: cdc_acm_data label
CONSOLE_INTERFACE = "Feather Sense console"


def find_port(interface: str = DATA_INTERFACE) -> str:
    """The /dev/ttyACM* whose USB interface string descriptor is `interface`."""
    matches = []
    for tty in sorted(Path("/sys/class/tty").glob("ttyACM*")):
        name = tty / "device" / "interface"
        try:
            if name.read_text().strip() == interface:
                matches.append(f"/dev/{tty.name}")
        except OSError:
            continue

    if not matches:
        raise RuntimeError(
            f"no serial port advertises the interface string {interface!r}. "
            "Is the board plugged in and running this firmware? "
            "`ls /sys/class/tty/ttyACM*/device/interface` shows what is there."
        )
    if len(matches) > 1:
        raise RuntimeError(f"more than one port claims to be {interface!r}: {matches}")

    return matches[0]


class SerialLink:
    """The data port: RPC in both directions, sample batches inbound."""

    def __init__(self, port: str | None = None, timeout: float = 0.05) -> None:
        self.port = port or find_port()
        # The baud rate is ignored -- this is a USB CDC endpoint, not a real
        # UART -- but every tool wants one.
        self._serial = serial.Serial(self.port, 115200, timeout=timeout)
        self._decoder = fp.FrameDecoder()
        self._pending: list[fp.RpcResponse] = []
        self._rpc_seq = 0

    def close(self) -> None:
        self._serial.close()

    def __enter__(self) -> "SerialLink":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    @property
    def errors(self) -> int:
        return self._decoder.errors

    def read_batches(self) -> Iterator[fp.Batch]:
        """Drain whatever has arrived, yielding decoded sample batches.

        RPC responses on the same pipe are set aside for :meth:`call` rather
        than dropped, so a reply that arrives mid-stream is not lost.
        """
        chunk = self._serial.read(4096)
        if not chunk:
            return
        for channel, payload in self._decoder.feed(chunk):
            if channel == fp.CHANNEL_RPC:
                try:
                    self._pending.append(fp.parse_response(payload))
                except ValueError:
                    self._decoder.errors += 1
                continue
            if channel != fp.CHANNEL_SAMPLES:
                self._decoder.errors += 1
                continue
            try:
                yield fp.parse_batch(payload)
            except ValueError:
                self._decoder.errors += 1

    def call(
        self, opcode: int, args: bytes = b"", timeout: float = 2.0
    ) -> fp.RpcResponse:
        """One request, one matching reply. Raises TimeoutError if none comes."""
        self._rpc_seq = (self._rpc_seq + 1) & 0xFF
        seq = self._rpc_seq
        self._serial.write(
            fp.frame(fp.CHANNEL_RPC, fp.build_request(seq, opcode, args))
        )
        self._serial.flush()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for response in list(self._pending):
                if response.seq == seq and response.opcode == opcode:
                    self._pending.remove(response)
                    return response
            # Draining batches is what advances the decoder; discard them here.
            for _ in self.read_batches():
                pass

        raise TimeoutError(f"no reply to opcode 0x{opcode:02x} seq {seq}")

    def fetch_scales(self) -> fp.ScaleTable:
        """The scale table for every stream. Decoding depends on it, so both
        readers fetch it at connect and print it before streaming.
        """
        table = fp.ScaleTable()
        for stream_id in fp.STREAM_NAMES:
            response = self.call(fp.OP_GET_SCALE, bytes([stream_id]))
            if not response.ok:
                raise RuntimeError(
                    f"get scale for stream {stream_id} failed with status {response.status}"
                )
            got, scales = fp.parse_scale_payload(response.payload)
            if got != stream_id:
                raise RuntimeError(f"asked for stream {stream_id}, got {got}")
            table.by_stream[stream_id] = scales
        return table

    def identify(self) -> tuple[str, str]:
        """(serial number, build id), for the header both readers print."""
        serial_response = self.call(fp.OP_GET_SERIAL)
        build_response = self.call(fp.OP_GET_BUILD_ID)
        return (
            serial_response.payload.hex().upper(),
            build_response.payload.decode("utf-8", "replace"),
        )


def summarize(
    stats: dict[int, fp.StreamStats], link: SerialLink, elapsed_s: float
) -> None:
    """Print the run totals. Divided by *measured* elapsed, as always."""
    print()
    print(f"=== run total over {elapsed_s:.1f}s, decode errors {link.errors} ===")
    for stat in stats.values():
        if stat.total_samples:
            print(stat.total_line(elapsed_s))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--seconds", type=float, default=20.0, help="how long to measure"
    )
    # A soak wants totals, not 5400 windowed lines. The window is still measured
    # rather than assumed, so widening it changes what is printed and not how
    # any rate is computed.
    parser.add_argument(
        "--window",
        type=float,
        default=1.0,
        help="seconds between windowed reports (default 1)",
    )
    parser.add_argument("--port", help="override the resolved data port")
    args = parser.parse_args()

    with SerialLink(args.port) as link:
        print(f"data port {link.port}")

        device_id, build_id = link.identify()
        print(f"serial    {device_id}")
        print(f"build     {build_id}")
        scales = link.fetch_scales()
        print("scales (value_in_SI = raw * num / den / 1e9):")
        print(scales.describe())
        print()

        stats = {s: fp.StreamStats(name) for s, name in fp.STREAM_NAMES.items()}
        started = time.monotonic()
        window_start = started

        # A soak is most likely to end with a Ctrl-C, and a run that throws away
        # its totals on the way out has measured nothing.
        try:
            while time.monotonic() - started < args.seconds:
                for batch in link.read_batches():
                    stats[batch.stream_id].add(batch)

                now = time.monotonic()
                elapsed = now - window_start
                if elapsed < args.window:
                    continue

                # Divided by *measured* elapsed, never by the nominal window.
                print(f"[{now - started:7.1f}s] errors {link.errors}", flush=True)
                for stat in stats.values():
                    if stat.samples:
                        print(stat.line(elapsed))
                    stat.reset()
                window_start = now
        except KeyboardInterrupt:
            print()
            print("interrupted", flush=True)

        summarize(stats, link, time.monotonic() - started)

    return 0


if __name__ == "__main__":
    sys.exit(main())
