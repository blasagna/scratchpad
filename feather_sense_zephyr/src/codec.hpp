/*
 * The wire format: batch headers, sample layouts, COBS framing, and the RPC
 * opcodes. Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * This translation unit is deliberately free of Zephyr headers, so the *same*
 * .cpp compiles into the firmware and into the native_sim ztest under
 * tests/codec/. Pulling in <zephyr/...> here breaks the host build.
 *
 * host/feather_protocol.py mirrors every constant in this file and names the
 * symbol it mirrors. There is one definition on each side and no third one.
 */

#ifndef FEATHER_SENSE_CODEC_HPP_
#define FEATHER_SENSE_CODEC_HPP_

#include <stddef.h>
#include <stdint.h>

namespace codec
{

/* --- streams -------------------------------------------------------------- */

enum StreamId : uint8_t {
	kStreamImu = 1,     /* accel + gyro, 208 Hz, batched */
	kStreamMagn = 2,    /* magnetometer, 20 Hz, batched */
	kStreamEnv = 3,     /* temperature, humidity, light, 1 Hz */
	kStreamBattery = 4, /* millivolts + percent, <=1 Hz, on change */
	kStreamButton = 5,  /* press and release events */
};

constexpr uint8_t kStreamMin = kStreamImu;
constexpr uint8_t kStreamMax = kStreamButton;
constexpr size_t kStreamCount = kStreamMax - kStreamMin + 1;

/* Sample body sizes, in bytes. Every one is a whole number of 16-bit words, so
 * the sample array stays 2-byte aligned behind the 10-byte header.
 */
constexpr size_t kImuSampleBytes = 12;    /* int16 gx,gy,gz,ax,ay,az */
constexpr size_t kMagnSampleBytes = 6;    /* int16 x,y,z */
constexpr size_t kEnvSampleBytes = 6;     /* int16 temp_c_centi, uint16 rh_centi, uint16 lux */
constexpr size_t kBatterySampleBytes = 4; /* uint16 mv, uint8 percent, uint8 flags */
constexpr size_t kButtonSampleBytes = 4;  /* uint16 code, uint8 pressed, uint8 pad */

/* Battery `flags` bits. */
constexpr uint8_t kBatteryFlagUsb = 0x01;

/* --- batch header --------------------------------------------------------- */

/*
 * batch = [ t_ms:u32 ][ seq:u16 ][ period_us:u16 ][ stream_id:u8 ][ count:u8 ]
 *         [ sample x count ]
 *
 * Little-endian throughout.
 *
 * `t_ms` is the device uptime at the FIRST sample in the batch, not at
 * transmit. `period_us` is the spacing between samples within the batch, taken
 * from the chip's own ODR, so the host back-dates the rest of the batch instead
 * of guessing from a nominal rate; it is 0 for the unbatched streams, where
 * `count` is 1 and there is nothing to back-date. `seq` counts batches per
 * stream and wraps at 16 bits: a gap in `seq` is a device-side drop, and a gap
 * in `t_ms` with no gap in `seq` is a link-side one.
 */
struct BatchHeader {
	uint32_t t_ms;
	uint16_t seq;
	uint16_t period_us;
	uint8_t stream_id;
	uint8_t count;
};

constexpr size_t kBatchHeaderBytes = 10;

/* Writes kBatchHeaderBytes into `out`, which must have room. */
void pack_batch_header(uint8_t *out, const BatchHeader &header);

/* Reads a header out of `in`. Returns false if `len` is short of a header. */
bool unpack_batch_header(const uint8_t *in, size_t len, BatchHeader &header);

/* The sample body size for a stream id, or 0 if the id is not a stream. */
size_t sample_bytes(uint8_t stream_id);

/* --- COBS ----------------------------------------------------------------- */

/*
 * Consistent Overhead Byte Stuffing: removes every 0x00 from the payload, so a
 * lone trailing 0x00 marks a frame boundary unambiguously and a corrupt frame
 * resynchronises at the next delimiter. The serial path needs this because a
 * byte stream is not self-delimiting; the BLE path does not, because a GATT
 * notification already is a datagram.
 */

/* The longest run of non-zero bytes one code byte can span. */
constexpr uint8_t kCobsBlock = 0xFE;

/* Worst-case encoded size for `n` payload bytes, excluding the delimiter. */
constexpr size_t cobs_max_encoded_size(size_t n)
{
	return n + n / kCobsBlock + 1;
}

/*
 * Encodes `len` bytes into `out`, which must hold cobs_max_encoded_size(len).
 * Returns the number of bytes written, or 0 if `out_cap` is too small. The
 * trailing 0x00 delimiter is NOT written -- the caller appends it, so a frame
 * and its delimiter can be handed to the transport in one write.
 */
size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

/*
 * Decodes one delimiter-free frame. Returns the number of bytes written to
 * `out`, or -1 if the frame is malformed or `out_cap` is too small.
 */
long cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

/* --- usb framing ---------------------------------------------------------- */

/*
 * On the serial link both sample batches and RPC frames share one pipe, so a
 * frame carries one leading byte saying which it is:
 *
 *     frame = cobs( [ channel:u8 ][ payload ] ) + 0x00
 *
 * This is the serial analogue of a GATT characteristic -- on BLE the
 * characteristic identifies the channel and nothing extra goes on the wire. The
 * `payload` bytes are identical on both transports either way, which is what
 * keeps there being one encoder and one decoder.
 *
 * 0x00 is not used, and could not be: it is the COBS delimiter.
 */
enum Channel : uint8_t {
	kChannelSamples = 1, /* device -> host: a batch */
	kChannelRpc = 2,     /* both directions: a request or a response */
};

/* --- rpc ------------------------------------------------------------------ */

/*
 * request  = [ seq:u8 ][ opcode:u8 ][ args ]
 * response = [ seq:u8 ][ opcode:u8 ][ status:i8 ][ payload ]
 *
 * `seq` is echoed, so a host can match a reply to its request and time out on
 * one that never arrives. `status` is 0 on success and a negative errno
 * otherwise.
 */
enum Opcode : uint8_t {
	kOpGetBattery = 0x01, /* -> uint16 mv, uint8 percent, uint8 flags */
	kOpSetStream = 0x02,  /* uint8 stream_id, uint8 enable -> the same, as applied */
	kOpGetScale = 0x03,   /* uint8 stream_id -> uint8 stream_id, uint8 n, n x scale */
	kOpGetSerial = 0x04,  /* -> 8 bytes of hwinfo device id */
	kOpGetBuildId = 0x05, /* -> UTF-8, <= 48 bytes */
};

constexpr size_t kRpcRequestHeaderBytes = 2;
constexpr size_t kRpcResponseHeaderBytes = 3;
constexpr size_t kRpcMaxFrameBytes = 96;

/*
 * One field of a `get scale` reply: value_in_nano_SI = raw x num / den.
 *
 * Every stream is reported the same way, whichever side did the arithmetic --
 * the IMU stream carries the chip's own int16 registers untouched, and the rest
 * carry device-converted fixed point. The host therefore has one decoder path,
 * and changing the accelerometer's full-scale range becomes a fact it learns at
 * runtime rather than a constant it must be reflashed to agree with. The cost,
 * stated plainly: a host cannot decode a capture it did not ask the scales for.
 */
struct ScaleField {
	uint8_t unit;
	int32_t num;
	int32_t den;
};

constexpr size_t kScaleFieldBytes = 9;

/* Units, as reported by `get scale`. */
enum Unit : uint8_t {
	kUnitMetresPerSecondSquared = 1,
	kUnitRadiansPerSecond = 2,
	kUnitTesla = 3,
	kUnitDegreesCelsius = 4,
	kUnitPercentRelativeHumidity = 5,
	kUnitLux = 6,
	kUnitVolts = 7,
	kUnitPercent = 8,
	kUnitDimensionless = 9,
};

/* Writes kScaleFieldBytes into `out`. */
void pack_scale_field(uint8_t *out, const ScaleField &field);

} /* namespace codec */

#endif /* FEATHER_SENSE_CODEC_HPP_ */
