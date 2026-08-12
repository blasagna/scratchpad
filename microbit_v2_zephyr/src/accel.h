/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ACCEL_H_
#define APP_ACCEL_H_

#include <stdint.h>
#include <zephyr/kernel.h>

/** One accelerometer sample. Axes are milli-g. */
struct accel_sample {
	uint32_t t_ms;
	int16_t x;
	int16_t y;
	int16_t z;
};

/** Pending samples, filled by the data-ready trigger, drained by the BLE thread. */
extern struct k_msgq accel_msgq;

/** Start the 100 Hz data-ready stream. */
int accel_start(void);

/** Samples dropped since boot because the queue was full. */
uint32_t accel_dropped(void);

#endif /* APP_ACCEL_H_ */
