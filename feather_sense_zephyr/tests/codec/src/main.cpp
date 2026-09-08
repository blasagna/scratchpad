/*
 * Host unit tests for the wire format. Copyright (c) 2026 Bob DiMaiolo.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr builds C++ against its minimal libc++ with -nostdinc++, so <cstdint>
 * exists but <cmath>/<cstdlib> do not. These use the C headers.
 */

#include "codec.hpp"

#include <zephyr/ztest.h>

#include <stdint.h>
#include <string.h>

using namespace codec;

ZTEST_SUITE(batch_header, NULL, NULL, NULL, NULL, NULL);

/* The header is the contract host/feather_protocol.py mirrors, so its byte
 * layout is pinned literally rather than through the unpacker -- a test that
 * only round-trips through this file's own code would agree with any layout.
 */
ZTEST(batch_header, test_packs_the_documented_little_endian_layout)
{
	const BatchHeader header = {
		.t_ms = 0x11223344,
		.seq = 0xAABB,
		.period_us = 0x1234,
		.stream_id = kStreamImu,
		.count = 19,
	};
	uint8_t out[kBatchHeaderBytes];

	pack_batch_header(out, header);

	const uint8_t expected[kBatchHeaderBytes] = {
		0x44, 0x33, 0x22, 0x11, /* t_ms, little-endian */
		0xBB, 0xAA,             /* seq */
		0x34, 0x12,             /* period_us */
		0x01,                   /* stream_id */
		19,                     /* count */
	};

	zassert_mem_equal(out, expected, sizeof(expected), "header layout changed");
}

ZTEST(batch_header, test_round_trips_every_field)
{
	const BatchHeader header = {
		.t_ms = 0xFFFFFFFF,
		.seq = 0xFFFF,
		.period_us = 4808,
		.stream_id = kStreamBattery,
		.count = 1,
	};
	uint8_t buffer[kBatchHeaderBytes];
	BatchHeader decoded = {};

	pack_batch_header(buffer, header);
	zassert_true(unpack_batch_header(buffer, sizeof(buffer), decoded));

	zassert_equal(decoded.t_ms, header.t_ms);
	zassert_equal(decoded.seq, header.seq);
	zassert_equal(decoded.period_us, header.period_us);
	zassert_equal(decoded.stream_id, header.stream_id);
	zassert_equal(decoded.count, header.count);
}

ZTEST(batch_header, test_rejects_a_short_buffer)
{
	uint8_t buffer[kBatchHeaderBytes] = {};
	BatchHeader decoded = {};

	zassert_false(unpack_batch_header(buffer, kBatchHeaderBytes - 1, decoded));
}

/* The header is 10 bytes so that the int16 sample array behind it stays 2-byte
 * aligned. If that ever stops holding, every sample body needs a memcpy the
 * design does not budget for.
 */
ZTEST(batch_header, test_header_leaves_the_sample_array_aligned)
{
	zassert_equal(kBatchHeaderBytes % 2, 0);

	for (uint8_t id = kStreamMin; id <= kStreamMax; id++) {
		zassert_not_equal(sample_bytes(id), 0, "stream %u has no sample size", id);
		zassert_equal(sample_bytes(id) % 2, 0, "stream %u sample is not word-sized", id);
	}

	zassert_equal(sample_bytes(0), 0);
	zassert_equal(sample_bytes(kStreamMax + 1), 0);
}

/* Nineteen IMU samples is what a 247-byte ATT MTU leaves room for, and the
 * number ble.cpp's MTU arithmetic must arrive at.
 */
ZTEST(batch_header, test_a_247_byte_mtu_carries_nineteen_imu_samples)
{
	const int usable = 247 - 3 - static_cast<int>(kBatchHeaderBytes);

	zassert_equal(usable / static_cast<int>(kImuSampleBytes), 19);
}

/* --- COBS ----------------------------------------------------------------- */

ZTEST_SUITE(cobs, NULL, NULL, NULL, NULL, NULL);

/* Encode, decode, and compare -- with a guard that the encoding really is
 * zero-free, which is the entire property the framing depends on.
 */
