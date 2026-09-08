/* Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0 */

#ifndef FEATHER_SENSE_LED_HPP_
#define FEATHER_SENSE_LED_HPP_

#include "battery_level.hpp"

namespace led
{

int start();

/*
 * Paint the NeoPixel for a battery band. Repaints only on a change of band:
 * ws2812_gpio bit-bangs the line with interrupts locked for the duration --
 * roughly 30 us for one pixel -- which is short, but it is also unnecessary a
 * hundred times out of a hundred and one.
 */
void show(battery::Band band);

/*
 * Drive the pixel directly, for `fs led`. Overrides the band until the next
 * band change repaints it.
 *
 * This exists because a NeoPixel has no readback: the only way to tell a
 * timing fault from a colour-channel-order fault from a wrong pin is to send a
 * known colour and look. Sending pure red, green and blue in turn distinguishes
 * all three at once.
 */
int set(uint8_t r, uint8_t g, uint8_t b);

} /* namespace led */

#endif /* FEATHER_SENSE_LED_HPP_ */
