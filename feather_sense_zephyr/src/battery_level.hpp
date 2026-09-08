/*
 * Divider millivolts to percent, and the status-LED band with hysteresis.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * Free of Zephyr headers on purpose: tests/battery_level/ compiles this exact
 * file for native_sim.
 */

#ifndef FEATHER_SENSE_BATTERY_LEVEL_HPP_
#define FEATHER_SENSE_BATTERY_LEVEL_HPP_

#include <stddef.h>
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

/*
 * A moving average of the last kAverageSamples readings.
 *
 * The ADC dithers by about 6.7 mV RMS, which is two thirds of a percent point
 * on the curve above, while a real discharge moves 17.5 mV/h -- one point every
 * 34 minutes. Unfiltered, requirement 1.7's "on a change of at least 1 %" fired
 * about 1100 times more often than that, on dither rather than on charge. The
 * measurements are in README, "the discharge curve".
 *
 * 30 samples at the 1 Hz sample period is ~30 s, which cuts the noise to about
 * 1.2 mV (an eighth of a point) for a lag of well under 2 % of the 34-minute
 * interval being measured. Longer windows buy progressively less.
 *
 * Averaging is over *samples*, not over wall-clock: a failed ADC read is never
 * added, so a run of failures widens the span the window covers rather than
 * feeding it invented values.
 */
constexpr size_t kAverageSamples = 30;

class MillivoltAverage
{
      public:
	/*
	 * Adds a reading and returns the mean of what the window now holds.
	 *
	 * Before the window fills this averages only the readings taken so far,
	 * so the first reading is returned unchanged rather than being dragged
	 * toward a zero that was never measured. The LED therefore shows a band
	 * immediately at boot instead of 30 s later.
	 */
	uint16_t add(uint16_t millivolts);

	/* How many readings the window holds, up to kAverageSamples. */
	size_t count() const
	{
		return count_;
	}

	void reset();

      private:
	uint16_t samples_[kAverageSamples] = {};
	uint32_t sum_ = 0;
	size_t count_ = 0;
	size_t next_ = 0;
};

/*
 * Integer percent with a deadband, so a level resting on a boundary stops
 * chattering across it.
 *
 * The 30 s average is most of the fix and not all of it: it leaves about 1.2 mV
 * of noise, and `percent` is an integer, so a filtered level sitting near a
 * boundary still flips back and forth. Replaying a 19.4 h discharge, the raw
 * stream emitted 2008/h, the average alone 53.9/h, and the average plus this
 * 1.2/h -- against 1.20 percent points/h of real change. The measurements are in
 * README, "the reading is averaged over 30 s".
 *
 * This is the same trick `band_for()` already uses one level up, for the same
 * reason: a threshold with no width is a threshold that rings.
 */
constexpr int kDeadbandMillivolts = 5;

class PercentHysteresis
{
      public:
	/*
	 * The percent to report for `millivolts`.
	 *
	 * Holds the last reported value until the reading leaves that value's
	 * millivolt span by more than kDeadbandMillivolts at either end; then
	 * reports whatever the curve now says, which may be several points away
	 * if the reading jumped. The first call reports exactly, so nothing has
	 * to climb out of a value never measured.
	 */
	uint8_t update(uint16_t millivolts);

	void reset();

      private:
	uint8_t held_ = 0;
	bool have_ = false;
};

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
