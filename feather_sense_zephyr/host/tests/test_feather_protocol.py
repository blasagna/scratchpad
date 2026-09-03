"""The host's wire-format module, on its own terms.

``test_cpp_parity.py`` holds this module to bytes the firmware's encoder
actually produced, which is the stronger check wherever it reaches. It does not
reach everything, and this suite covers the rest:

- **Behaviour the device has no opinion about** -- how a malformed frame is
  counted and resynchronised, what a truncated batch raises, how rates are
  computed. ``codec.cpp`` encodes; none of this is its business.
- **Per-stream field signedness**, which is a genuine gap in the parity test
  rather than a division of labour. ``sample_bytes()`` lives in ``codec.cpp``
  and is checked over there, but the ``Sample`` structs that say which fields
  are signed live in ``../../src/env.cpp`` and its siblings, behind Zephyr
  headers the standalone parity build cannot compile. So the pin below is a pin
  against *this* file changing, not a cross-check: flipping ``"<hHH"`` to
  ``"<hhH"`` passes the parity suite, and only the assertions here object.
- **The 16-bit ``seq`` wrap**, which the arithmetic has always handled and no
  run was ever long enough to reach.

    pixi run test
"""

from __future__ import annotations

import struct
import unittest

import feather_protocol as fp


def make_batch(
    stream_id: int = fp.STREAM_IMU,
    t_ms: int = 0,
    seq: int = 0,
    period_us: int = 4808,
    count: int = 1,
    body: bytes | None = None,
) -> fp.Batch:
    """A decoded batch with a deterministic body, for the stats and decoders."""
    size = fp.STREAM_SAMPLE_BYTES[stream_id]
    if body is None:
        body = bytes((i * 11) & 0xFF for i in range(size * count))
    header = fp.BatchHeader(
        t_ms=t_ms, seq=seq, period_us=period_us, stream_id=stream_id, count=count
    )
    return fp.parse_batch(struct.pack("<IHHBB", *header) + body)


class Cobs(unittest.TestCase):
    def test_round_trips_every_length_and_leaves_no_zero_byte(self) -> None:
        for length in range(0, 300):
            data = bytes((i * 7 + 1) & 0xFF for i in range(length))
            with self.subTest(length=length):
                encoded = fp.cobs_encode(data)
                self.assertNotIn(0, encoded)
                self.assertEqual(fp.cobs_decode(encoded), data)

    def test_round_trips_runs_of_zeros(self) -> None:
        """Zeros are the whole reason COBS is here, so be exhaustive about them."""
        for length in range(0, 300):
            with self.subTest(length=length):
                encoded = fp.cobs_encode(b"\x00" * length)
                self.assertNotIn(0, encoded)
                self.assertEqual(fp.cobs_decode(encoded), b"\x00" * length)

    def test_round_trips_the_block_split_boundary(self) -> None:
        """253/254/255 is where COBS implementations are merely usually right.

        A run that ends because it filled a block restores no zero on decode; a
        run that ended at a zero does. Getting that backwards is invisible until
        a payload crosses 254 bytes.
        """
        for length in (252, 253, 254, 255, 256, 507, 508, 509):
            for filler in (b"\xaa", b"\x00"):
                with self.subTest(length=length, filler=filler.hex()):
                    data = filler * length
                    self.assertEqual(fp.cobs_decode(fp.cobs_encode(data)), data)

        block_then_zero = b"\xaa" * 254 + b"\x00"
        self.assertEqual(
            fp.cobs_decode(fp.cobs_encode(block_then_zero)), block_then_zero
        )

    def test_rejects_malformed_frames(self) -> None:
        """A decoder that guesses turns a framing fault into plausible data."""
        with self.assertRaises(ValueError):
            fp.cobs_decode(b"\x00\x11")  # a zero code byte
        with self.assertRaises(ValueError):
            fp.cobs_decode(b"\x09\x11\x22")  # a run that overruns the frame