static void assert_round_trip(const uint8_t *data, size_t len)
{
	uint8_t encoded[600];
	uint8_t decoded[600];

	const size_t n = cobs_encode(data, len, encoded, sizeof(encoded));
	zassert_true(n > 0, "encode of %zu bytes failed", len);

	for (size_t i = 0; i < n; i++) {
		zassert_not_equal(encoded[i], 0, "encoded byte %zu of %zu is a zero", i, len);
	}

	const long back = cobs_decode(encoded, n, decoded, sizeof(decoded));
	zassert_equal(back, static_cast<long>(len), "decoded %ld bytes, expected %zu", back, len);
	zassert_mem_equal(decoded, data, len, "round trip changed the payload");
}

ZTEST(cobs, test_round_trips_an_empty_payload)
{
	uint8_t encoded[4];

	const size_t n = cobs_encode(nullptr, 0, encoded, sizeof(encoded));

	zassert_equal(n, 1);
	zassert_equal(encoded[0], 1);
}

ZTEST(cobs, test_round_trips_payloads_of_every_length_up_to_a_batch)
{
	uint8_t data[300];

	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = static_cast<uint8_t>(i * 7 + 1);
	}

	for (size_t len = 1; len <= sizeof(data); len++) {
		assert_round_trip(data, len);
	}
}

/* Zeros are what COBS exists to remove, so payloads made of them are the ones
 * worth being exhaustive about.
 */
ZTEST(cobs, test_round_trips_runs_of_zeros)
{
	uint8_t data[300] = {};

	for (size_t len = 1; len <= sizeof(data); len++) {
		assert_round_trip(data, len);
	}
}

ZTEST(cobs, test_round_trips_zeros_at_the_edges)
{
	const uint8_t leading[] = {0x00, 0x11, 0x22};
	const uint8_t trailing[] = {0x11, 0x22, 0x00};
	const uint8_t both[] = {0x00, 0x11, 0x00};
	const uint8_t alternating[] = {0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00};

	assert_round_trip(leading, sizeof(leading));
	assert_round_trip(trailing, sizeof(trailing));
	assert_round_trip(both, sizeof(both));
	assert_round_trip(alternating, sizeof(alternating));
}

/*
 * The 253/254/255 block boundary, explicitly.
 *
 * This is where every COBS implementation goes wrong: a run that ends because
 * it filled a block (code 0xFF, no zero restored on decode) has to be
 * distinguishable from one that ended at a zero. The three lengths around the
 * boundary, in both the all-non-zero and zero-terminated shapes, are what
 * separate a correct implementation from one that is merely usually right.
 */
ZTEST(cobs, test_pins_the_block_split_boundary)
{
	uint8_t data[512];

	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = 0xAA;
	}

	/* A plain array rather than a braced range: Zephyr's minimal libc++
	 * (-nostdinc++) ships no <initializer_list>.
	 */
	const size_t boundaries[] = {253, 254, 255, 256, 507, 508, 509};

	for (size_t len : boundaries) {
		assert_round_trip(data, len);
	}

	/* A full block followed by a zero: the encoder must emit 0xFF, then a
	 * code byte for the empty run the zero ends.
	 */
	uint8_t block_then_zero[255];

	memset(block_then_zero, 0xAA, 254);
	block_then_zero[254] = 0x00;
	assert_round_trip(block_then_zero, sizeof(block_then_zero));

	/* And exactly 254 non-zero bytes, which is the case where the encoder
	 * opens a trailing code byte for a run that never starts.
	 */
	uint8_t exactly_a_block[254];

	memset(exactly_a_block, 0xAA, sizeof(exactly_a_block));
	assert_round_trip(exactly_a_block, sizeof(exactly_a_block));
}

/* A 254-byte run costs exactly one extra byte, and nothing shorter costs any.
 * This is the budget cobs_max_encoded_size() promises and that usb.cpp sizes
 * its frame buffer against.
 */
ZTEST(cobs, test_overhead_stays_within_the_budget)
{
	uint8_t data[600];
	uint8_t encoded[700];

	memset(data, 0xAA, sizeof(data));

	for (size_t len = 0; len <= sizeof(data); len++) {
		const size_t n = cobs_encode(data, len, encoded, sizeof(encoded));

		zassert_true(n > 0);
		zassert_true(n <= cobs_max_encoded_size(len),
			     "%zu bytes encoded to %zu, budget was %zu", len, n,
			     cobs_max_encoded_size(len));
	}
}

