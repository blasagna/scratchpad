//! Descriptive statistics over a slice of `f64`.
//!
//! This is the *core* crate: plain Rust, no Python awareness at all. Everything
//! Python-facing lives in the sibling `statkit_py` crate, which depends on this
//! one. Keeping the split means this code stays usable (and testable) as an
//! ordinary Rust library, and the binding layer stays visibly thin.

use std::fmt;

/// Summary statistics for a non-empty sample.
///
/// `stddev` is the *sample* standard deviation (Bessel-corrected, `n - 1`), and
/// is `0.0` for a single value.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Summary {
    pub count: usize,
    pub mean: f64,
    pub median: f64,
    pub min: f64,
    pub max: f64,
    pub stddev: f64,
}

/// Everything that can go wrong in this library.
///
/// A single error type with a `Display` impl is what the binding layer needs to
/// produce one clean Python exception; see `bindings/src/lib.rs`.
#[derive(Debug, Clone, PartialEq)]
pub enum StatError {
    /// No values were supplied.
    Empty,
    /// A value was NaN or an infinity, which would poison every statistic.
    NotFinite(f64),
    /// Every value is identical, so the spread is zero and z-scores are undefined.
    Constant(f64),
    /// Every value was finite, but a statistic computed from them was not:
    /// summing values near `f64::MAX` overflows to infinity.
    Overflow,
    /// A token in the input text was not a number.
    Parse { token: String },
}

impl fmt::Display for StatError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StatError::Empty => write!(f, "no values given"),
            StatError::NotFinite(value) => write!(f, "value is not finite: {value}"),
            StatError::Constant(value) => {
                write!(
                    f,
                    "all values are {value}, so the standard deviation is zero"
                )
            }
            StatError::Overflow => write!(f, "the values are too large to summarize"),
            StatError::Parse { token } => write!(f, "not a number: '{token}'"),
        }
    }
}

impl std::error::Error for StatError {}

/// Computes the summary statistics of `values`.
///
/// # Errors
///
/// Returns [`StatError::Empty`] for an empty slice, [`StatError::NotFinite`] if
/// any value is NaN or infinite, and [`StatError::Overflow`] if the values are
/// finite but a statistic derived from them is not.
pub fn summarize(values: &[f64]) -> Result<Summary, StatError> {
    check(values)?;

    let count = values.len();
    let n = count as f64;
    let mean = values.iter().sum::<f64>() / n;

    // Sorting a copy keeps the caller's slice untouched. `total_cmp` is a total
    // order over f64, which is exactly what `sort_by` wants -- and it is sound
    // here because `check` already ruled out NaN.
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let mid = count / 2;
    let median = if count % 2 == 0 {
        (sorted[mid - 1] + sorted[mid]) / 2.0
    } else {
        sorted[mid]
    };

    let stddev = if count < 2 {
        0.0
    } else {
        let sum_sq = values.iter().map(|v| (v - mean).powi(2)).sum::<f64>();
        (sum_sq / (n - 1.0)).sqrt()
    };

    // Checking the inputs is not enough: a sum of finite values can still
    // overflow to infinity, and every statistic derived from it would then be
    // infinite or NaN. Reject that here rather than hand back a poisoned
    // `Summary` -- `zscores` in particular would go on to divide by it.
    if !mean.is_finite() || !median.is_finite() || !stddev.is_finite() {
        return Err(StatError::Overflow);
    }

    Ok(Summary {
        count,
        mean,
        median,
        min: sorted[0],
        max: sorted[count - 1],
        stddev,
    })
}

/// Returns each value's distance from the mean, in standard deviations.
///
/// # Errors
///
/// As [`summarize`], plus [`StatError::Constant`] when the spread is zero (which
/// includes any single-value input), because the z-score is undefined there.
pub fn zscores(values: &[f64]) -> Result<Vec<f64>, StatError> {
    let summary = summarize(values)?;
    if summary.stddev == 0.0 {
        return Err(StatError::Constant(summary.mean));
    }

    Ok(values
        .iter()
        .map(|v| (v - summary.mean) / summary.stddev)
        .collect())
}