class Framing(unittest.TestCase):
    def test_reassembles_a_frame_split_across_reads(self) -> None:
        """A CDC read boundary lands wherever it likes; frames outlive it."""
        payload = bytes(range(40))
        wire = fp.frame(fp.CHANNEL_SAMPLES, payload)
        decoder = fp.FrameDecoder()

        got = []
        for i in range(len(wire)):
            got += list(decoder.feed(wire[i : i + 1]))

        self.assertEqual(got, [(fp.CHANNEL_SAMPLES, payload)])
        self.assertEqual(decoder.errors, 0)

    def test_separates_the_two_channels_on_one_pipe(self) -> None:
        """Batches and RPC replies share the pipe; only the channel byte differs."""
        decoder = fp.FrameDecoder()
        wire = fp.frame(fp.CHANNEL_SAMPLES, b"\x01\x02") + fp.frame(
            fp.CHANNEL_RPC, b"\x03"
        )

        self.assertEqual(
            list(decoder.feed(wire)),
            [(fp.CHANNEL_SAMPLES, b"\x01\x02"), (fp.CHANNEL_RPC, b"\x03")],
        )

    def test_ignores_empty_frames_and_counts_malformed_ones(self) -> None:
        decoder = fp.FrameDecoder()

        # Doubled and leading delimiters are not errors: they carry no frame.
        self.assertEqual(list(decoder.feed(b"\x00\x00")), [])
        self.assertEqual(decoder.errors, 0)

        # A frame whose COBS is malformed is counted, not raised.
        self.assertEqual(list(decoder.feed(b"\x09\x11\x22\x00")), [])
        self.assertEqual(decoder.errors, 1)

    def test_resynchronises_at_the_next_delimiter(self) -> None:
        """One bad frame must not cost the frame behind it."""
        decoder = fp.FrameDecoder()
        wire = b"\x09\x11\x22\x00" + fp.frame(fp.CHANNEL_SAMPLES, b"\xde\xad")

        self.assertEqual(list(decoder.feed(wire)), [(fp.CHANNEL_SAMPLES, b"\xde\xad")])
        self.assertEqual(decoder.errors, 1)


