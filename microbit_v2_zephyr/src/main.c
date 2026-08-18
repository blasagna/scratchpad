/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See README.md for the requirements this implements and for the hardware
 * notes -- in particular, why the microphone goes through the SAADC.
 */

#include "accel.h"
#include "audio.h"
#include "ble.h"
#include "buttons.h"
#include "display.h"
#include "temp.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* How often to report queue health on the console. */
#define STATS_PERIOD_MS 10000

int main(void)
{
	int err;

	LOG_INF("micro:bit V2 sensor application starting");

	err = display_init();
	if (err) {
		return err;
	}

	err = buttons_init();
	if (err) {
		return err;
	}

	err = audio_start();
	if (err) {
		return err;
	}

	err = ble_start();
	if (err) {
		return err;
	}

	err = accel_start();
	if (err) {
		return err;
	}

	err = temp_start();
	if (err) {
		return err;
	}

	display_text("hi");

	for (;;) {
		k_sleep(K_MSEC(STATS_PERIOD_MS));
		LOG_INF("accel queue %u/%u, dropped %u, overruns %u",
			k_msgq_num_used_get(&accel_msgq),
			k_msgq_num_free_get(&accel_msgq) + k_msgq_num_used_get(&accel_msgq),
			accel_dropped(), accel_overruns());
	}

	return 0;
}
