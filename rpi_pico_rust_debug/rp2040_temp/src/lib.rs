//! Conversion from the RP2040's onboard temperature-sensor ADC reading to a
//! temperature, split out of the firmware crate so it can be unit-tested on
//! the host. The firmware itself targets `thumbv6m-none-eabi`, where the
//! libtest harness has no `std` to link against.
//!
//! `#![no_std]` under normal builds — the firmware links this crate — but the
//! test harness needs `std`, so it is lifted for `cargo test`.

#![cfg_attr(not(test), no_std)]

/// ADC reference voltage in microvolts (the RP2040 samples against 3.3 V).
const VREF_UV: i64 = 3_300_000;

/// Full scale of the RP2040's 12-bit SAR ADC.
const ADC_FULL_SCALE: i64 = 4096;

/// Sensor voltage at 27 °C, in microvolts (RP2040 datasheet, section 4.9.5).
const V_AT_27C_UV: i64 = 706_000;

/// Sensor slope: microvolts per degree Celsius, negated — the voltage falls
/// as the die warms. The datasheet gives 1.721 mV/°C.
const UV_PER_DEGREE: i64 = 1721;

/// Converts a 12-bit reading from the RP2040's onboard temperature sensor
/// into millidegrees Celsius, using the formula from the RP2040 datasheet
/// (section 4.9.5): `T = 27 - (V - 0.706) / 0.001721`.
///
/// Works in microvolts so the division doesn't quantize the result to
/// ~0.58 °C steps, and in `i64` throughout — both `raw * 3_300_000` and the
/// scaled offset overflow `i32`, which debug builds (overflow checks on)
/// would turn into a panic.
///
/// ```
/// // 0.706 V is 27 °C by definition, and 0.706 V is raw ≈ 876.
/// let millicelsius = rp2040_temp::raw_to_millicelsius(876);
/// assert!((millicelsius - 27_000).abs() < 500);
/// ```
pub fn raw_to_millicelsius(raw: u16) -> i32 {
    let voltage_uv = (raw as i64) * VREF_UV / ADC_FULL_SCALE;
    (27_000 - (voltage_uv - V_AT_27C_UV) * 1000 / UV_PER_DEGREE) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The datasheet formula in floating point, in millidegrees Celsius.
    /// Deliberately an independent expression of the same physics rather than
    /// a copy of the integer implementation, so it can disagree with it.
    fn reference_millicelsius(raw: u16) -> f64 {
        let volts = f64::from(raw) * 3.3 / 4096.0;
        (27.0 - (volts - 0.706) / 0.001721) * 1000.0
    }

    /// The raw reading a perfect ADC would report for a given die temperature.
    fn raw_for_celsius(celsius: f64) -> u16 {
        let volts = 0.706 - (celsius - 27.0) * 0.001721;
        (volts * 4096.0 / 3.3).round() as u16
    }

    /// The headline property: over the whole ADC domain the integer version
    /// stays within a couple of millidegrees of the datasheet formula.
    ///
    /// Two truncating divisions contribute error — microvolts (<1 µV, i.e.
    /// <0.6 m°C) and the final divide by the slope (<1 m°C) — so anything
    /// beyond a few m°C is a real disagreement, not rounding.
    #[test]
    fn tracks_the_datasheet_formula_across_the_adc_range() {
        for raw in 0..=4095u16 {
            let ours = f64::from(raw_to_millicelsius(raw));
            let reference = reference_millicelsius(raw);
            let error = (ours - reference).abs();
            assert!(
                error <= 2.0,
                "raw={raw}: got {ours} m°C, datasheet says {reference:.1} m°C (off by {error:.1})"
            );
        }
    }

    /// Regression test for a units bug: the offset term was once computed in
    /// whole degrees and subtracted from a millidegree constant, which pinned
    /// every reading to roughly 27000 m°C no matter how hot the die got.
    ///
    /// Asserted as a span rather than a point, because that is what the bug
    /// destroyed — it left a 105 °C sweep looking like a 105 m°C wobble.
    #[test]
    fn scale_is_millidegrees_not_degrees() {
        let cold = raw_to_millicelsius(raw_for_celsius(-20.0));
        let hot = raw_to_millicelsius(raw_for_celsius(85.0));
        let span = hot - cold;
        assert!(
            (span - 105_000).abs() < 1_000,
            "a -20..85 °C sweep should span ~105000 m°C, spanned {span}"
        );
    }

    /// Regression test for an `i32` overflow: `raw * 3_300_000` does not fit
    /// in 32 bits, and debug builds turn that into a panic rather than a
    /// wrapped value. Exhaustive over `u16` — the ADC is 12-bit, so anything
    /// above 4095 is already out of contract, but the signature accepts it
    /// and it must not panic.
    #[test]
    fn no_overflow_across_the_entire_u16_domain() {
        for raw in 0..=u16::MAX {
            let _ = raw_to_millicelsius(raw);
        }
    }

    /// The sensor voltage falls as the die warms, so temperature must fall
    /// monotonically as the raw reading rises.
    #[test]
    fn is_monotonically_decreasing_in_raw() {
        let mut previous = raw_to_millicelsius(0);
        for raw in 1..=4095u16 {
            let current = raw_to_millicelsius(raw);
            assert!(
                current <= previous,
                "raw={raw}: {current} m°C is warmer than {previous} m°C at raw={}",
                raw - 1
            );
            previous = current;
        }
    }

    /// The one point the datasheet fixes exactly: 0.706 V is 27 °C.
    #[test]
    fn reads_27c_at_the_datasheet_reference_voltage() {
        let raw = raw_for_celsius(27.0);
        let millicelsius = raw_to_millicelsius(raw);
        // One LSB is 3.3 V / 4096 ≈ 806 µV ≈ 0.47 °C, so the reading can only
        // land this close no matter how exact the arithmetic is.
        assert!(
            (millicelsius - 27_000).abs() < 500,
            "raw={raw} should read ~27000 m°C, read {millicelsius}"
        );
    }

    /// Spot checks at temperatures a Pico actually sees, to catch a change
    /// that keeps the span right but shifts the whole curve.
    #[test]
    fn matches_expected_readings_at_realistic_temperatures() {
        for celsius in [0.0, 20.0, 27.0, 40.0, 60.0] {
            let expected = (celsius * 1000.0) as i32;
            let actual = raw_to_millicelsius(raw_for_celsius(celsius));
            assert!(
                (actual - expected).abs() < 500,
                "{celsius} °C should read ~{expected} m°C, read {actual}"
            );
        }
    }
}