/// Parses whitespace- and comma-separated numbers out of `text`.
///
/// Empty tokens are skipped, so `"1, 2,,3"` and `"1 2 3"` parse the same. The
/// result may be empty; it is [`summarize`]'s job to reject that.
///
/// # Errors
///
/// Returns [`StatError::Parse`] naming the first token that is not a number.
pub fn parse_values(text: &str) -> Result<Vec<f64>, StatError> {
    text.split(|c: char| c.is_whitespace() || c == ',')
        .filter(|token| !token.is_empty())
        .map(|token| {
            token.parse::<f64>().map_err(|_| StatError::Parse {
                token: token.to_string(),
            })
        })
        .collect()
}

/// Rejects the inputs no statistic can be computed from.
fn check(values: &[f64]) -> Result<(), StatError> {
    if values.is_empty() {
        return Err(StatError::Empty);
    }
    match values.iter().find(|v| !v.is_finite()) {
        Some(&value) => Err(StatError::NotFinite(value)),
        None => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Compares floats that are the result of arithmetic, not exact literals.
    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    #[test]
    fn summarize_computes_every_field() {
        let summary = summarize(&[1.0, 2.0, 3.0, 4.0, 10.0]).unwrap();
        assert_eq!(summary.count, 5);
        assert!(close(summary.mean, 4.0));
        assert!(close(summary.median, 3.0));
        assert!(close(summary.min, 1.0));
        assert!(close(summary.max, 10.0));
        // Sample variance: (9 + 4 + 1 + 0 + 36) / 4 = 12.5
        assert!(close(summary.stddev, 12.5_f64.sqrt()));
    }

    #[test]
    fn summarize_averages_the_middle_pair_on_even_counts() {
        let summary = summarize(&[4.0, 1.0, 3.0, 2.0]).unwrap();
        assert!(close(summary.median, 2.5));
    }

    #[test]
    fn summarize_leaves_the_input_order_alone() {
        let values = [3.0, 1.0, 2.0];
        summarize(&values).unwrap();
        assert_eq!(values, [3.0, 1.0, 2.0]);
    }

    #[test]
    fn summarize_gives_a_single_value_zero_spread() {
        let summary = summarize(&[7.5]).unwrap();
        assert_eq!(summary.count, 1);
        assert!(close(summary.mean, 7.5));
        assert!(close(summary.median, 7.5));
        assert_eq!(summary.stddev, 0.0);
    }

    #[test]
    fn summarize_rejects_empty_input() {
        assert_eq!(summarize(&[]), Err(StatError::Empty));
    }

    #[test]
    fn summarize_rejects_non_finite_values() {
        assert_eq!(
            summarize(&[1.0, f64::INFINITY]),
            Err(StatError::NotFinite(f64::INFINITY))
        );
        assert!(matches!(
            summarize(&[f64::NAN]),
            Err(StatError::NotFinite(_))
        ));
    }

    #[test]
    fn summarize_rejects_finite_input_whose_statistics_overflow() {
        // The inputs pass `check`, but their sum is +inf.
        assert_eq!(summarize(&[1e308, 1e308]), Err(StatError::Overflow));
        // Here the mean is 0.0; it is the sum of squares that overflows.
        assert_eq!(summarize(&[1e200, -1e200]), Err(StatError::Overflow));
    }

    #[test]
    fn zscores_are_centered_and_scaled() {
        let scores = zscores(&[1.0, 2.0, 3.0]).unwrap();
        assert!(close(scores[0], -1.0));
        assert!(close(scores[1], 0.0));
        assert!(close(scores[2], 1.0));
    }

    #[test]
    fn zscores_reject_a_constant_sample() {
        assert_eq!(zscores(&[2.0, 2.0, 2.0]), Err(StatError::Constant(2.0)));
        assert_eq!(zscores(&[2.0]), Err(StatError::Constant(2.0)));
    }

    #[test]
    fn parse_values_accepts_whitespace_and_commas() {
        assert_eq!(
            parse_values(" 1, 2,,3\n-4.5\t6e2 ").unwrap(),
            vec![1.0, 2.0, 3.0, -4.5, 600.0]
        );
    }

    #[test]
    fn parse_values_of_blank_text_is_empty() {
        assert_eq!(parse_values("  \n , ").unwrap(), Vec::<f64>::new());
    }

    #[test]
    fn parse_values_names_the_offending_token() {
        assert_eq!(
            parse_values("1 two 3"),
            Err(StatError::Parse {
                token: "two".to_string()
            })
        );
    }
}
