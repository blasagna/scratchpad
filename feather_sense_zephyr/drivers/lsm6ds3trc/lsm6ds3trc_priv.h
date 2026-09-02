/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between lsm6ds3trc.c and lsm6ds3trc_trigger.c. Not for application use
 * -- lsm6ds3trc.h is the application-facing header.
 */

#ifndef FEATHER_SENSE_LSM6DS3TRC_PRIV_H_
#define FEATHER_SENSE_LSM6DS3TRC_PRIV_H_

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include <lsm6ds3tr-c_reg.h>

struct lsm6ds3trc_config {
	struct i2c_dt_spec i2c;
#ifdef CONFIG_LSM6DS3TRC_TRIGGER
	struct gpio_dt_spec irq_gpio;
#endif
};

struct lsm6ds3trc_data {
	const struct device *dev;
	stmdev_ctx_t ctx;
	uint8_t who_am_i;

	/* The most recent single read through the ordinary sensor API. The
	 * streaming path does not touch this -- it goes through the FIFO.
	 */
	struct {
		int16_t gyro[3];
		int16_t accel[3];
	} last;

#ifdef CONFIG_LSM6DS3TRC_TRIGGER
	struct gpio_callback gpio_cb;
	const struct sensor_trigger *watermark_trigger;
	sensor_trigger_handler_t watermark_handler;

#if defined(CONFIG_LSM6DS3TRC_TRIGGER_OWN_THREAD)
	struct k_sem trig_sem;
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_LSM6DS3TRC_THREAD_STACK_SIZE);
	struct k_thread thread;
#elif defined(CONFIG_LSM6DS3TRC_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
#endif
#endif /* CONFIG_LSM6DS3TRC_TRIGGER */
};

#ifdef CONFIG_LSM6DS3TRC_TRIGGER
int lsm6ds3trc_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			   sensor_trigger_handler_t handler);
int lsm6ds3trc_trigger_init(const struct device *dev);
#endif

#endif /* FEATHER_SENSE_LSM6DS3TRC_PRIV_H_ */
