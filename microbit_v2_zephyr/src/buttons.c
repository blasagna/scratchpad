/*
 * Both buttons report press and release over BLE. Beyond that they diverge:
 * B buzzes, A starts an audio capture.
 *
 * A must never buzz. The speaker sits centimetres from the microphone on the
 * same board, so a tone on A would be measured by A's own FFT. For the same
 * reason B's buzz is suppressed while a capture is running.
 */

#include "buttons.h"
#include "audio.h"
#include "ble.h"

#include <zephyr/device.h>
#include <zephyr/drivers/buzzer.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_DBG);

#define BUZZ_FREQ_HZ     440
#define BUZZ_DURATION_MS 80

#define BUTTON_A 0
#define BUTTON_B 1

static const struct device *const buzzer = DEVICE_DT_GET(DT_NODELABEL(buzzer));

static void button_handler(struct input_event *evt, void *user_data)
{
	uint8_t button;
	bool pressed = (evt->value != 0);

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_A:
		button = BUTTON_A;
		break;
	case INPUT_KEY_B:
		button = BUTTON_B;
		break;
	default:
		return;
	}

	LOG_DBG("button %c %s", button == BUTTON_A ? 'A' : 'B', pressed ? "press" : "release");
	ble_notify_button(button, pressed ? 1 : 0, k_uptime_get_32());

	if (!pressed) {
		return;
	}

	if (button == BUTTON_A) {
		audio_request_capture();
	} else if (!audio_capture_active()) {
		int err = buzzer_tone(buzzer, BUZZ_FREQ_HZ, BUZZ_DURATION_MS);

		if (err) {
			LOG_WRN("buzzer failed (%d)", err);
		}
	}
}

INPUT_CALLBACK_DEFINE(NULL, button_handler, NULL);

int buttons_init(void)
{
	if (!device_is_ready(buzzer)) {
		LOG_ERR("buzzer not ready");
		return -ENODEV;
	}

	return 0;
}
