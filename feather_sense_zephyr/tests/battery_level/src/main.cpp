/*
 * Host unit tests for the battery arithmetic. Copyright (c) 2026 Bob DiMaiolo.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_level.hpp"

#include <zephyr/ztest.h>

#include <stdint.h>

using namespace battery;

ZTEST_SUITE(average, NULL, NULL, NULL, NULL, NULL);

/* The window fills from the first reading rather than from zero, so the LED has
 * a band to show at boot instead of climbing out of a value never measured.
 */
ZTEST(average, test_the_first_reading_is_returned_unchanged)
{
	MillivoltAverage avg;

	zassert_equal(avg.count(), 0);
	zassert_equal(avg.add(4000), 4000);
	zassert_equal(avg.count(), 1);
}

ZTEST(average, test_averages_only_what_it_has_until_the_window_fills)
{
	MillivoltAverage avg;

	zassert_equal(avg.add(4000), 4000);
	zassert_equal(avg.add(4100), 4050);
	zassert_equal(avg.add(4200), 4100);
	zassert_equal(avg.count(), 3);
}

ZTEST(average, test_the_window_is_bounded_and_forgets_the_oldest)
{
	MillivoltAverage avg;

	for (size_t i = 0; i < kAverageSamples; i++) {
		avg.add(4000);
	}
	zassert_equal(avg.count(), kAverageSamples);
	zassert_equal(avg.add(4000), 4000);
	zassert_equal(avg.count(), kAverageSamples, "the window must not grow");

	/* Push the old value entirely out and the mean must arrive exactly. */
	for (size_t i = 0; i < kAverageSamples; i++) {
		avg.add(3800);
	}
	zassert_equal(avg.add(3800), 3800);
}

ZTEST(average, test_rounds_to_nearest_rather_than_truncating)
{
	MillivoltAverage avg;

	/* 4000 and 4001 average to 4000.5, which must not truncate to 4000:
	 * truncation biases every mean half a millivolt low.
	 */
	avg.add(4000);
	zassert_equal(avg.add(4001), 4001);
}

ZTEST(average, test_reset_empties_the_window)
{
	MillivoltAverage avg;

	avg.add(4200);
	avg.add(4200);
	avg.reset();

	zassert_equal(avg.count(), 0);
	zassert_equal(avg.add(3500), 3500);
}

/*
 * The property the filter exists for, stated as the measurement that motivated
 * it: +-25 mV of dither around a steady level must stop moving the percent.
 *
 * Unfiltered, a level sitting on a percent boundary crosses it on almost every
 * sample -- that is the 1100x over-emission in README, "the discharge curve".
 */
ZTEST(average, test_dither_around_a_boundary_stops_moving_the_percent)
{
	const int level = 3800; /* exactly 60 %, a band edge too */
	const int swing[] = {25, -25, 24, -23, 18, -19, 25, -25, 12, -14};

	MillivoltAverage avg;
	uint8_t raw_changes = 0;
	uint8_t smoothed_changes = 0;
	uint8_t raw_prev = percent_from_millivolts(level + swing[0]);
	uint8_t smoothed_prev = 0;

	/* Warm the window at the level, the way a real run arrives at one. */
	for (size_t i = 0; i < kAverageSamples; i++) {
		avg.add(static_cast<uint16_t>(level));
	}
	smoothed_prev = percent_from_millivolts(avg.add(static_cast<uint16_t>(level)));

	for (int round = 0; round < 12; round++) {
		for (size_t i = 0; i < sizeof(swing) / sizeof(swing[0]); i++) {
			const int reading = level + swing[i];

			const uint8_t raw = percent_from_millivolts(reading);
			if (raw != raw_prev) {
				raw_changes++;
			}
			raw_prev = raw;

			const uint8_t smoothed =
				percent_from_millivolts(avg.add(static_cast<uint16_t>(reading)));
			if (smoothed != smoothed_prev) {
				smoothed_changes++;
			}
			smoothed_prev = smoothed;
		}
	}

	zassert_true(raw_changes > 50, "the raw reading should cross constantly, saw %u",
		     raw_changes);
	zassert_true(smoothed_changes <= 2, "the filtered reading should sit still, saw %u",
		     smoothed_changes);
}

/* Suppressing dither must not mean ignoring the discharge. A real 17.5 mV/h
 * drift has to arrive, and arrive close to on time.
 */
