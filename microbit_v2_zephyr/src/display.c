/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thin wrapper over Zephyr's mb_display, which sits on top of the
 * nordic,nrf-led-matrix driver. All of its calls are asynchronous: they hand
 * the work to a background queue and a new call cancels the one in flight.
 */

#include "display.h"

#include <zephyr/display/mb_display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* Milliseconds per character while scrolling. */
#define SCROLL_MS 300

static struct mb_display *disp;

int display_init(void)
{
	disp = mb_display_get();
	if (disp == NULL) {
		LOG_ERR("LED matrix display unavailable");
		return -ENODEV;
	}

	return 0;
}

void display_frequency(uint32_t hz)
{
	if (disp == NULL) {
		return;
	}

	mb_display_print(disp, MB_DISPLAY_MODE_SCROLL, SCROLL_MS, "%u", hz);
}

void display_text(const char *text)
{
	if (disp == NULL) {
		return;
	}

	mb_display_print(disp, MB_DISPLAY_MODE_SCROLL, SCROLL_MS, "%s", text);
}
