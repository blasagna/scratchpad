/*
 * Emits wire-format vectors using the firmware's own encoder. Copyright (c)
 * 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * ../../src/codec.cpp is free of Zephyr headers so that tests/codec/ can build
 * it for native_sim; this reuses that same property to build it with the host
 * compiler and print what it produces. test_cpp_parity.py compiles this file,
 * runs it, and requires host/feather_protocol.py to produce identical bytes.
 *
 * The point is that neither side is checked against a transcription of the
 * other. tests/codec/src/main.cpp pins the C++ layout against literal bytes;
 * this pins the *Python* against the C++ encoder's actual output. A constant
 * edited on one side and not the other fails here rather than on the wire.
 *
 * Unlike everything under ../../src and ../../tests, this is compiled by a
 * plain hosted g++ and not by Zephyr, so the full standard library is fair game.
 */

#include "codec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace codec;

namespace
{

std::string to_hex(const std::vector<uint8_t> &bytes)
{
	static const char *digits = "0123456789abcdef";
	std::string out;

	out.reserve(bytes.size() * 2);
	for (uint8_t b : bytes) {
		out.push_back(digits[b >> 4]);
		out.push_back(digits[b & 0x0F]);
	}
	return out;
}

/* An empty payload is a real case -- it is what an empty COBS frame encodes --
 * and its hex is the empty string, which would vanish from a whitespace-split
 * line. "-" keeps every row the same shape.
 */
std::string hex_field(const std::vector<uint8_t> &bytes)
{
	return bytes.empty() ? std::string("-") : to_hex(bytes);
}

/* cobs <plain-hex> <encoded-hex> -- the encoder's output for a raw payload. */
void emit_cobs(const std::vector<uint8_t> &plain)
{
	std::vector<uint8_t> encoded(cobs_max_encoded_size(plain.size()) + 8);
	const size_t n = cobs_encode(plain.data(), plain.size(), encoded.data(), encoded.size());

	if (n == 0) {
		std::fprintf(stderr, "encode failed for %zu bytes\n", plain.size());
		std::exit(1);
	}
	encoded.resize(n);
	std::printf("cobs %s %s\n", hex_field(plain).c_str(), hex_field(encoded).c_str());
}

/*
 * batch <t_ms> <seq> <period_us> <stream_id> <count> <channel> <plain-hex>
 *       <encoded-hex>
 *
 * `plain` is what usb.cpp hands to the framer: the channel byte, the packed
 * header, then `count` sample bodies. The fields are printed alongside so the
 * Python side can assert on decoded values and not only on bytes.
 */
void emit_batch(uint32_t t_ms, uint16_t seq, uint16_t period_us, uint8_t stream_id, uint8_t count)
{
	const size_t body = sample_bytes(stream_id) * count;
	std::vector<uint8_t> plain(1 + kBatchHeaderBytes + body);
	const BatchHeader header = {t_ms, seq, period_us, stream_id, count};

	plain[0] = kChannelSamples;
	pack_batch_header(&plain[1], header);
	/* A deterministic pattern: the bytes only have to be reproducible, and
	 * this one puts a 0x00 in every batch long enough to have one, which is
	 * what makes the framing do any work.
	 */
	for (size_t i = 0; i < body; i++) {
		plain[1 + kBatchHeaderBytes + i] = static_cast<uint8_t>(i * 37);
	}

	std::vector<uint8_t> encoded(cobs_max_encoded_size(plain.size()) + 8);
	const size_t n = cobs_encode(plain.data(), plain.size(), encoded.data(), encoded.size());
	encoded.resize(n);

	std::printf("batch %u %u %u %u %u %u %s %s\n", t_ms, seq, period_us, stream_id, count,
		    kChannelSamples, to_hex(plain).c_str(), to_hex(encoded).c_str());
}

/* scale <unit> <num> <den> <hex> */
void emit_scale(uint8_t unit, int32_t num, int32_t den)
{
	std::vector<uint8_t> out(kScaleFieldBytes);
	const ScaleField field = {unit, num, den};

	pack_scale_field(out.data(), field);
	std::printf("scale %u %d %d %s\n", unit, num, den, to_hex(out).c_str());
}

/* sizes -- the constants feather_protocol.py restates, straight from the header. */
void emit_sizes()
{
	std::printf("size header %zu\n", kBatchHeaderBytes);
	std::printf("size scale_field %zu\n", kScaleFieldBytes);
	std::printf("size cobs_block %u\n", kCobsBlock);
	for (uint8_t id = kStreamMin; id <= kStreamMax; id++) {
		std::printf("size stream_%u %zu\n", id, sample_bytes(id));
	}
	std::printf("const channel_samples %u\n", kChannelSamples);
	std::printf("const channel_rpc %u\n", kChannelRpc);
	std::printf("const battery_flag_usb %u\n", kBatteryFlagUsb);
	std::printf("const op_get_battery %u\n", kOpGetBattery);
	std::printf("const op_set_stream %u\n", kOpSetStream);
	std::printf("const op_get_scale %u\n", kOpGetScale);
	std::printf("const op_get_serial %u\n", kOpGetSerial);
	std::printf("const op_get_build_id %u\n", kOpGetBuildId);
	std::printf("const unit_ms2 %u\n", kUnitMetresPerSecondSquared);
	std::printf("const unit_rad_s %u\n", kUnitRadiansPerSecond);
	std::printf("const unit_tesla %u\n", kUnitTesla);
	std::printf("const unit_degc %u\n", kUnitDegreesCelsius);
	std::printf("const unit_rh %u\n", kUnitPercentRelativeHumidity);
	std::printf("const unit_lux %u\n", kUnitLux);
	std::printf("const unit_volts %u\n", kUnitVolts);
	std::printf("const unit_percent %u\n", kUnitPercent);
	std::printf("const unit_dimensionless %u\n", kUnitDimensionless);
}

std::vector<uint8_t> repeated(uint8_t value, size_t n)
{
	return std::vector<uint8_t>(n, value);
}

} /* namespace */

