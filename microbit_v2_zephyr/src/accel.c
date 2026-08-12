/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LSM303AGR accelerometer at 100 Hz, driven by the chip's data-ready interrupt
 * on INT1 (P0.25) rather than by polling.
 */

#include "accel.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(accel, LOG_LEVEL_INF);

#define ACCEL_SAMPLE_RATE_HZ 100

/* One second of headroom. The BLE thread drains far faster than this when a
 * central is subscribed; the depth only matters while nobody is listening.
 */
#define ACCEL_QUEUE_DEPTH 128

K_MSGQ_DEFINE(accel_msgq, sizeof(struct accel_sample), ACCEL_QUEUE_DEPTH, 4);

static const struct device *const accel_dev = DEVICE_DT_GET(DT_ALIAS(accel0));
static uint32_t dropped;

/* The sensor API reports m/s^2. Convert via micro-m/s^2 so the whole thing stays
 * in integer arithmetic: 1 g = 9.80665 m/s^2.
 */
static int16_t to_milli_g(const struct sensor_value *val)
{
	int64_t micro_m_s2 = sensor_value_to_micro(val);

	return (int16_t)CLAMP((micro_m_s2 * 1000) / 9806650, INT16_MIN, INT16_MAX);
}

static void drdy_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	struct sensor_value val[3];
	struct accel_sample sample;
	int err;

	ARG_UNUSED(trig);

	err = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	if (err) {
		LOG_ERR("fetch failed (%d)", err);
		return;
	}

	err = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, val);
	if (err) {
		LOG_ERR("channel get failed (%d)", err);
		return;
	}

	sample.t_ms = k_uptime_get_32();
	sample.x = to_milli_g(&val[0]);
	sample.y = to_milli_g(&val[1]);
	sample.z = to_milli_g(&val[2]);

	/* Drop the oldest sample rather than the newest: a stalled consumer should
	 * cost us history, not the live signal.
	 */
	if (k_msgq_put(&accel_msgq, &sample, K_NO_WAIT) != 0) {
		struct accel_sample discard;

		(void)k_msgq_get(&accel_msgq, &discard, K_NO_WAIT);
		(void)k_msgq_put(&accel_msgq, &sample, K_NO_WAIT);
		dropped++;
	}
}

int accel_start(void)
{
	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	struct sensor_value odr = {.val1 = ACCEL_SAMPLE_RATE_HZ, .val2 = 0};
	int err;

	if (!device_is_ready(accel_dev)) {
		LOG_ERR("%s not ready", accel_dev->name);
		return -ENODEV;
	}

	err = sensor_attr_set(accel_dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY,
			      &odr);
	if (err) {
		LOG_ERR("cannot set %d Hz ODR (%d)", ACCEL_SAMPLE_RATE_HZ, err);
		return err;
	}

	err = sensor_trigger_set(accel_dev, &trig, drdy_handler);
	if (err) {
		LOG_ERR("cannot set data-ready trigger (%d)", err);
		return err;
	}

	LOG_INF("accelerometer streaming at %d Hz", ACCEL_SAMPLE_RATE_HZ);
	return 0;
}

uint32_t accel_dropped(void)
{
	return dropped;
}