ZTEST(average, test_a_real_drift_still_moves_the_percent)
{
	MillivoltAverage avg;
	uint16_t out = 0;

	/* 4000 mV down to 3900: ten whole percent points. */
	for (int mv = 4000; mv >= 3900; mv--) {
		for (int repeat = 0; repeat < 4; repeat++) {
			out = avg.add(static_cast<uint16_t>(mv));
		}
	}

	/* The window lags by half its length in samples, which at 4 samples per
	 * millivolt is under 4 mV -- far inside one percent point.
	 */
	zassert_within(out, 3900, 5, "the average should track the drift, ended at %u", out);
	zassert_equal(percent_from_millivolts(out), 70);
}

ZTEST_SUITE(deadband, NULL, NULL, NULL, NULL, NULL);

ZTEST(deadband, test_the_first_reading_is_exact)
{
	PercentHysteresis h;

	zassert_equal(h.update(3800), 60);
}

ZTEST(deadband, test_holds_through_noise_that_crosses_a_boundary)
{
	/* 3800 mV is exactly 60 %. Without a deadband, 3799 reports 59 and
	 * 3800 reports 60, so noise of a single millivolt chatters.
	 */
	PercentHysteresis h;

	zassert_equal(h.update(3800), 60);
	for (int i = 0; i < 50; i++) {
		zassert_equal(h.update(3799), 60, "one millivolt under must not move it");
		zassert_equal(h.update(3804), 60);
		zassert_equal(h.update(3796), 60, "within the deadband below");
		zassert_equal(h.update(3809), 60, "still inside the held point's span");
	}
}

ZTEST(deadband, test_releases_once_the_reading_clears_the_deadband)
{
	PercentHysteresis h;

	zassert_equal(h.update(3800), 60);
	/* The span of 60 % is [3800, 3810); leaving it needs 5 mV of margin. */
	zassert_equal(h.update(3795), 60, "exactly at the margin still holds");
	zassert_equal(h.update(3794), 59, "one past it releases");

	/* Now holding 59 %, whose span is [3790, 3800) and so runs to 3805 once
	 * the deadband is added.
	 */
	zassert_equal(h.update(3804), 59, "inside the deadband above 59's span");
	zassert_equal(h.update(3805), 60, "clearing above reports the curve");
}

ZTEST(deadband, test_a_jump_reports_the_curve_rather_than_one_step)
{
	PercentHysteresis h;

	zassert_equal(h.update(4000), 80);
	zassert_equal(h.update(3500), 30, "a large move is not rate limited");
}

/* The clamps have only one real edge each. Testing the edge that does not
 * exist would let a pegged reading re-report itself forever.
 */
ZTEST(deadband, test_holds_at_the_clamped_ends)
{
	PercentHysteresis low;

	zassert_equal(low.update(3000), 0);
	zassert_equal(low.update(2000), 0, "further below empty is still 0");
	zassert_equal(low.update(3214), 0, "inside the deadband above 0's span");
	zassert_equal(low.update(3215), 1);

	PercentHysteresis high;

	zassert_equal(high.update(4300), 100);
	zassert_equal(high.update(4500), 100, "further above full is still 100");
	zassert_equal(high.update(4195), 100, "inside the deadband below");
	zassert_equal(high.update(4194), 99);
}

ZTEST(deadband, test_reset_forgets_the_held_value)
{
	PercentHysteresis h;

	h.update(4200);
	h.reset();
	zassert_equal(h.update(3800), 60, "after reset the next reading is exact");
}

/*
 * The two filters together, against the shape of the real measurement: dither
 * on a boundary must stop emitting, and a genuine discharge must still arrive.
 * Either one alone passes a weaker version of this; the pair is the point.
 */
ZTEST(deadband, test_dither_stops_emitting_while_a_real_drift_still_arrives)
{
	const int swing[] = {25, -25, 24, -23, 18, -19, 25, -25, 12, -14};
	MillivoltAverage avg;
	PercentHysteresis h;
	uint8_t changes = 0;
	uint8_t prev = 0;
	bool have_prev = false;

	/* Sit on a boundary and dither for 120 samples. */
	for (int round = 0; round < 12; round++) {
		for (size_t i = 0; i < sizeof(swing) / sizeof(swing[0]); i++) {
			const uint16_t mv = static_cast<uint16_t>(3800 + swing[i]);
			const uint8_t p = h.update(avg.add(mv));

			if (have_prev && p != prev) {
				changes++;
			}
			prev = p;
			have_prev = true;
		}
	}
	zassert_true(changes <= 1, "dither on a boundary should settle, saw %u", changes);

	/* Then discharge 60 mV, six real points, and require them to show up. */
	for (int mv = 3800; mv >= 3740; mv--) {
		for (int repeat = 0; repeat < 4; repeat++) {
			prev = h.update(avg.add(static_cast<uint16_t>(mv)));
		}
	}
	zassert_true(prev <= 55 && prev >= 53, "a real 60 mV drop must arrive, got %u", prev);
}

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
