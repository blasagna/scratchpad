/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The nRF52833's on-die temperature sensor, sampled once a second. This is the
 * CPU die, not the room -- the micro:bit V2 has no ambient sensor -- so expect
 * readings a few degrees above room temperature.
 *
 * The peripheral is shared: because the board clocks its LF domain from the
 * internal RC oscillator, Zephyr's clock calibration samples it too. That is
 * safe, since temp_nrf5.c guards each conversion with a mutex.
 */

#include "temp.h"
#include "ble.h"

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(temp, LOG_LEVEL_DBG);

#define TEMP_PERIOD_MS  1000
#define TEMP_STACK_SIZE 768
#define TEMP_PRIORITY   8

static const struct device *const temp_dev = DEVICE_DT_GET_ANY(nordic_nrf_temp);

static void temp_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct sensor_value val;
		int16_t centi_c;
		int err;

		k_sleep(K_MSEC(TEMP_PERIOD_MS));

		err = sensor_sample_fetch_chan(temp_dev, SENSOR_CHAN_DIE_TEMP);
		if (err) {
			LOG_ERR("fetch failed (%d)", err);
			continue;
		}

		err = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &val);
		if (err) {
			LOG_ERR("channel get failed (%d)", err);
			continue;
		}

		centi_c = (int16_t)(sensor_value_to_milli(&val) / 10);
		LOG_DBG("die temperature %d.%02d C", centi_c / 100, abs(centi_c % 100));

		ble_notify_temp(centi_c);
	}
}

K_THREAD_DEFINE(temp_tid, TEMP_STACK_SIZE, temp_thread, NULL, NULL, NULL, TEMP_PRIORITY, 0,
		K_TICKS_FOREVER);

int temp_start(void)
{
	if (!device_is_ready(temp_dev)) {
		LOG_ERR("die temperature sensor not ready");
		return -ENODEV;
	}

	k_thread_start(temp_tid);
	LOG_INF("die temperature sampling at 1 Hz");
	return 0;
}
