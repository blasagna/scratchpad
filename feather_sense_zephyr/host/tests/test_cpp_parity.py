"""Cross-language parity: the Python decoder against the firmware's encoder.

``../../src/codec.cpp`` is the device's definition of the wire format and
``../feather_protocol.py`` is the host's. Both are hand-written, and until now
only the C++ one was tested -- ``../../tests/codec/`` pins its byte layout
against literals, and nothing at all pinned the Python.

Testing the Python against literals of its own would have been the weaker
thing, for the reason ``../../tests/codec/src/main.cpp`` already gives about
round-trips: a suite that only checks a module against a transcription of what
its author believed agrees with any layout both were wrong about together. So
this compiles ``codec.cpp`` with the host compiler -- which it supports, being
free of Zephyr headers so that ``native_sim`` can build it -- runs
``gen_vectors.cpp`` against it, and requires ``feather_protocol`` to produce
the same bytes and to decode them back.

Skipped, not failed, where there is no C++ compiler: the suite in
``test_feather_protocol.py`` runs everywhere and this one sharpens it.

    pixi run test
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import feather_protocol as fp


def _unhex(field: str) -> bytes:
    """The generator writes "-" for an empty byte string; see gen_vectors.cpp."""
    return b"" if field == "-" else bytes.fromhex(field)


_HERE = Path(__file__).resolve().parent
_AREA = _HERE.parent.parent
_CODEC_CPP = _AREA / "src" / "codec.cpp"
_GENERATOR = _HERE / "gen_vectors.cpp"

# Must match ../../src/codec.hpp. The generator prints each of these from the
# header itself, so the pairing is what is under test, not the value.
_CONSTANTS = {
    "channel_samples": lambda: fp.CHANNEL_SAMPLES,
    "channel_rpc": lambda: fp.CHANNEL_RPC,
    "battery_flag_usb": lambda: fp.BATTERY_FLAG_USB,
    "op_get_battery": lambda: fp.OP_GET_BATTERY,
    "op_set_stream": lambda: fp.OP_SET_STREAM,
    "op_get_scale": lambda: fp.OP_GET_SCALE,
    "op_get_serial": lambda: fp.OP_GET_SERIAL,
    "op_get_build_id": lambda: fp.OP_GET_BUILD_ID,
}

# The unit enum, by the generator's name for each row.
_UNITS = {
    "unit_ms2": "m/s^2",
    "unit_rad_s": "rad/s",
    "unit_tesla": "T",
    "unit_degc": "degC",
    "unit_rh": "%RH",
    "unit_lux": "lx",
    "unit_volts": "V",
    "unit_percent": "%",
    "unit_dimensionless": "",
}


def _build_and_run() -> list[list[str]]:
    """Compile the generator against the firmware's codec.cpp and run it."""
    compiler = shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        raise unittest.SkipTest(
            "no C++ compiler; parity against the firmware unchecked"
        )

    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "gen_vectors"
        # -Werror deliberately: this builds the firmware's own translation unit,
        # and a warning here is a warning the Zephyr build would also take.
        built = subprocess.run(
            [
                compiler,
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(_AREA / "src"),
                str(_GENERATOR),
                str(_CODEC_CPP),
                "-o",
                str(binary),
            ],
            capture_output=True,
            text=True,
        )
        if built.returncode != 0:
            # Not check=True: CalledProcessError prints the return code and
            # swallows the diagnostics, and a compile failure here is usually a
            # real change to codec.cpp that wants reading.
            raise AssertionError(
                f"compiling the firmware's codec.cpp failed:\n{built.stderr}"
            )
        out = subprocess.run(
            [str(binary)], check=True, capture_output=True, text=True
        ).stdout

    return [line.split() for line in out.splitlines() if line.strip()]