class ParseBatch(unittest.TestCase):
    def test_rejects_a_payload_shorter_than_a_header(self) -> None:
        with self.assertRaisesRegex(ValueError, "shorter than a header"):
            fp.parse_batch(b"\x00" * (fp.HEADER_BYTES - 1))

    def test_rejects_an_unknown_stream_id(self) -> None:
        header = struct.pack("<IHHBB", 0, 0, 0, 99, 0)
        with self.assertRaisesRegex(ValueError, "unknown stream id 99"):
            fp.parse_batch(header)

    def test_rejects_a_body_that_contradicts_the_count(self) -> None:
        """`count` and the byte length must agree, or the samples are guesses."""
        header = struct.pack("<IHHBB", 0, 0, 4808, fp.STREAM_IMU, 3)
        with self.assertRaisesRegex(ValueError, "declares 3 samples"):
            fp.parse_batch(
                header + b"\x00" * (fp.STREAM_SAMPLE_BYTES[fp.STREAM_IMU] * 2)
            )

    def test_every_stream_decodes_one_value_per_named_field(self) -> None:
        for stream_id in fp.STREAM_NAMES:
            with self.subTest(stream=fp.STREAM_NAMES[stream_id]):
                batch = make_batch(stream_id=stream_id, count=2)
                self.assertEqual(len(batch.samples), 2)
                for sample in batch.samples:
                    self.assertEqual(len(sample), len(fp.STREAM_FIELDS[stream_id]))

    def test_field_signedness_matches_what_the_firmware_packs(self) -> None:
        """The gap the parity suite cannot cover; see this module's docstring.

        Each case is the extreme that separates a signed reading from an
        unsigned one, against the struct in the firmware file named beside it.
        """
        # ../../src/imu.cpp: int16 gx,gy,gz,ax,ay,az -- raw registers, and gyro
        # at +-2000 dps needs the full signed range.
        imu = make_batch(stream_id=fp.STREAM_IMU, body=b"\x00\x80" * 6, count=1)
        self.assertEqual(imu.samples[0], (-32768,) * 6)

        # ../../src/magn.cpp: int16 x,y,z in deci-uT.
        magn = make_batch(stream_id=fp.STREAM_MAGN, body=b"\xff\x7f" * 3, count=1)
        self.assertEqual(magn.samples[0], (32767,) * 3)

        # ../../src/env.cpp:48 -- int16 temperature_centi_c, then uint16
        # humidity_centi_pct and uint16 light_level. Light is a raw clear-channel
        # count and does exceed 32767, so reading it signed is not academic.
        env = make_batch(
            stream_id=fp.STREAM_ENV, body=b"\x00\x80" + b"\xff\xff" * 2, count=1
        )
        self.assertEqual(env.samples[0], (-32768, 65535, 65535))

        # ../../src/battery.cpp: uint16 mv, uint8 percent, uint8 flags.
        battery = make_batch(
            stream_id=fp.STREAM_BATTERY, body=b"\xff\xff\x64\x01", count=1
        )
        self.assertEqual(battery.samples[0], (65535, 100, fp.BATTERY_FLAG_USB))

        # ../../src/buttons.cpp: uint16 code, uint8 pressed, uint8 pad.
        button = make_batch(
            stream_id=fp.STREAM_BUTTON, body=b"\xff\xff\x01\x00", count=1
        )
        self.assertEqual(button.samples[0], (65535, 1, 0))

    def test_period_us_cannot_express_a_spacing_slower_than_about_15_hz(self) -> None:
        """A live limitation, not a bug -- but an undocumented ceiling in code.

        `period_us` is a uint16, so 65535 us (~15.3 Hz) is the slowest spacing a
        *batched* stream can state. Nothing batched here is slower (the
        magnetometer is 50 000), and the unbatched streams send 0. A future
        5 Hz batched stream needs a wider field or a different unit.
        """
        make_batch(stream_id=fp.STREAM_MAGN, period_us=65535, count=2)

        with self.assertRaises(struct.error):
            make_batch(stream_id=fp.STREAM_MAGN, period_us=65536, count=2)

    def test_back_dates_samples_from_the_batch_timestamp(self) -> None:
        """`t_ms` is the first sample's instant, `period_us` the spacing."""
        batch = make_batch(t_ms=1000, period_us=4808, count=3)

        self.assertEqual(batch.timestamps_ms(), [1000, 1004.808, 1009.616])

    def test_an_unbatched_stream_needs_no_back_dating(self) -> None:
        batch = make_batch(stream_id=fp.STREAM_ENV, t_ms=500, period_us=0, count=1)

        self.assertEqual(batch.timestamps_ms(), [500])


