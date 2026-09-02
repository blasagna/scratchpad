/*
 * Divider millivolts to percent, and the status-LED band with hysteresis.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * Free of Zephyr headers on purpose: tests/battery_level/ compiles this exact
 * file for native_sim.
 */

#ifndef FEATHER_SENSE_BATTERY_LEVEL_HPP_
#define FEATHER_SENSE_BATTERY_LEVEL_HPP_

#include <stdint.h>

namespace battery
{

/* The crude linear curve the CircuitPython port used, carried over unchanged:
 * no lookup table, no open-circuit-voltage correction, no coulomb counting. It
 * is wrong in the way every linear LiPo gauge is wrong, and it is the same kind
 * of wrong on both ports, which is worth more here than being differently
 * approximate.
 */
constexpr int kEmptyMillivolts = 3200;
constexpr int kFullMillivolts = 4200;

/* Clamped to 0..100. */
uint8_t percent_from_millivolts(int millivolts);

enum class Band : uint8_t {
	kUnknown = 0, /* nothing painted yet -- no hysteresis to apply */
	kLow,
	kMedium,
	kHigh,
};

/*
 * The band to display at `percent`, given the band currently displayed.
 *
 * Bare thresholds are 25 % and 60 %; each moves by 3 points against the
 * direction of travel, so leaving red needs 28 %, falling back to it needs
 * below 22 %, climbing to green needs 63 %, and falling out of it needs below
 * 57 %. Nothing latches: the band moves in both directions, and no sequence of
 * inputs can pin it -- which is the property tests/battery_level/ checks, since
 * it is the one a plausible-looking implementation silently loses.
 */
Band band_for(uint8_t percent, Band current);

} /* namespace battery */

#endif /* FEATHER_SENSE_BATTERY_LEVEL_HPP_ */
