/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * Free of Zephyr headers on purpose: tests/codec/ compiles this exact file for
 * native_sim. See codec.hpp.
 */

#include "codec.hpp"

namespace codec
{
namespace
{

void put_u16(uint8_t *out, uint16_t value)
{
	out[0] = static_cast<uint8_t>(value & 0xFF);
	out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void put_u32(uint8_t *out, uint32_t value)
{
	out[0] = static_cast<uint8_t>(value & 0xFF);
	out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
	out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
	out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint16_t get_u16(const uint8_t *in)
{
	return static_cast<uint16_t>(in[0]) | static_cast<uint16_t>(in[1] << 8);
}

uint32_t get_u32(const uint8_t *in)
{
	return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
	       (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

void put_i32(uint8_t *out, int32_t value)
{
	put_u32(out, static_cast<uint32_t>(value));
}

} /* namespace */

void pack_batch_header(uint8_t *out, const BatchHeader &header)
{
	put_u32(&out[0], header.t_ms);
	put_u16(&out[4], header.seq);
	put_u16(&out[6], header.period_us);
	out[8] = header.stream_id;
	out[9] = header.count;
}

bool unpack_batch_header(const uint8_t *in, size_t len, BatchHeader &header)
{
	if (len < kBatchHeaderBytes) {
		return false;
	}

	header.t_ms = get_u32(&in[0]);
	header.seq = get_u16(&in[4]);
	header.period_us = get_u16(&in[6]);
	header.stream_id = in[8];
	header.count = in[9];

	return true;
}

size_t sample_bytes(uint8_t stream_id)
{
	switch (stream_id) {
	case kStreamImu:
		return kImuSampleBytes;
	case kStreamMagn:
		return kMagnSampleBytes;
	case kStreamEnv:
		return kEnvSampleBytes;
	case kStreamBattery:
		return kBatterySampleBytes;
	case kStreamButton:
		return kButtonSampleBytes;
	default:
		return 0;
	}
}

void pack_scale_field(uint8_t *out, const ScaleField &field)
{
	out[0] = field.unit;
	put_i32(&out[1], field.num);
	put_i32(&out[5], field.den);
}

/*
 * COBS.
 *
 * The encoder walks the payload one zero-delimited run at a time. A run of 254
 * non-zero bytes needs splitting across code bytes: a 0xFF code means "254
 * non-zero bytes, and no zero followed", and it is what distinguishes a run
 * that merely reached the block limit from one that ended at a zero. That
 * distinction is the whole of the 253/254/255 boundary every COBS
 * implementation gets wrong, and tests/codec pins it explicitly.
 */
size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
	if (out_cap < cobs_max_encoded_size(len)) {
		return 0;
	}

	size_t code_index = 0;
	size_t write = 1;
	uint8_t code = 1;

	for (size_t read = 0; read < len; read++) {
		if (in[read] != 0) {
			out[write++] = in[read];
			code++;
			if (code != 0xFF) {
				continue;
			}
		}

		/* Either a zero ended the run, or the run filled a block. Both
		 * close the current code byte; only the zero is consumed.
		 */
		out[code_index] = code;
		code_index = write++;
		code = 1;
	}

	out[code_index] = code;

	/* When the payload ends exactly on a full block, the loop above has
	 * already opened a code byte for a run that never starts. It holds 1,
	 * meaning an empty run, which is correct and costs one byte -- that is
	 * the overhead cobs_max_encoded_size() budgets for.
	 */
	return write;
}

long cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
	size_t read = 0;
	size_t write = 0;

	while (read < len) {
		const uint8_t code = in[read];

		/* A zero code byte cannot occur in a well-formed frame -- the
		 * encoder never emits one, and a delimiter would have ended the
		 * frame before here. Nor can a run that runs off the end.
		 */
		if (code == 0 || read + code > len) {
			return -1;
		}

		read++;
		for (uint8_t i = 1; i < code; i++) {
			if (write >= out_cap) {
				return -1;
			}
			out[write++] = in[read++];
		}

		/* A 0xFF code is a block that filled up rather than a run that
		 * ended at a zero, so no zero is restored. Neither is one at the
		 * very end of the frame: that trailing zero is the delimiter,
		 * and it is not part of the payload.
		 */
		if (code != 0xFF && read < len) {
			if (write >= out_cap) {
				return -1;
			}
			out[write++] = 0;
		}
	}

	return static_cast<long>(write);
}

} /* namespace codec */