class Stats(unittest.TestCase):
    def test_device_rate_divides_by_intervals_not_samples(self) -> None:
        """`count - 1`. Dividing by `count` is the ~10 % over-report."""
        stats = fp.StreamStats("magn")
        # 11 samples at the magnetometer's 50 ms spacing span 10 intervals, so
        # 500 ms of signal: 20/s. Dividing by the sample count says 22/s.
        stats.add(
            make_batch(stream_id=fp.STREAM_MAGN, t_ms=0, period_us=50_000, count=11)
        )

        self.assertAlmostEqual(stats.device_rate, 20.0)

    def test_one_sample_spans_no_intervals(self) -> None:
        stats = fp.StreamStats("env")
        stats.add(make_batch(stream_id=fp.STREAM_ENV, period_us=0, count=1))

        self.assertEqual(stats.device_rate, 0.0)

    def test_host_rate_uses_measured_elapsed(self) -> None:
        stats = fp.StreamStats("imu")
        stats.add(make_batch(count=10))

        # A window gated on >= 1.0 s is always overshot; 10 samples in a
        # measured 1.05 s is 9.52/s and must not round up to 10.
        self.assertAlmostEqual(stats.host_rate(1.05), 10 / 1.05)

    def test_counts_a_sequence_gap_as_the_number_of_lost_batches(self) -> None:
        stats = fp.StreamStats("imu")
        stats.add(make_batch(seq=4))
        stats.add(make_batch(seq=7))  # 5 and 6 never arrived

        self.assertEqual(stats.seq_gaps, 2)
        self.assertEqual(stats.total_seq_gaps, 2)

    def test_a_sixteen_bit_wrap_is_not_a_gap(self) -> None:
        """65535 -> 0 is the counter rolling over, not 65535 lost batches.

        At ~20.8 IMU batches/s this arrives about 52 minutes in. The masked
        arithmetic was always written for it; nothing had ever reached it.
        """
        stats = fp.StreamStats("imu")
        stats.add(make_batch(seq=65535))
        stats.add(make_batch(seq=0))

        self.assertEqual(stats.seq_gaps, 0)
        self.assertEqual(stats.total_seq_gaps, 0)
        self.assertEqual(stats.seq_wraps, 1)

    def test_a_gap_that_straddles_the_wrap_is_still_a_gap(self) -> None:
        stats = fp.StreamStats("imu")
        stats.add(make_batch(seq=65534))
        stats.add(make_batch(seq=1))  # 65535 and 0 never arrived

        self.assertEqual(stats.seq_gaps, 2)
        self.assertEqual(stats.seq_wraps, 1)

    def test_a_reboot_is_not_a_wrap(self) -> None:
        """The check that makes "seq wraps 1" mean something.

        A reboot sends `seq` backwards too. If both printed the same thing, a
        long run could not tell the rollover it was looking for from a board
        that fell over. `t_ms` restarting is the discriminator.
        """
        stats = fp.StreamStats("imu")
        stats.add(make_batch(t_ms=3_000_000, seq=40000))
        stats.add(make_batch(t_ms=12, seq=0))

        self.assertEqual(stats.restarts, 1)
        self.assertEqual(stats.seq_wraps, 0)
        self.assertEqual(stats.total_seq_gaps, 0)

    def test_a_wrap_with_the_clock_still_running_is_a_wrap(self) -> None:
        stats = fp.StreamStats("imu")
        stats.add(make_batch(t_ms=3_000_000, seq=65535))
        stats.add(make_batch(t_ms=3_000_048, seq=0))

        self.assertEqual(stats.restarts, 0)
        self.assertEqual(stats.seq_wraps, 1)
        self.assertEqual(stats.total_seq_gaps, 0)

    def test_reset_starts_a_window_without_losing_continuity_or_totals(self) -> None:
        stats = fp.StreamStats("imu")
        stats.add(make_batch(seq=1, count=10))
        stats.reset()

        self.assertEqual(stats.samples, 0)
        self.assertEqual(stats.total_samples, 10)

        # The next batch is still measured against seq=1, so a drop across the
        # window boundary is not laundered by the reset.
        stats.add(make_batch(seq=4, count=10))
        self.assertEqual(stats.seq_gaps, 2)
        self.assertEqual(stats.total_seq_gaps, 2)
        self.assertEqual(stats.total_samples, 20)
        self.assertEqual(stats.total_batches, 2)

    def test_the_run_total_sees_a_gap_the_window_cannot(self) -> None:
        """`reset()` clears `_prev_ts`, so a stall at a window boundary is
        invisible to the windowed figure. It is not invisible to the run."""
        stats = fp.StreamStats("battery")
        stats.add(make_batch(stream_id=fp.STREAM_BATTERY, t_ms=0, period_us=0, count=1))
        stats.reset()
        stats.add(
            make_batch(stream_id=fp.STREAM_BATTERY, t_ms=4000, period_us=0, count=1)
        )

        self.assertEqual(stats.max_gap_ms, 0.0)
        self.assertEqual(stats.total_max_gap_ms, 4000.0)

    def test_run_totals_span_every_window(self) -> None:
        stats = fp.StreamStats("magn")
        for window in range(4):
            stats.add(
                make_batch(
                    stream_id=fp.STREAM_MAGN,
                    t_ms=window * 1000,
                    period_us=50_000,
                    count=11,
                )
            )
            stats.reset()

        self.assertEqual(stats.total_samples, 44)
        self.assertEqual(stats.total_batches, 4)
        self.assertAlmostEqual(stats.total_host_rate(4.0), 11.0)
        # 44 samples from t=0 to the last at t=3500: 43 intervals over 3.5 s.
        self.assertAlmostEqual(stats.total_device_rate, 43 * 1000 / 3500)


