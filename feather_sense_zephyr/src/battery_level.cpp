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

uint16_t MillivoltAverage::add(uint16_t millivolts)
{
	/* A ring buffer with a running sum: the cost per reading is one add and
	 * one subtract regardless of the window size, and the sum cannot
	 * overflow a uint32_t -- 30 readings of at most 65535 is 1.97e6.
	 */
	if (count_ == kAverageSamples) {
		sum_ -= samples_[next_];
	} else {
		count_++;
	}

	samples_[next_] = millivolts;
	sum_ += millivolts;
	next_ = (next_ + 1) % kAverageSamples;

	/* Round to nearest rather than truncating: truncation biases the mean
	 * half a millivolt low every time, and the whole point here is to stop
	 * sub-percent noise from moving an integer percent.
	 */
	return static_cast<uint16_t>((sum_ + count_ / 2) / count_);
}

void MillivoltAverage::reset()
{
	sum_ = 0;
	count_ = 0;
	next_ = 0;
}

uint8_t PercentHysteresis::update(uint16_t millivolts)
{
	if (!have_) {
		held_ = percent_from_millivolts(millivolts);
		have_ = true;
		return held_;
	}

	/*
	 * The millivolt span that maps to the held percent, widened by the
	 * deadband at both ends. percent_from_millivolts() is a floor division
	 * by 10, so percent p covers [3200 + 10p, 3200 + 10p + 10) -- except at
	 * the clamps, where 0 runs down to nothing and 100 runs up to nothing.
	 * Testing an edge that does not exist would let a clamped reading
	 * re-report itself forever.
	 */
	const int low = kEmptyMillivolts + held_ * 10 - kDeadbandMillivolts;
	const int high = kEmptyMillivolts + (held_ + 1) * 10 + kDeadbandMillivolts;
	const bool below = held_ > 0 && millivolts < low;
	const bool above = held_ < 100 && millivolts >= high;

	if (below || above) {
		held_ = percent_from_millivolts(millivolts);
	}

	return held_;
}

void PercentHysteresis::reset()
{
	held_ = 0;
	have_ = false;
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