ZTEST(cobs, test_refuses_an_output_buffer_that_is_too_small)
{
	const uint8_t data[] = {1, 2, 3};
	uint8_t encoded[2];

	zassert_equal(cobs_encode(data, sizeof(data), encoded, sizeof(encoded)), 0);
}

/* A malformed frame must be rejected rather than decoded into something
 * plausible: the serial reader counts these, and a decoder that quietly
 * produced garbage would turn a framing fault into bad data.
 */
ZTEST(cobs, test_rejects_malformed_frames)
{
	uint8_t out[16];

	const uint8_t zero_code[] = {0x00, 0x11};
	zassert_equal(cobs_decode(zero_code, sizeof(zero_code), out, sizeof(out)), -1);

	const uint8_t runs_past_the_end[] = {0x09, 0x11, 0x22};
	zassert_equal(cobs_decode(runs_past_the_end, sizeof(runs_past_the_end), out, sizeof(out)),
		      -1);
}

ZTEST(cobs, test_rejects_a_decode_buffer_that_is_too_small)
{
	const uint8_t encoded[] = {0x04, 0x11, 0x22, 0x33};
	uint8_t out[2];

	zassert_equal(cobs_decode(encoded, sizeof(encoded), out, sizeof(out)), -1);
}

/* A whole batch, framed the way usb.cpp frames one: the channel byte, the
 * header and a full complement of IMU samples.
 */
ZTEST(cobs, test_frames_a_full_imu_batch)
{
	uint8_t plain[1 + kBatchHeaderBytes + 19 * kImuSampleBytes];
	const BatchHeader header = {
		.t_ms = 123456,
		.seq = 42,
		.period_us = 4808,
		.stream_id = kStreamImu,
		.count = 19,
	};

	plain[0] = kChannelSamples;
	pack_batch_header(&plain[1], header);
	for (size_t i = kBatchHeaderBytes + 1; i < sizeof(plain); i++) {
		plain[i] = static_cast<uint8_t>(i % 256);
	}

	assert_round_trip(plain, sizeof(plain));
}

/* The scale table is what a host needs to decode anything at all, so its byte
 * layout is pinned the same way the header's is.
 */
ZTEST(batch_header, test_packs_a_scale_field)
{
	/* The accelerometer's row: 0.061 mg/LSB at +-2 g, as nano-m/s^2 per
	 * LSB scaled by 100. ST's own sensitivity, lsm6ds3tr-c_reg.c:102.
	 */
	const ScaleField field = {kUnitMetresPerSecondSquared, 59820565, 100};
	uint8_t out[kScaleFieldBytes];

	pack_scale_field(out, field);

	const uint8_t expected[kScaleFieldBytes] = {
		1,                      /* unit */
		0x15, 0xCA, 0x90, 0x03, /* num = 59820565 = 0x0390CA15, little-endian */
		0x64, 0x00, 0x00, 0x00, /* den = 100 */
	};

	zassert_mem_equal(out, expected, sizeof(expected), "scale field layout changed");
}

/* The arithmetic the host will do, checked against physics rather than against
 * the constants themselves: one g on the accelerometer, and a right angle per
 * second on the gyro.
 */
ZTEST(batch_header, test_the_scale_factors_convert_to_the_right_units)
{
	/* 1 g is 9.80665 m/s^2, which at 0.061 mg/LSB is 16393 LSB. */
	const int64_t one_g_lsb = 16393;
	const int64_t nano_ms2 = one_g_lsb * 59820565 / 100;

	zassert_within(nano_ms2, 9806650000LL, 1000000LL, "one g should read 9.80665 m/s^2");

	/* 90 deg/s at 8.75 mdps/LSB is 10286 LSB, and 90 deg/s is
	 * 1.5707963 rad/s.
	 */
	const int64_t ninety_dps_lsb = 10286;
	const int64_t nano_rad = ninety_dps_lsb * 15271631 / 100;

	zassert_within(nano_rad, 1570796327LL, 200000LL, "90 deg/s should read 1.5708 rad/s");
}
