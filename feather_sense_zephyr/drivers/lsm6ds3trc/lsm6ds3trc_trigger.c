/*
 * LSM6DS3TR-C / LSM6DS33 FIFO-watermark trigger on INT1.
 *
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compiled only when the devicetree node carries `irq-gpios`. Whether INT1 is
 * routed to a GPIO on the Feather Sense is unverified, so the shipping
 * configuration has no such property and the application drains the FIFO on a
 * k_timer instead. What a trigger buys is wake latency and a little CPU; the
 * sample *spacing* comes from the chip's own clock either way, so `period_us`
 * and the simultaneity argument hold with or without it.
 */

#include "lsm6ds3trc.h"
#include "lsm6ds3trc_priv.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(LSM6DS3TRC, CONFIG_LSM6DS3TRC_LOG_LEVEL);

static void lsm6ds3trc_handle_int(const struct device *dev)
{
	struct lsm6ds3trc_data *data = dev->data;

	if (data->watermark_handler != NULL) {
		data->watermark_handler(dev, data->watermark_trigger);
	}
}

static void lsm6ds3trc_gpio_callback(const struct device *port, struct gpio_callback *cb,
				     uint32_t pins)
{
	struct lsm6ds3trc_data *data = CONTAINER_OF(cb, struct lsm6ds3trc_data, gpio_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

#if defined(CONFIG_LSM6DS3TRC_TRIGGER_OWN_THREAD)
	k_sem_give(&data->trig_sem);
#elif defined(CONFIG_LSM6DS3TRC_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&data->work);
#endif
}

#if defined(CONFIG_LSM6DS3TRC_TRIGGER_OWN_THREAD)
static void lsm6ds3trc_thread(void *p1, void *p2, void *p3)
{
	struct lsm6ds3trc_data *data = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->trig_sem, K_FOREVER);
		lsm6ds3trc_handle_int(data->dev);
	}
}
#elif defined(CONFIG_LSM6DS3TRC_TRIGGER_GLOBAL_THREAD)
static void lsm6ds3trc_work_handler(struct k_work *work)
{
	struct lsm6ds3trc_data *data = CONTAINER_OF(work, struct lsm6ds3trc_data, work);

	lsm6ds3trc_handle_int(data->dev);
}
#endif

int lsm6ds3trc_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			   sensor_trigger_handler_t handler)
{
	const struct lsm6ds3trc_config *cfg = dev->config;
	struct lsm6ds3trc_data *data = dev->data;
	lsm6ds3tr_c_int1_route_t route;

	if (trig->type != SENSOR_TRIG_FIFO_WATERMARK) {
		return -ENOTSUP;
	}

	if (gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, GPIO_INT_DISABLE) < 0) {
		return -EIO;
	}

	data->watermark_trigger = trig;
	data->watermark_handler = handler;

	/* Read-modify-write: INT1 carries other sources this driver does not
	 * use, and clobbering them would be a silent change of behaviour.
	 */
	if (lsm6ds3tr_c_pin_int1_route_get(&data->ctx, &route) < 0) {
		return -EIO;
	}
	route.int1_fth = (handler != NULL) ? 1U : 0U;
	if (lsm6ds3tr_c_pin_int1_route_set(&data->ctx, route) < 0) {
		return -EIO;
	}

	if (handler == NULL) {
		return 0;
	}

	if (gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, GPIO_INT_EDGE_TO_ACTIVE) < 0) {
		return -EIO;
	}

	/* The watermark may already be set, in which case INT1 is high and an
	 * edge-triggered line will never produce one. Deliver the first
	 * callback by hand so the stream starts rather than waiting for a
	 * transition that already happened.
	 */
	if (gpio_pin_get_dt(&cfg->irq_gpio) > 0) {
		lsm6ds3trc_handle_int(dev);
	}

	return 0;
}

int lsm6ds3trc_trigger_init(const struct device *dev)
{
	const struct lsm6ds3trc_config *cfg = dev->config;
	struct lsm6ds3trc_data *data = dev->data;

	if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
		LOG_ERR("INT1 GPIO %s is not ready", cfg->irq_gpio.port->name);
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT) < 0) {
		return -EIO;
	}

	gpio_init_callback(&data->gpio_cb, lsm6ds3trc_gpio_callback, BIT(cfg->irq_gpio.pin));
	if (gpio_add_callback(cfg->irq_gpio.port, &data->gpio_cb) < 0) {
		return -EIO;
	}

#if defined(CONFIG_LSM6DS3TRC_TRIGGER_OWN_THREAD)
	k_sem_init(&data->trig_sem, 0, K_SEM_MAX_LIMIT);
	k_thread_create(&data->thread, data->thread_stack, CONFIG_LSM6DS3TRC_THREAD_STACK_SIZE,
			lsm6ds3trc_thread, data, NULL, NULL,
			K_PRIO_COOP(CONFIG_LSM6DS3TRC_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread, "lsm6ds3trc");
#elif defined(CONFIG_LSM6DS3TRC_TRIGGER_GLOBAL_THREAD)
	k_work_init(&data->work, lsm6ds3trc_work_handler);
#endif

	return 0;
}
