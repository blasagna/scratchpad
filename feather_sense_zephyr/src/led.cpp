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

/* The CircuitPython port's hues, scaled to ~10% brightness: at full scale the
 * pixel is a glare on a desk, and the band is legible long before that. The
 * channel ratios are kept, so yellow stays yellow. `set()` below is deliberately
 * not scaled -- the diagnostic path must be able to ask for full brightness.
 */
constexpr led_rgb kRed = {.r = 24, .g = 0, .b = 0};
constexpr led_rgb kYellow = {.r = 24, .g = 15, .b = 0};
constexpr led_rgb kGreen = {.r = 0, .g = 24, .b = 0};
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

	/*
	 * Blank the pixel, unconditionally.
	 *
	 * Not via show(kUnknown): `painted` starts at kUnknown, so that call
	 * matches its own early return and never reaches the strip at all -- the
	 * write below used to happen only by accident of never happening. It has
	 * to happen. The WS2812 latches its colour and this board is reset
	 * without being power-cycled (`fs bootloader`), so a warm start would
	 * otherwise keep the previous image's band lit until the battery thread
	 * repainted it a second later.
	 *
	 * It is also the only proof the strip is driveable at all: a ws2812-gpio
	 * write to a wrong-but-valid pin fails silently, so a dark pixel here is
	 * the first thing to doubt. The pin (P0.16) comes from the Adafruit
	 * pinout, not from Zephyr's board files -- nothing in them describes it.
	 */
	/* A copy, not &kOff: the API takes a non-const pointer and its own
	 * documentation warns that it may overwrite the pixels it is handed.
	 * show() takes the same precaution.
	 */
	led_rgb pixel = kOff;

	const int ret = led_strip_update_rgb(strip, &pixel, 1);
	if (ret != 0) {
		LOG_ERR("could not blank the pixel (%d)", ret);
		return ret;
	}

	return 0;
}

} /* namespace led */
