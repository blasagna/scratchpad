/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * The NeoPixel is the only part on this board that can express three bands:
 * the two plain LEDs are red (P1.09) and blue (P1.10) and cannot make green.
 */

#include "led.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

namespace led
{
namespace
{

const device *const strip = DEVICE_DT_GET(DT_NODELABEL(neopixel));

battery::Band painted = battery::Band::kUnknown;

/* The CircuitPython port's colors, unchanged -- it was already right. */
constexpr led_rgb kRed = {.r = 255, .g = 0, .b = 0};
constexpr led_rgb kYellow = {.r = 255, .g = 160, .b = 0};
constexpr led_rgb kGreen = {.r = 0, .g = 255, .b = 0};
constexpr led_rgb kOff = {.r = 0, .g = 0, .b = 0};

} /* namespace */

void show(battery::Band band)
{
	if (band == painted) {
		return;
	}

	led_rgb pixel;

	switch (band) {
	case battery::Band::kLow:
		pixel = kRed;
		break;
	case battery::Band::kMedium:
		pixel = kYellow;
		break;
	case battery::Band::kHigh:
		pixel = kGreen;
		break;
	default:
		pixel = kOff;
		break;
	}

	const int ret = led_strip_update_rgb(strip, &pixel, 1);
	if (ret != 0) {
		LOG_WRN("could not update the pixel (%d)", ret);
		return;
	}

	painted = band;
}

int set(uint8_t r, uint8_t g, uint8_t b)
{
	led_rgb pixel = {.r = r, .g = g, .b = b};

	const int ret = led_strip_update_rgb(strip, &pixel, 1);
	if (ret != 0) {
		return ret;
	}

	/* Forget what was painted, so the next band change repaints rather than
	 * deciding it is already showing the right thing.
	 */
	painted = battery::Band::kUnknown;

	return 0;
}

int start()
{
	if (!device_is_ready(strip)) {
		LOG_ERR("%s is not ready", strip->name);
		return -ENODEV;
	}

	/* A ws2812-gpio write to a wrong-but-valid pin fails silently, so a
	 * dark pixel here is the first thing to doubt. The pin (P0.16) comes
	 * from the Adafruit pinout, not from Zephyr's board files -- nothing in
	 * them describes it.
	 */
	show(battery::Band::kUnknown);

	return 0;
}

} /* namespace led */
