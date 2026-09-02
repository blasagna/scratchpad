/*
 * The 1 Hz environmental stream: temperature, humidity, light.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * The lowest-priority sampling thread, deliberately. Zephyr's sht3xd driver
 * issues a blocking single-shot conversion, and the CircuitPython port deleted
 * its environmental reads over exactly this cost. Preemptive scheduling
 * *contains* it -- a blocked env thread cannot delay the IMU thread above it --
 * but it does not make the read cheaper, which is why last_fetch_ms() exists
 * and why `fs env` prints it.
 */

#include "env.hpp"

#include "codec.hpp"
#include "streams.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(env, LOG_LEVEL_INF);

namespace env
{
namespace
{

/* The SHT30 is the one sensor the board DTS already declares, as `sht3xd@44`,
 * and it carries no alias -- so it is fetched by compatible. The build asserts
 * the node exists rather than checking a pointer that DEVICE_DT_GET_ANY makes
 * non-null by construction.
 */
BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht3xd), "the board DTS should declare the SHT30");

const device *const sht = DEVICE_DT_GET_ANY(sensirion_sht3xd);
const device *const light = DEVICE_DT_GET(DT_ALIAS(light0));

constexpr size_t kStackSize = 1536;
K_THREAD_STACK_DEFINE(stack, kStackSize);
k_thread thread;

constexpr int kPriority = 8;
constexpr int kPeriodMs = 1000;

struct Sample {
	int16_t temperature_centi_c;
	uint16_t humidity_centi_pct;
	uint16_t light_level;
};

static_assert(sizeof(Sample) == codec::kEnvSampleBytes);

k_timer timer;
uint32_t fetch_us;
uint32_t failures;

int16_t to_centi(const sensor_value &value)
{
	const int64_t centi = static_cast<int64_t>(value.val1) * 100 + value.val2 / 10000;

	if (centi > INT16_MAX) {
		return INT16_MAX;
	}
	if (centi < INT16_MIN) {
		return INT16_MIN;
	}

	return static_cast<int16_t>(centi);
}

void entry(void *, void *, void *)
{
	k_timer_start(&timer, K_MSEC(kPeriodMs), K_MSEC(kPeriodMs));

	while (true) {
		k_timer_status_sync(&timer);

		if (!streams::enabled(codec::kStreamEnv)) {
			continue;
		}

		Sample sample = {};
		sensor_value value;

		/* Cycle counter, not k_uptime_get_32(): the whole point of the
		 * measurement is to find out whether this read is expensive,
		 * and a millisecond tick cannot tell 0 ms from 900 us.
		 */
		const uint32_t started = k_cycle_get_32();
		const bool sht_ok = sensor_sample_fetch(sht) == 0;

		fetch_us = k_cyc_to_us_near32(k_cycle_get_32() - started);

		if (!sht_ok) {
			failures++;
		}

		if (sht_ok && sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &value) == 0) {
			sample.temperature_centi_c = to_centi(value);
		}
		if (sht_ok && sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &value) == 0) {
			sample.humidity_centi_pct = static_cast<uint16_t>(to_centi(value));
		}

		/* SENSOR_CHAN_LIGHT on the APDS9960 is the clear channel's raw
		 * count, not lux -- see drivers/sensor/apds9960/apds9960.c:275,
		 * which returns sample_crgb[0] verbatim. The scale table
		 * therefore reports this field as dimensionless rather than
		 * claiming a photometric unit the driver does not produce.
		 */
		if (sensor_sample_fetch(light) == 0 &&
		    sensor_channel_get(light, SENSOR_CHAN_LIGHT, &value) == 0) {
			sample.light_level = static_cast<uint16_t>(
				value.val1 < 0
					? 0
					: (value.val1 > UINT16_MAX ? UINT16_MAX : value.val1));
		}

		/* period_us is 0 for the unbatched streams: count is 1 and
		 * there is nothing for the host to back-date.
		 */
		streams::emit(codec::kStreamEnv, k_uptime_get_32(), 0, 1, &sample, sizeof(sample));
	}
}

} /* namespace */

uint32_t last_fetch_us()
{
	return fetch_us;
}

uint32_t fetch_failures()
{
	return failures;
}

int start()
{
	if (!device_is_ready(sht)) {
		LOG_ERR("the SHT30 is not ready");
		return -ENODEV;
	}
	if (!device_is_ready(light)) {
		LOG_ERR("%s is not ready", light->name);
		return -ENODEV;
	}

	k_timer_init(&timer, nullptr, nullptr);

	k_thread_create(&thread, stack, kStackSize, entry, nullptr, nullptr, nullptr, kPriority, 0,
			K_NO_WAIT);
	k_thread_name_set(&thread, "env");

	return 0;
}

} /* namespace env */
