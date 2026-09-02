/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_level.hpp"

namespace battery
{
namespace
{

constexpr int kRedMax = 25;
constexpr int kYellowMax = 60;
constexpr int kHysteresis = 3;

} /* namespace */

uint8_t percent_from_millivolts(int millivolts)
{
	if (millivolts <= kEmptyMillivolts) {
		return 0;
	}
	if (millivolts >= kFullMillivolts) {
		return 100;
	}

	/* Integer arithmetic throughout: the span is 1000 mV, so this is a
	 * division by 10 and cannot lose a percent to rounding.
	 */
	return static_cast<uint8_t>((millivolts - kEmptyMillivolts) * 100 /
				    (kFullMillivolts - kEmptyMillivolts));
}

Band band_for(uint8_t percent, Band current)
{
	int red_max = kRedMax;
	int yellow_max = kYellowMax;

	switch (current) {
	case Band::kLow:
		red_max += kHysteresis;
		break;
	case Band::kMedium:
		red_max -= kHysteresis;
		yellow_max += kHysteresis;
		break;
	case Band::kHigh:
		yellow_max -= kHysteresis;
		break;
	case Band::kUnknown:
		break;
	}

	if (percent < red_max) {
		return Band::kLow;
	}
	if (percent < yellow_max) {
		return Band::kMedium;
	}

	return Band::kHigh;
}

} /* namespace battery */
