/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rpc.hpp"

#include "battery.hpp"
#include "codec.hpp"
#include "streams.hpp"

#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rpc, LOG_LEVEL_INF);

namespace rpc
{
namespace
{

using codec::ScaleField;

/*
 * A field that is a count rather than a physical quantity: a raw ALS reading, a
 * flags byte, a key code.
 *
 * num is 1000000000 rather than 1, because the whole table is expressed in
 * *nano*-SI -- value = raw * num / den / 1e9 on the host -- so identity here is
 * 1e9/1, not 1/1. Getting that wrong is silent and looks like a dead sensor:
 * the light channel reported a perfectly good count of 129 and the host printed
 * 0.0000, having divided it by a billion.
 */
constexpr ScaleField kRawCount = {codec::kUnitDimensionless, 1000000000, 1};

/*
 * The scale table, one entry per numeric field of a sample, in the order the
 * fields appear on the wire.
 *
 * value_in_nano_SI = raw x num / den. Every stream is described the same way,
 * whichever side did the arithmetic -- the IMU carries the chip's own int16
 * registers untouched and everything else carries device-converted fixed
 * point -- so the host has one decoder path. The accel and gyro rows are ST's
 * own sensitivities, from lsm6ds3tr-c_reg.c:102 and :127.
 */
constexpr ScaleField kImuScales[] = {
	/* Gyro first: that is the order the chip's FIFO produces. */
	{codec::kUnitRadiansPerSecond, 15271631, 100},
	{codec::kUnitRadiansPerSecond, 15271631, 100},
	{codec::kUnitRadiansPerSecond, 15271631, 100},
	{codec::kUnitMetresPerSecondSquared, 59820565, 100},
	{codec::kUnitMetresPerSecondSquared, 59820565, 100},
	{codec::kUnitMetresPerSecondSquared, 59820565, 100},
};

/*
 * The wire carries deci-microtesla, so one LSB is 100 nT.
 *
 * Not centi-microtesla, which is what the design document specified and what
 * this shipped with until it was pointed at real hardware: the LIS3MDL's own
 * +-4 gauss full scale is +-400 uT, and centi-uT puts that at +-40000 -- past
 * an int16 -- so the *wire* clipped before the sensor did. The board on this
 * bench sits in a ~570 uT field and railed all three axes at once, so this was
 * not a theoretical limit. Deci-uT reaches +-3276.7 uT, eight times the chip's
 * range, and costs nothing real: the quantisation step is 0.1 uT against the
 * part's own ~0.32 uT RMS noise.
 */
constexpr ScaleField kMagnScales[] = {
	{codec::kUnitTesla, 100, 1},
	{codec::kUnitTesla, 100, 1},
	{codec::kUnitTesla, 100, 1},
};

constexpr ScaleField kEnvScales[] = {
	{codec::kUnitDegreesCelsius, 10000000, 1},          /* centi-degrees */
	{codec::kUnitPercentRelativeHumidity, 10000000, 1}, /* centi-percent */
	/* Not lux: the APDS9960 driver reports the clear channel's raw
	 * count (apds9960.c:275), so the honest unit is none at all. */
	{kRawCount},
};

constexpr ScaleField kBatteryScales[] = {
	{codec::kUnitVolts, 1000000, 1}, /* millivolts */
	{codec::kUnitPercent, 1000000000, 1},
	{kRawCount}, /* flags */
};

constexpr ScaleField kButtonScales[] = {
	{kRawCount}, /* key code */
	{kRawCount}, /* pressed */
	{kRawCount}, /* padding */
};

const ScaleField *scales_for(uint8_t stream_id, size_t &count)
{
	switch (stream_id) {
	case codec::kStreamImu:
		count = ARRAY_SIZE(kImuScales);
		return kImuScales;
	case codec::kStreamMagn:
		count = ARRAY_SIZE(kMagnScales);
		return kMagnScales;
	case codec::kStreamEnv:
		count = ARRAY_SIZE(kEnvScales);
		return kEnvScales;
	case codec::kStreamBattery:
		count = ARRAY_SIZE(kBatteryScales);
		return kBatteryScales;
	case codec::kStreamButton:
		count = ARRAY_SIZE(kButtonScales);
		return kButtonScales;
	default:
		count = 0;
		return nullptr;
	}
}

/* A response is [seq][opcode][status] followed by the payload the handler
 * writes. `write_payload` returns the payload length, or a negative errno,
 * which becomes the status and truncates the payload to nothing.
 */
struct Response {
	uint8_t *payload;
	size_t capacity;
};

int op_get_battery(const uint8_t *, size_t, Response out)
{
	if (out.capacity < codec::kBatterySampleBytes) {
		return -ENOMEM;
	}

	const battery::Reading reading = battery::last();

	out.payload[0] = static_cast<uint8_t>(reading.millivolts & 0xFF);
	out.payload[1] = static_cast<uint8_t>(reading.millivolts >> 8);
	out.payload[2] = reading.percent;
	out.payload[3] = reading.flags;

	return codec::kBatterySampleBytes;
}

int op_set_stream(const uint8_t *args, size_t args_len, Response out)
{
	if (args_len < 2) {
		return -EINVAL;
	}
	if (out.capacity < 2) {
		return -ENOMEM;
	}

	const uint8_t stream_id = args[0];
	const bool requested = args[1] != 0;

	/* Report the state actually applied rather than echoing the request,
	 * so "enable a stream this build does not have" is a visible no-op
	 * rather than a silent lie.
	 */
	streams::set_enabled(stream_id, requested);

	out.payload[0] = stream_id;
	out.payload[1] = streams::enabled(stream_id) ? 1 : 0;

	return 2;
}

int op_get_scale(const uint8_t *args, size_t args_len, Response out)
{
	if (args_len < 1) {
		return -EINVAL;
	}

	size_t count = 0;
	const ScaleField *fields = scales_for(args[0], count);

	if (fields == nullptr) {
		return -ENOENT;
	}
	if (out.capacity < 2 + count * codec::kScaleFieldBytes) {
		return -ENOMEM;
	}

	out.payload[0] = args[0];
	out.payload[1] = static_cast<uint8_t>(count);

	for (size_t i = 0; i < count; i++) {
		codec::pack_scale_field(&out.payload[2 + i * codec::kScaleFieldBytes], fields[i]);
	}

	return static_cast<int>(2 + count * codec::kScaleFieldBytes);
}

int op_get_serial(const uint8_t *, size_t, Response out)
{
	/* The nRF52840's FICR DEVICEID: two 32-bit words, a real per-chip
	 * identifier rather than a build constant.
	 */
	constexpr size_t kDeviceIdBytes = 8;

	if (out.capacity < kDeviceIdBytes) {
		return -ENOMEM;
	}

	const ssize_t got = hwinfo_get_device_id(out.payload, kDeviceIdBytes);
	if (got < 0) {
		return static_cast<int>(got);
	}

	return static_cast<int>(got);
}

int op_get_build_id(const uint8_t *, size_t, Response out)
{
	/* The VERSION file's app version plus the `git describe` CMakeLists.txt
	 * injects, so a board can be traced back to a commit.
	 */
	static const char build_id[] = APP_VERSION_STRING "+" APP_GIT_DESCRIBE;
	constexpr size_t kMaxBuildIdBytes = 48;

	size_t len = sizeof(build_id) - 1;
	if (len > kMaxBuildIdBytes) {
		len = kMaxBuildIdBytes;
	}
	if (out.capacity < len) {
		return -ENOMEM;
	}

	memcpy(out.payload, build_id, len);

	return static_cast<int>(len);
}

} /* namespace */

size_t handle(const uint8_t *request, size_t len, uint8_t *out, size_t out_cap)
{
	if (request == nullptr || len < codec::kRpcRequestHeaderBytes ||
	    out_cap < codec::kRpcResponseHeaderBytes) {
		return 0;
	}

	const uint8_t seq = request[0];
	const uint8_t opcode = request[1];
	const uint8_t *args = &request[codec::kRpcRequestHeaderBytes];
	const size_t args_len = len - codec::kRpcRequestHeaderBytes;

	const Response payload = {
		.payload = &out[codec::kRpcResponseHeaderBytes],
		.capacity = out_cap - codec::kRpcResponseHeaderBytes,
	};

	int result;

	switch (opcode) {
	case codec::kOpGetBattery:
		result = op_get_battery(args, args_len, payload);
		break;
	case codec::kOpSetStream:
		result = op_set_stream(args, args_len, payload);
		break;
	case codec::kOpGetScale:
		result = op_get_scale(args, args_len, payload);
		break;
	case codec::kOpGetSerial:
		result = op_get_serial(args, args_len, payload);
		break;
	case codec::kOpGetBuildId:
		result = op_get_build_id(args, args_len, payload);
		break;
	default:
		result = -ENOTSUP;
		break;
	}

	out[0] = seq;
	out[1] = opcode;
	out[2] = static_cast<uint8_t>(static_cast<int8_t>(result < 0 ? result : 0));

	LOG_DBG("opcode 0x%02x seq %u -> %d", opcode, seq, result);

	return codec::kRpcResponseHeaderBytes + (result < 0 ? 0 : static_cast<size_t>(result));
}

} /* namespace rpc */