class Scales(unittest.TestCase):
    def test_converts_through_the_nano_si_table(self) -> None:
        """One g on the accelerometer, checked against physics."""
        scale = fp.Scale(unit=1, num=59820565, den=100)

        self.assertAlmostEqual(scale.to_si(16393), 9.80665, places=3)

    def test_a_dimensionless_field_is_one_e9_over_one(self) -> None:
        """The trap that made a working light sensor print 0.0000.

        The table is in *nano*-SI, so the identity is 1e9/1. Writing 1/1 is
        silent: a good count of 129 came out as 1.29e-7 and displayed as zero.
        """
        identity = fp.Scale(unit=9, num=1_000_000_000, den=1)
        self.assertEqual(identity.to_si(129), 129.0)

        wrong = fp.Scale(unit=9, num=1, den=1)
        self.assertAlmostEqual(wrong.to_si(129), 1.29e-7)

    def test_decode_pairs_scales_with_field_names_in_wire_order(self) -> None:
        table = fp.ScaleTable(
            by_stream={fp.STREAM_MAGN: [fp.Scale(3, 100, 1)] * 3},
        )
        batch = make_batch(stream_id=fp.STREAM_MAGN, body=b"\x0a\x00" * 3, count=1)

        # 10 raw at 100 nano-tesla per LSB is 1 uT.
        self.assertEqual(batch.samples[0], (10, 10, 10))
        self.assertEqual(table.decode(batch), [{"x": 1e-6, "y": 1e-6, "z": 1e-6}])

    def test_decoding_without_scales_is_an_error_not_a_guess(self) -> None:
        """A capture whose scales were never fetched cannot be decoded at all."""
        with self.assertRaisesRegex(ValueError, "no scales for imu"):
            fp.ScaleTable().decode(make_batch())


class Rpc(unittest.TestCase):
    def test_request_layout_and_sequence_masking(self) -> None:
        self.assertEqual(fp.build_request(7, fp.OP_GET_SCALE, b"\x01"), b"\x07\x03\x01")
        # `seq` is one byte on the wire and the client counts past 255.
        self.assertEqual(fp.build_request(256, fp.OP_GET_BATTERY), b"\x00\x01")

    def test_response_carries_a_signed_errno_status(self) -> None:
        response = fp.parse_response(b"\x07\x01\xea\x99")

        self.assertEqual(response.seq, 7)
        self.assertEqual(response.opcode, fp.OP_GET_BATTERY)
        self.assertEqual(response.status, -22)  # -EINVAL
        self.assertFalse(response.ok)
        self.assertEqual(response.payload, b"\x99")

    def test_rejects_a_truncated_response(self) -> None:
        with self.assertRaisesRegex(ValueError, "shorter than a header"):
            fp.parse_response(b"\x07\x01")

    def test_parses_a_battery_reply(self) -> None:
        self.assertEqual(fp.parse_battery_payload(b"\xd8\x0f\x55\x01"), (4056, 85, 1))

    def test_rejects_a_scale_reply_that_declares_more_fields_than_it_carries(
        self,
    ) -> None:
        payload = bytes([fp.STREAM_IMU, 6]) + b"\x00" * fp.SCALE_FIELD_BYTES

        with self.assertRaisesRegex(ValueError, "declares 6 fields"):
            fp.parse_scale_payload(payload)


if __name__ == "__main__":
    unittest.main()
