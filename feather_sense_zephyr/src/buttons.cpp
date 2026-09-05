/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board DTS already declares the user button: a gpio-keys `button0` on
 * gpio1 pin 2 with zephyr,code = INPUT_KEY_0 and the sw0 alias. Going through
 * the input subsystem means no debounce code and no polling loop here, and no
 * thread of this module's own -- the callback runs on the input subsystem's.
 */

#include "buttons.hpp"

#include "codec.hpp"
#include "streams.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

namespace
{

const device *const button_port = DEVICE_DT_GET(DT_GPIO_CTLR(DT_ALIAS(sw0), gpios));

void button_handler(input_event *event, void *)
{
	if (event->type != INPUT_EV_KEY) {
		return;
	}

	const codec::ButtonSample sample = {
		.code = static_cast<uint16_t>(event->code),
		.pressed = static_cast<uint8_t>(event->value != 0 ? 1 : 0),
		.pad = 0,
	};

	LOG_DBG("key %u %s", event->code, sample.pressed ? "press" : "release");

	streams::emit(codec::kStreamButton, k_uptime_get_32(), 0, 1, &sample, sizeof(sample));
}

} /* namespace */

/*
 * Registering against every device (NULL) rather than against one means
 * `input report 1 11 1` at the shell drives this whole path, synthetic event
 * included -- which is how the button stream is exercised end to end without
 * anyone touching the board.
 */
INPUT_CALLBACK_DEFINE(NULL, button_handler, NULL);

namespace buttons
{

int start()
{
	if (!device_is_ready(button_port)) {
		LOG_ERR("the button's GPIO controller is not ready");
		return -ENODEV;
	}

	return 0;
}

} /* namespace buttons */