int main()
{
	emit_sizes();

	/* The COBS cases tests/codec/src/main.cpp pins for the C++ side. Same
	 * inputs, so a divergence shows up as a byte difference here rather
	 * than as two suites that pass separately.
	 */
	emit_cobs({});
	emit_cobs({0x11});
	emit_cobs({0x00});
	emit_cobs({0x00, 0x11, 0x22});
	emit_cobs({0x11, 0x22, 0x00});
	emit_cobs({0x00, 0x11, 0x00});
	emit_cobs({0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00});
	emit_cobs(repeated(0x00, 300));

	/* The 253/254/255 block boundary, where COBS implementations diverge. */
	for (size_t len : {(size_t)252, (size_t)253, (size_t)254, (size_t)255, (size_t)256,
			   (size_t)507, (size_t)508, (size_t)509}) {
		emit_cobs(repeated(0xAA, len));
	}

	/* A full block followed by a zero, and exactly one block. */
	std::vector<uint8_t> block_then_zero = repeated(0xAA, 254);
	block_then_zero.push_back(0x00);
	emit_cobs(block_then_zero);
	emit_cobs(repeated(0xAA, 254));

	/* Every length up to a batch, so no off-by-one hides between cases. */
	for (size_t len = 1; len <= 300; len++) {
		std::vector<uint8_t> data(len);
		for (size_t i = 0; i < len; i++) {
			data[i] = static_cast<uint8_t>(i * 7 + 1);
		}
		emit_cobs(data);
	}

	/* Batches as the firmware actually emits them. 10 is the IMU FIFO
	 * watermark and so the size of every USB batch; 19 is what a 247-byte
	 * ATT MTU leaves room for.
	 */
	emit_batch(123456, 42, 4808, kStreamImu, 10);
	emit_batch(123456, 42, 4808, kStreamImu, 19);
	emit_batch(0, 0, 4808, kStreamImu, 1);
	emit_batch(0xFFFFFFFF, 0xFFFF, 50000, kStreamMagn, 1);
	emit_batch(1000, 65535, 50000, kStreamMagn, 4);
	emit_batch(2000, 7, 0, kStreamEnv, 1);
	emit_batch(3000, 8, 0, kStreamBattery, 1);
	emit_batch(4000, 9, 0, kStreamButton, 1);

	/* The scale rows rpc.cpp reports, including the dimensionless one whose
	 * identity is 1e9/1 -- writing 1/1 there made a working light sensor
	 * read 0.0000, and it is the row most likely to be got wrong again.
	 */
	emit_scale(kUnitMetresPerSecondSquared, 59820565, 100);
	emit_scale(kUnitRadiansPerSecond, 15271631, 100);
	emit_scale(kUnitTesla, 100, 1);
	emit_scale(kUnitDegreesCelsius, 10000000, 1);
	emit_scale(kUnitPercentRelativeHumidity, 10000000, 1);
	emit_scale(kUnitDimensionless, 1000000000, 1);
	emit_scale(kUnitVolts, 1000000, 1);
	emit_scale(kUnitPercent, 1000000000, 1);

	return 0;
}
