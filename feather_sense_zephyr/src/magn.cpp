/*
 * The 20 Hz magnetometer stream.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * Device-converted fixed point, unlike the IMU: at 20 Hz there is no hot path
 * to protect, and using the in-tree LIS3MDL driver is worth far more than
 * saving a multiply.
 */

#include "magn.hpp"

#include "codec.hpp"
#include "streams.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(magn, LOG_LEVEL_INF);

namespace magn
{
namespace
{

const device *const dev = DEVICE_DT_GET(DT_ALIAS(magn0));

constexpr size_t kStackSize = 1536;
K_THREAD_STACK_DEFINE(stack, kStackSize);
k_thread thread;

constexpr int kPriority = 6;

/* CONFIG_LIS3MDL_ODR is set to "20" in prj.conf rather than inherited: the
 * driver's default is the string "0.625", and sampling faster than the chip
 * refreshes means re-reading samples it has not produced.
 */
constexpr int kRateHz = 20;
constexpr int kPeriodMs = 1000 / kRateHz;
constexpr uint16_t kPeriodUs = 1000000 / kRateHz;

/*
 * Two samples per batch, not four.
 *
 * At 20 Hz the throughput is 120 B/s and batching saves nothing worth having;
 * what batching costs is latency, and the design's budget is well under 100 ms.
 * Two samples is exactly 100 ms and halves the notification count; four would
 * spend 200 ms of latency to save bandwidth this link does not need.
 */
constexpr uint8_t kBatchSamples = 2;

k_timer timer;

/*
 * Gauss (what the driver reports) to deci-microtesla (what goes on the wire).
 * One gauss is 100 uT, so one gauss is 1000 deci-uT.
 *
 * Deci rather than centi, which is what the design document specified: the
 * chip's own +-4 gauss full scale is +-400 uT, and centi-uT would put that at
 * +-40000 -- past an int16 -- so the *wire* would clip before the sensor did.
 * That is not hypothetical. The first board this ran on sat in a ~570 uT field
 * and railed all three axes at once. Deci-uT reaches +-3276.7 uT, and the
 * 0.1 uT step it costs is below the part's own ~0.32 uT RMS noise, so nothing
 * real is lost. The clamp below is kept anyway: it is now unreachable through
 * this sensor, which is exactly the state a clamp should be in.
 */
int16_t gauss_to_deci_ut(const sensor_value &value)
{
	const int64_t micro_gauss = static_cast<int64_t>(value.val1) * 1000000 + value.val2;
	const int64_t deci_ut = micro_gauss / 1000;

	if (deci_ut > INT16_MAX) {
		return INT16_MAX;
	}
	if (deci_ut < INT16_MIN) {
		return INT16_MIN;
	}

	return static_cast<int16_t>(deci_ut);
}

void entry(void *, void *, void *)
{
	codec::MagnSample batch[kBatchSamples];
	uint8_t filled = 0;
	uint32_t first_t_ms = 0;

	k_timer_start(&timer, K_MSEC(kPeriodMs), K_MSEC(kPeriodMs));

	while (true) {
		k_timer_status_sync(&timer);

		if (!streams::enabled(codec::kStreamMagn)) {
			filled = 0;
			continue;
		}

		sensor_value values[3];

		if (sensor_sample_fetch(dev) < 0 ||
		    sensor_channel_get(dev, SENSOR_CHAN_MAGN_XYZ, values) < 0) {
			LOG_WRN("magnetometer read failed");
			continue;
		}

		if (filled == 0) {
			first_t_ms = k_uptime_get_32();
		}

		batch[filled] = codec::MagnSample{
			.x = gauss_to_deci_ut(values[0]),
			.y = gauss_to_deci_ut(values[1]),
			.z = gauss_to_deci_ut(values[2]),
		};
		filled++;

		if (filled == kBatchSamples) {
			streams::emit(codec::kStreamMagn, first_t_ms, kPeriodUs, filled, batch,
				      filled * sizeof(codec::MagnSample));
			filled = 0;
		}
	}
}

} /* namespace */

int start()
{
	if (!device_is_ready(dev)) {
		LOG_ERR("%s is not ready", dev->name);
		return -ENODEV;
	}

	k_timer_init(&timer, nullptr, nullptr);

	k_thread_create(&thread, stack, kStackSize, entry, nullptr, nullptr, nullptr, kPriority, 0,
			K_NO_WAIT);
	k_thread_name_set(&thread, "magn");

	return 0;
}

} /* namespace magn */
