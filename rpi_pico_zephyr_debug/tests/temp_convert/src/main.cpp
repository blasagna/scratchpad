/*
 * Host unit tests for raw_to_millicelsius(), ported one-for-one from the Rust
 * sibling's rp2040_temp crate. They run on native_sim, where floating point
 * and the host C library are available to cross-check the integer math.
 */

#include "temp_convert.hpp"

#include <zephyr/ztest.h>

#include <math.h>
#include <stdint.h>

/* Zephyr's minimal C++ library (-nostdinc++) doesn't ship <cmath>/<cstdlib>,
 * so use the C math header and a hand-rolled integer abs rather than
 * std::fabs / std::lround / std::abs.
 */
static int32_t abs_i32(int32_t v)
{
	return v < 0 ? -v : v;
}

/* The datasheet formula in floating point, in millidegrees Celsius.
 * Deliberately an independent expression of the same physics rather than a copy
 * of the integer implementation, so it can disagree with it.
 */
static double reference_millicelsius(uint16_t raw)
{
	double volts = static_cast<double>(raw) * 3.3 / 4096.0;

	return (27.0 - (volts - 0.706) / 0.001721) * 1000.0;
}

/* The raw reading a perfect ADC would report for a given die temperature. */
static uint16_t raw_for_celsius(double celsius)
{
	double volts = 0.706 - (celsius - 27.0) * 0.001721;

	return static_cast<uint16_t>(lround(volts * 4096.0 / 3.3));
}

ZTEST_SUITE(temp_convert, NULL, NULL, NULL, NULL, NULL);

/* The headline property: over the whole ADC domain the integer version stays
 * within a couple of millidegrees of the datasheet formula. Two truncating
 * divisions contribute error (microvolts, <0.6 m°C, and the final divide by the
 * slope, <1 m°C), so anything beyond a few m°C is a real disagreement.
 */
ZTEST(temp_convert, test_tracks_datasheet_formula_across_adc_range)
{
	for (uint16_t raw = 0; raw <= 4095; raw++) {
		double ours = static_cast<double>(raw_to_millicelsius(raw));
		double reference = reference_millicelsius(raw);
		double error = fabs(ours - reference);

		zassert_true(error <= 2.0, "raw=%u: got %f m°C, datasheet %f m°C (off by %f)", raw,
			     ours, reference, error);
	}
}

/* Regression test for a units bug: the offset term was once computed in whole
 * degrees and subtracted from a millidegree constant, which pinned every
 * reading to roughly 27000 m°C. Asserted as a span, because that is what such a
 * bug destroys -- it flattens a 105 °C sweep into a 105 m°C wobble.
 */
ZTEST(temp_convert, test_scale_is_millidegrees_not_degrees)
{
	int32_t cold = raw_to_millicelsius(raw_for_celsius(-20.0));
	int32_t hot = raw_to_millicelsius(raw_for_celsius(85.0));
	int32_t span = hot - cold;

	zassert_true(abs_i32(span - 105000) < 1000,
		     "a -20..85 °C sweep should span ~105000 m°C, spanned %d", span);
}

/* Regression test for an overflow: `raw * 3'300'000` does not fit in 32 bits.
 * Exhaustive over the whole uint16_t domain -- the ADC is 12-bit, so anything
 * above 4095 is already out of contract, but the signature accepts it and it
 * must not misbehave. (int64_t math keeps it correct; this guards a regression
 * back to 32-bit intermediates.)
 */
ZTEST(temp_convert, test_no_overflow_across_the_entire_u16_domain)
{
	for (uint32_t raw = 0; raw <= UINT16_MAX; raw++) {
		(void)raw_to_millicelsius(static_cast<uint16_t>(raw));
	}
}

/* The sensor voltage falls as the die warms, so temperature must fall
 * monotonically as the raw reading rises.
 */
ZTEST(temp_convert, test_is_monotonically_decreasing_in_raw)
{
	int32_t previous = raw_to_millicelsius(0);

	for (uint16_t raw = 1; raw <= 4095; raw++) {
		int32_t current = raw_to_millicelsius(raw);

		zassert_true(current <= previous, "raw=%u: %d m°C is warmer than %d m°C at raw=%u",
			     raw, current, previous, raw - 1);
		previous = current;
	}
}

/* The one point the datasheet fixes exactly: 0.706 V is 27 °C. One LSB is
 * 3.3 V / 4096 ~= 806 µV ~= 0.47 °C, so the reading can only land this close.
 */
ZTEST(temp_convert, test_reads_27c_at_the_datasheet_reference_voltage)
{
	uint16_t raw = raw_for_celsius(27.0);
	int32_t millicelsius = raw_to_millicelsius(raw);

	zassert_true(abs_i32(millicelsius - 27000) < 500, "raw=%u should read ~27000 m°C, read %d",
		     raw, millicelsius);
}

/* Spot checks at temperatures a Pico actually sees, to catch a change that
 * keeps the span right but shifts the whole curve.
 */
ZTEST(temp_convert, test_matches_expected_readings_at_realistic_temperatures)
{
	const double points[] = {0.0, 20.0, 27.0, 40.0, 60.0};

	for (double celsius : points) {
		int32_t expected = static_cast<int32_t>(celsius * 1000.0);
		int32_t actual = raw_to_millicelsius(raw_for_celsius(celsius));

		zassert_true(abs_i32(actual - expected) < 500, "%d °C should read ~%d m°C, read %d",
			     static_cast<int>(celsius), expected, actual);
	}
}