class CppParity(unittest.TestCase):
    """Every assertion here compares against bytes the firmware code produced."""

    vectors: list[list[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.vectors = _build_and_run()

    def rows(self, kind: str) -> list[list[str]]:
        found = [row[1:] for row in self.vectors if row[0] == kind]
        self.assertTrue(found, f"the generator emitted no {kind!r} rows")
        return found

    def test_sizes_match_the_firmware_header(self) -> None:
        sizes = {name: int(value) for name, value in self.rows("size")}

        self.assertEqual(fp.HEADER_BYTES, sizes["header"])
        self.assertEqual(fp.SCALE_FIELD_BYTES, sizes["scale_field"])
        # Private on purpose -- it is an implementation detail of the encoder,
        # but it is the same detail on both sides and worth pinning across them.
        self.assertEqual(fp._COBS_BLOCK, sizes["cobs_block"])

        for stream_id, expected in fp.STREAM_SAMPLE_BYTES.items():
            self.assertEqual(
                expected,
                sizes[f"stream_{stream_id}"],
                f"{fp.STREAM_NAMES[stream_id]} sample size disagrees with codec.hpp",
            )

        # Neither side may carry a stream the other does not know about.
        emitted = {
            int(n.removeprefix("stream_")) for n in sizes if n.startswith("stream_")
        }
        self.assertEqual(emitted, set(fp.STREAM_SAMPLE_BYTES))
        self.assertEqual(emitted, set(fp.STREAM_NAMES))
        self.assertEqual(emitted, set(fp.STREAM_FIELDS))

    def test_constants_match_the_firmware_header(self) -> None:
        emitted = {name: int(value) for name, value in self.rows("const")}

        for name, get in _CONSTANTS.items():
            self.assertEqual(get(), emitted[name], f"{name} disagrees with codec.hpp")

        for name, unit_name in _UNITS.items():
            self.assertEqual(
                fp.UNIT_NAMES[emitted[name]],
                unit_name,
                f"{name} is not the unit feather_protocol names",
            )

    def test_cobs_encoding_is_byte_identical(self) -> None:
        for plain_hex, encoded_hex in self.rows("cobs"):
            plain = _unhex(plain_hex)
            encoded = _unhex(encoded_hex)
            with self.subTest(length=len(plain)):
                self.assertEqual(fp.cobs_encode(plain), encoded)
                self.assertEqual(fp.cobs_decode(encoded), plain)
                self.assertNotIn(
                    0, encoded, "an encoding with a zero in it is not a frame"
                )

    def test_batches_frame_and_decode_the_way_the_firmware_wrote_them(self) -> None:
        for row in self.rows("batch"):
            t_ms, seq, period_us, stream_id, count, channel = (int(v) for v in row[:6])
            plain = _unhex(row[6])
            encoded = _unhex(row[7])
            payload = plain[1:]

            with self.subTest(stream=fp.STREAM_NAMES[stream_id], count=count):
                self.assertEqual(plain[0], channel)
                # What usb.cpp puts on the wire, delimiter and all.
                self.assertEqual(fp.frame(channel, payload), encoded + b"\x00")

                decoder = fp.FrameDecoder()
                got = list(decoder.feed(encoded + b"\x00"))
                self.assertEqual(decoder.errors, 0)
                self.assertEqual(got, [(channel, payload)])

                batch = fp.parse_batch(payload)
                self.assertEqual(batch.header.t_ms, t_ms)
                self.assertEqual(batch.header.seq, seq)
                self.assertEqual(batch.header.period_us, period_us)
                self.assertEqual(batch.header.stream_id, stream_id)
                self.assertEqual(batch.header.count, count)
                self.assertEqual(len(batch.samples), count)
                # One value per named field, so STREAM_FIELDS and the struct
                # format cannot drift apart.
                for sample in batch.samples:
                    self.assertEqual(len(sample), len(fp.STREAM_FIELDS[stream_id]))

    def test_scale_fields_parse_to_what_the_firmware_packed(self) -> None:
        for unit_s, num_s, den_s, hex_s in self.rows("scale"):
            unit, num, den = int(unit_s), int(num_s), int(den_s)
            packed = _unhex(hex_s)

            with self.subTest(unit=fp.UNIT_NAMES[unit], num=num, den=den):
                self.assertEqual(len(packed), fp.SCALE_FIELD_BYTES)

                # Parsed as a one-field `get scale` reply, which is the only
                # way these bytes ever reach the host.
                payload = bytes([fp.STREAM_IMU, 1]) + packed
                stream_id, scales = fp.parse_scale_payload(payload)
                self.assertEqual(stream_id, fp.STREAM_IMU)
                self.assertEqual(scales, [fp.Scale(unit=unit, num=num, den=den)])
                self.assertEqual(
                    struct.pack(fp._SCALE_FIELD_FORMAT, unit, num, den), packed
                )


if __name__ == "__main__":
    unittest.main()
