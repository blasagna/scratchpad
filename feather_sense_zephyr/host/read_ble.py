#!/usr/bin/env python3
"""Measure the Feather Sense's BLE link, and own the BLE transport.

Four notify characteristics, one per rate class, so this subscribes to only what
it needs; plus the RPC pair. ``feather_rerun.py`` imports :class:`BleLink` from
here rather than opening its own connection.

Two calibrations from the CircuitPython port, which ran the same link from the
same board and host: it sustained roughly **110 notifications per second** at
the default 23-byte ATT MTU, saturating around 100 Hz of IMU (~4.4 KB/s); and
requesting the 7.5 ms minimum connection interval measured *identical* to
leaving the negotiated default alone, so that code was deleted rather than kept
as a plausible-looking no-op. The expected win here is therefore MTU and the
nRF52840's 2M PHY, not interval tuning -- and if raising the MTU does not move
the number, the next thing to check is the notification rate, not the interval.

    pixi run ble --seconds 20
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

import feather_protocol as fp


class BleLink:
    """A connected board: subscriptions in, RPC both ways."""

    def __init__(self, client: BleakClient) -> None:
        self._client = client
        self._rpc_seq = 0
        self._responses: asyncio.Queue[fp.RpcResponse] = asyncio.Queue()
        self.errors = 0

    @staticmethod
    async def find(name: str = fp.BLE_DEVICE_NAME, timeout: float = 15.0) -> str:
        device = await BleakScanner.find_device_by_filter(
            lambda d, _: (d.name or "") == name, timeout=timeout
        )
        if device is None:
            raise RuntimeError(
                f"no device advertising as {name!r} within {timeout:g}s. "
                "Is the board powered and not already connected elsewhere?"
            )
        return device.address

    async def start_rpc(self) -> None:
        await self._client.start_notify(fp.UUID_RPC_RESPONSE, self._on_rpc)

    def _on_rpc(self, _sender: object, data: bytearray) -> None:
        try:
            self._responses.put_nowait(fp.parse_response(bytes(data)))
        except ValueError:
            self.errors += 1

    async def call(
        self, opcode: int, args: bytes = b"", timeout: float = 3.0
    ) -> fp.RpcResponse:
        self._rpc_seq = (self._rpc_seq + 1) & 0xFF
        seq = self._rpc_seq
        await self._client.write_gatt_char(
            fp.UUID_RPC_REQUEST, fp.build_request(seq, opcode, args), response=True
        )

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                response = await asyncio.wait_for(self._responses.get(), remaining)
            except asyncio.TimeoutError:
                break
            if response.seq == seq and response.opcode == opcode:
                return response

        raise TimeoutError(f"no reply to opcode 0x{opcode:02x} seq {seq}")

    async def fetch_scales(self) -> fp.ScaleTable:
        table = fp.ScaleTable()
        for stream_id in fp.STREAM_NAMES:
            response = await self.call(fp.OP_GET_SCALE, bytes([stream_id]))
            if not response.ok:
                raise RuntimeError(
                    f"get scale for stream {stream_id} failed with status {response.status}"
                )
            got, scales = fp.parse_scale_payload(response.payload)
            table.by_stream[got] = scales
        return table

    async def identify(self) -> tuple[str, str]:
        serial_response = await self.call(fp.OP_GET_SERIAL)
        build_response = await self.call(fp.OP_GET_BUILD_ID)
        return (
            serial_response.payload.hex().upper(),
            build_response.payload.decode("utf-8", "replace"),
        )

    async def subscribe(self, on_batch) -> None:
        """Subscribe to all four sample characteristics.

        `stream_id` is in the header even though the characteristic already
        implies it, so the bytes on BLE and on USB are the same bytes and there
        is one decoder for both.
        """

        def handler(_sender: object, data: bytearray) -> None:
            try:
                on_batch(fp.parse_batch(bytes(data)))
            except ValueError:
                self.errors += 1

        for uuid in fp.BLE_STREAM_CHARACTERISTICS:
            await self._client.start_notify(uuid, handler)

    @property
    def mtu(self) -> int:
        return self._client.mtu_size


async def run(address: str | None, seconds: float) -> int:
    address = address or await BleLink.find()
    print(f"connecting to {address}")

    async with BleakClient(address) as client:
        link = BleLink(client)
        # The MTU governs how many IMU samples the board puts in one
        # notification: (mtu - 3 - 10) / 12. At 247 that is 19.
        print(f"connected, ATT MTU {link.mtu}")

        await link.start_rpc()
        device_id, build_id = await link.identify()
        print(f"serial    {device_id}")
        print(f"build     {build_id}")
        scales = await link.fetch_scales()
        print("scales (value_in_SI = raw * num / den / 1e9):")
        print(scales.describe())
        print()

        stats = {s: fp.StreamStats(name) for s, name in fp.STREAM_NAMES.items()}

        def on_batch(batch: fp.Batch) -> None:
            stats[batch.stream_id].add(batch)

        await link.subscribe(on_batch)

        started = time.monotonic()
        window_start = started
        while time.monotonic() - started < seconds:
            await asyncio.sleep(0.2)
            now = time.monotonic()
            elapsed = now - window_start
            if elapsed < 1.0:
                continue

            print(f"[{now - started:5.1f}s] errors {link.errors}")
            for stat in stats.values():
                if stat.samples:
                    print(stat.line(elapsed))
                stat.reset()
            window_start = now

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--seconds", type=float, default=20.0, help="how long to measure"
    )
    parser.add_argument("--address", help="skip the scan and connect to this address")
    args = parser.parse_args()

    return asyncio.run(run(args.address, args.seconds))


if __name__ == "__main__":
    sys.exit(main())
