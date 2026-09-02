/*
 * Host unit tests for the battery arithmetic. Copyright (c) 2026 Bob DiMaiolo.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_level.hpp"

#include <zephyr/ztest.h>

#include <stdint.h>

using namespace battery;

ZTEST_SUITE(percent, NULL, NULL, NULL, NULL, NULL);

ZTEST(percent, test_clamps_outside_the_curve)
{
	zassert_equal(percent_from_millivolts(0), 0);
	zassert_equal(percent_from_millivolts(2000), 0);
	zassert_equal(percent_from_millivolts(kEmptyMillivolts), 0);
	zassert_equal(percent_from_millivolts(kEmptyMillivolts - 1), 0);

	zassert_equal(percent_from_millivolts(kFullMillivolts), 100);
	zassert_equal(percent_from_millivolts(kFullMillivolts + 1), 100);
	zassert_equal(percent_from_millivolts(5000), 100);
}

ZTEST(percent, test_is_linear_across_the_curve)
{
	zassert_equal(percent_from_millivolts(3300), 10);
	zassert_equal(percent_from_millivolts(3700), 50);
	zassert_equal(percent_from_millivolts(4000), 80);
}

/* The span is exactly 1000 mV, so ten millivolts is one percent and nothing
 * rounds away. A change of denominator would show up here first.
 */
ZTEST(percent, test_ten_millivolts_is_one_percent)
{
	for (int p = 0; p <= 100; p++) {
		zassert_equal(percent_from_millivolts(kEmptyMillivolts + p * 10), p);
	}
}

ZTEST(percent, test_never_decreases_as_voltage_rises)
{
	uint8_t previous = 0;

	for (int mv = 0; mv <= 5000; mv++) {
		const uint8_t p = percent_from_millivolts(mv);

		zassert_true(p >= previous, "%d mV read %u %% after %u %%", mv, p, previous);
		zassert_true(p <= 100);
		previous = p;
	}
}

/* --- bands ---------------------------------------------------------------- */

ZTEST_SUITE(band, NULL, NULL, NULL, NULL, NULL);

/* With nothing painted yet there is no hysteresis to apply, so the bare 25/60
 * thresholds decide.
 */
ZTEST(band, test_uses_the_bare_thresholds_when_nothing_is_painted)
{
	zassert_equal(band_for(0, Band::kUnknown), Band::kLow);
	zassert_equal(band_for(24, Band::kUnknown), Band::kLow);
	zassert_equal(band_for(25, Band::kUnknown), Band::kMedium);
	zassert_equal(band_for(59, Band::kUnknown), Band::kMedium);
	zassert_equal(band_for(60, Band::kUnknown), Band::kHigh);
	zassert_equal(band_for(100, Band::kUnknown), Band::kHigh);
}

/* The four documented crossings: 28 to leave red, below 22 to fall back to it,
 * 63 to reach green, below 57 to fall out of it.
 */
ZTEST(band, test_applies_hysteresis_in_both_directions)
{
	zassert_equal(band_for(27, Band::kLow), Band::kLow);
	zassert_equal(band_for(28, Band::kLow), Band::kMedium);

	zassert_equal(band_for(22, Band::kMedium), Band::kMedium);
	zassert_equal(band_for(21, Band::kMedium), Band::kLow);

	zassert_equal(band_for(62, Band::kMedium), Band::kMedium);
	zassert_equal(band_for(63, Band::kMedium), Band::kHigh);

	zassert_equal(band_for(57, Band::kHigh), Band::kHigh);
	zassert_equal(band_for(56, Band::kHigh), Band::kMedium);
}

/*
 * The property the whole thing exists for: the band moves in *both* directions
 * and no sequence of inputs can latch it.
 *
 * An implementation that widened a threshold without narrowing the opposite one
 * passes every crossing test above and still gets stuck -- which is exactly the
 * defect the CircuitPython port's README records shipping, as an LED that
 * displayed a constant amber. A sweep down and back up is what catches it.
 */
ZTEST(band, test_nothing_latches_over_a_full_sweep)
{
	Band current = Band::kUnknown;

	for (int p = 100; p >= 0; p--) {
		current = band_for(static_cast<uint8_t>(p), current);
	}
	zassert_equal(current, Band::kLow, "a sweep down to 0 %% should end at red");

	for (int p = 0; p <= 100; p++) {
		current = band_for(static_cast<uint8_t>(p), current);
	}
	zassert_equal(current, Band::kHigh, "a sweep up to 100 %% should end at green");

	/* And again, so a one-way path is not what made it work. */
	for (int p = 100; p >= 0; p--) {
		current = band_for(static_cast<uint8_t>(p), current);
	}
	zassert_equal(current, Band::kLow);
}

/* Every band must be reachable from every other, or one of them is decoration. */
ZTEST(band, test_every_band_is_reachable_from_every_other)
{
	const Band starts[] = {Band::kUnknown, Band::kLow, Band::kMedium, Band::kHigh};

	for (Band start : starts) {
		zassert_equal(band_for(0, start), Band::kLow);
		zassert_equal(band_for(100, start), Band::kHigh);
		/* 40 % is clear of both hysteresis windows from any state. */
		zassert_equal(band_for(40, start), Band::kMedium);
	}
}

/* Inside a hysteresis window the band must not move at all -- that is what
 * stops a reading jittering across a threshold from strobing the pixel, which
 * is the cost the "repaint only on a band change" rule is buying down.
 */
ZTEST(band, test_holds_steady_inside_the_window)
{
	for (int p = 22; p <= 27; p++) {
		zassert_equal(band_for(static_cast<uint8_t>(p), Band::kLow), Band::kLow);
		zassert_equal(band_for(static_cast<uint8_t>(p), Band::kMedium), Band::kMedium);
	}

	for (int p = 57; p <= 62; p++) {
		zassert_equal(band_for(static_cast<uint8_t>(p), Band::kMedium), Band::kMedium);
		zassert_equal(band_for(static_cast<uint8_t>(p), Band::kHigh), Band::kHigh);
	}
}
