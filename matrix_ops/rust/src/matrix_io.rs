//! Turning text into a [`Matrix`] and a [`Matrix`] back into text.

use crate::matrix::{Error, Matrix};

/// Decimal places used when `--precision` is not given.
pub const DEFAULT_PRECISION: usize = 4;

/// Ceiling on `--precision`. Past about 1074 places every digit is a zero the
/// trimming removes, and uncapped a single cell can demand gigabytes.
pub const MAX_PRECISION: usize = 1100;

/// Spaces printed between columns.
const COLUMN_GAP: &str = "  ";

/// Parses text into a matrix.
///
/// Dimensions are optional and the layout is the default: one non-blank line of
/// values is a `1 x N` row vector, and several lines are rows. Blank lines carry
/// no values and are not rows, which is what lets input be padded or end in a
/// newline. Rows of differing length are an error *before* any requested shape
/// is considered — a file whose rows differ in length is far more often a typo
/// than a request to reshape.
///
/// Given both dimensions the values are reshaped row-major and their count must
/// be exactly `rows * cols`; given one, the other is derived by division.
pub fn parse(
    text: &str,
    want_rows: Option<usize>,
    want_cols: Option<usize>,
) -> Result<Matrix, Error> {
    let (values, natural_rows, natural_cols) = scan(text)?;
    if values.is_empty() {
        return Err(Error::Empty);
    }

    let (rows, cols) = resolve_shape(
        values.len(),
        (natural_rows, natural_cols),
        want_rows,
        want_cols,
    )?;

    // The values are already row-major and `resolve_shape` has established that
    // there are exactly `rows * cols` of them, so this is a move, not a copy.
    Matrix::from_vec(rows, cols, values)
}

/// Parses one whitespace-delimited token as a finite number.
///
/// `f64::from_str` is the parser but not the whole rule: it also accepts
/// `inf`, `infinity`, and `nan`, and maps an overflow to an infinity. All of
/// those are refused here so the accepted set is written down rather than
/// inherited. An underflow is *not* an error — `1e-400` parses to zero, which
/// is a fine answer.
///
/// This is where the port diverges from C and C++, which use `strtod` and so
/// also accept C99 hex floats like `0x1p3`. `f64::from_str` rejects those, and
/// hand-rolling hex-float parsing to close a gap nobody types is not worth the
/// rounding-correctness burden it carries.
fn parse_number(token: &str) -> Result<f64, Error> {
    match token.parse::<f64>() {
        Ok(value) if value.is_finite() => Ok(value),
        _ => Err(Error::BadNumber(token.to_string())),
    }
}

/// Scans text into values and reports the layout the text itself implies: the
/// number of non-blank lines, and the number of values on each of them.
fn scan(text: &str) -> Result<(Vec<f64>, usize, usize), Error> {
    let mut values = Vec::new();
    let mut rows = 0;
    let mut cols = 0;

    // `str::lines` splits on '\n' and drops a trailing '\r', which is why a
    // CRLF file needs no special handling.
    for line in text.lines() {
        let mut line_cols = 0;
        for token in line.split_whitespace() {
            values.push(parse_number(token)?);
            line_cols += 1;
        }

        if line_cols == 0 {
            continue;
        }
        if rows == 0 {
            cols = line_cols;
        } else if line_cols != cols {
            return Err(Error::Ragged {
                expected: cols,
                found: line_cols,
            });
        }
        rows += 1;
    }

    Ok((values, rows, cols))
}

/// Turns the layout's shape and the requested dimensions into the final one.
///
/// All four cases live here rather than scattered through `parse`, so a port
/// has one function to reproduce.
fn resolve_shape(
    count: usize,
    natural: (usize, usize),
    want_rows: Option<usize>,
    want_cols: Option<usize>,
) -> Result<(usize, usize), Error> {
    let bad_shape = || Error::BadShape {
        count,
        rows: want_rows,
        cols: want_cols,
    };

    match (want_rows, want_cols) {
        (None, None) => Ok(natural),
        (Some(rows), Some(cols)) => {
            // Checked before comparing: a wrapped product could equal `count`
            // and yield a matrix of the wrong shape.
            let requested = rows
                .checked_mul(cols)
                .ok_or(Error::Overflow { rows, cols })?;
            if requested != count {
                return Err(bad_shape());
            }
            Ok((rows, cols))
        }
        (Some(rows), None) => {
            if rows == 0 || !count.is_multiple_of(rows) {
                return Err(bad_shape());
            }
            Ok((rows, count / rows))
        }
        (None, Some(cols)) => {
            if cols == 0 || !count.is_multiple_of(cols) {
                return Err(bad_shape());
            }
            Ok((count / cols, cols))
        }
    }
}

/// Renders a matrix: one row per line, every line newline-terminated including
/// the last.
///
/// Each element is rendered to `precision` decimal places with trailing zeros
/// trimmed, then right-justified into the width of the widest rendering so the
/// decimal points line up down each column.
pub fn render(m: &Matrix, precision: usize) -> String {
    // Everything is rendered before anything is emitted, because the widest
    // cell sets the width all of them are justified into.
    let cells: Vec<String> = m
        .as_slice()
        .iter()
        .map(|value| format_element(*value, precision))
        .collect();
    let width = cells.iter().map(String::len).max().unwrap_or(0);

    let mut out = String::new();
    for r in 0..m.rows() {
        for c in 0..m.cols() {
            if c > 0 {
                out.push_str(COLUMN_GAP);
            }
            out.push_str(&format!("{:>width$}", cells[r * m.cols() + c]));
        }
        out.push('\n');
    }
    out
}

/// Renders one element, trimming trailing zeros so an integral value prints as
/// an integer.
///
/// Fixed notation always (`{:.N}`), never `{}`: the shortest-representation
/// formatter counts significant digits rather than decimals and switches to
/// scientific notation for large values, which reads badly in a column.
fn format_element(value: f64, precision: usize) -> String {
    let mut text = format!("{value:.precision$}");

    // Only a rendering that has a fraction can be trimmed; at precision 0 there
    // is no '.' and every digit is significant.
    if text.contains('.') {
        text.truncate(text.trim_end_matches('0').len());
        text.truncate(text.trim_end_matches('.').len());
    }

    // Normalize a negative zero. It arrives two ways — the f64 -0.0, and a small
    // negative rounded away by the precision — so the check is on the rendering
    // rather than on the value. "-0" in a result reads as a bug.
    if text == "-0" {
        text = "0".to_string();
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parsed(text: &str) -> Matrix {
        parse(text, None, None).expect("should parse")
    }

    fn shaped(text: &str, rows: Option<usize>, cols: Option<usize>) -> Matrix {
        parse(text, rows, cols).expect("should parse")
    }

    // --- shape ---

    #[test]
    fn a_single_line_is_a_row_vector() {
        let m = parsed("1 2 3");
        assert_eq!(m.shape(), (1, 3));
        assert_eq!(m.as_slice(), [1.0, 2.0, 3.0]);
    }

    #[test]
    fn a_single_value_is_one_by_one() {
        assert_eq!(parsed("42").shape(), (1, 1));
    }

    #[test]
    fn newlines_delimit_rows() {
        let m = parsed("1 2\n3 4\n5 6");
        assert_eq!(m.shape(), (3, 2));
        assert_eq!(m.as_slice(), [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
    }

    #[test]
    fn a_final_newline_is_optional() {
        assert_eq!(parsed("1 2\n3 4\n"), parsed("1 2\n3 4"));
    }

    #[test]
    fn blank_lines_and_surrounding_whitespace_are_ignored() {
        assert_eq!(parsed("\n\n  1 2  \n\n  3 4  \n\n"), parsed("1 2\n3 4"));
    }

    #[test]
    fn tabs_separate_values_and_do_not_start_a_row() {
        let m = parsed("1\t2\t3");
        assert_eq!(m.shape(), (1, 3));
    }

    #[test]
    fn carriage_returns_are_just_whitespace() {
        assert_eq!(parsed("1 2\r\n3 4\r\n"), parsed("1 2\n3 4"));
    }

    #[test]
    fn ragged_rows_are_always_an_error() {
        assert_eq!(
            parse("1 2 3\n4 5", None, None),
            Err(Error::Ragged {
                expected: 3,
                found: 2
            })
        );
        // Even when dimensions that would fit the value count are given.
        assert!(parse("1 2 3\n4 5", Some(1), Some(5)).is_err());
    }

    #[test]
    fn empty_input_is_an_error() {
        assert_eq!(parse("", None, None), Err(Error::Empty));
        assert_eq!(parse("  \n\n  \n", None, None), Err(Error::Empty));
    }

    #[test]
    fn both_dimensions_reshape_a_flat_list() {
        let m = shaped("1 2 3 4 5 6", Some(2), Some(3));
        assert_eq!(m.shape(), (2, 3));
        assert_eq!(m[(1, 0)], 4.0);
    }

    #[test]
    fn both_dimensions_override_the_layout() {
        let m = shaped("1 2\n3 4\n5 6", Some(3), Some(2));
        assert_eq!(m.shape(), (3, 2));
        let flat = shaped("1 2\n3 4\n5 6", Some(1), Some(6));
        assert_eq!(flat.shape(), (1, 6));
    }

    #[test]
    fn both_dimensions_must_match_the_value_count() {
        assert!(parse("1 2 3 4 5 6", Some(2), Some(2)).is_err());
        assert!(parse("1 2 3 4 5 6", Some(3), Some(3)).is_err());
    }

    #[test]
    fn rows_alone_derives_the_columns() {
        assert_eq!(shaped("1 2 3 4 5 6", Some(3), None).shape(), (3, 2));
    }

    #[test]
    fn cols_alone_derives_the_rows() {
        assert_eq!(shaped("1 2 3 4 5 6", None, Some(3)).shape(), (2, 3));
    }

    #[test]
    fn a_single_dimension_must_divide_the_value_count() {
        assert!(parse("1 2 3 4 5", Some(2), None).is_err());
        assert!(parse("1 2 3 4 5", None, Some(2)).is_err());
    }

    // --- numbers ---

    #[test]
    fn accepts_signs_decimals_and_exponents() {
        let m = parsed("-2.5 +3 1e3 -1.5E-2 .5 4.");
        assert_eq!(m.as_slice(), [-2.5, 3.0, 1000.0, -0.015, 0.5, 4.0]);
    }

    #[test]
    fn rejects_non_numeric_tokens() {
        for token in ["abc", "1.2.3", "5x", "--5", "1,2"] {
            assert!(
                parse(token, None, None).is_err(),
                "{token} should be rejected"
            );
        }
    }

    #[test]
    fn rejects_the_non_finite_spellings_the_parser_would_otherwise_accept() {
        for token in ["nan", "NaN", "inf", "-inf", "-INF", "infinity"] {
            assert_eq!(
                parse(token, None, None),
                Err(Error::BadNumber(token.to_string()))
            );
        }
    }

    #[test]
    fn rejects_a_value_too_large_to_represent() {
        assert!(parse("1e400", None, None).is_err());
    }

    #[test]
    fn accepts_a_value_that_underflows_to_zero() {
        assert_eq!(parsed("1e-400").as_slice(), [0.0]);
    }

    #[test]
    fn rejects_hex_floats_unlike_the_c_and_cpp_ports() {
        // strtod accepts these; f64::from_str does not. A deliberate, documented
        // divergence rather than an accident.
        assert!(parse("0x10", None, None).is_err());
        assert!(parse("0x1p3", None, None).is_err());
    }

    // --- rendering ---

    fn rendered(rows: usize, cols: usize, values: &[f64], precision: usize) -> String {
        let m = Matrix::from_vec(rows, cols, values.to_vec()).unwrap();
        render(&m, precision)
    }

    #[test]
    fn trims_trailing_zeros() {
        let out = rendered(2, 2, &[1.0, 2.0, 3.0, 4.0], DEFAULT_PRECISION);
        assert_eq!(out, "1  2\n3  4\n");
    }

    #[test]
    fn right_justifies_into_a_common_width() {
        let out = rendered(2, 2, &[1.0, 22.0, 333.0, 4444.0], DEFAULT_PRECISION);
        assert_eq!(out, "   1    22\n 333  4444\n");
    }

    #[test]
    fn keeps_fractions_up_to_the_precision() {
        let out = rendered(1, 2, &[0.5, 0.25], DEFAULT_PRECISION);
        assert_eq!(out, " 0.5  0.25\n");
    }

    #[test]
    fn breaks_a_rounding_tie_towards_the_even_digit() {
        // The canary for formatter drift: 0.25 at one decimal is 0.2, not 0.3.
        let out = rendered(1, 2, &[0.25, 0.45], 1);
        assert_eq!(out, "0.2  0.5\n");
    }

    #[test]
    fn rounds_a_third_to_the_precision() {
        let out = rendered(1, 1, &[1.0 / 3.0], DEFAULT_PRECISION);
        assert_eq!(out, "0.3333\n");
    }

    #[test]
    fn never_prints_a_negative_zero() {
        // -0.0 outright, and a small negative the precision rounds away.
        let out = rendered(1, 3, &[-0.0, -0.00001, -1.0], DEFAULT_PRECISION);
        assert_eq!(out, " 0   0  -1\n");
    }

    #[test]
    fn every_line_ends_in_a_newline_including_the_last() {
        let out = rendered(3, 1, &[1.0, 2.0, 3.0], DEFAULT_PRECISION);
        assert_eq!(out, "1\n2\n3\n");
    }

    #[test]
    fn negative_values_widen_the_column() {
        let out = rendered(2, 2, &[-1.0, 2.0, 3.0, -4.0], DEFAULT_PRECISION);
        assert_eq!(out, "-1   2\n 3  -4\n");
    }

    #[test]
    fn precision_zero_leaves_no_decimal_point() {
        let out = rendered(1, 2, &[1.5, 2.4], 0);
        assert_eq!(out, "2  2\n");
    }

    #[test]
    fn renders_a_width_no_fixed_buffer_would_hold() {
        // 1e300 at 300 decimals is 301 digits, a point, and 300 more.
        let out = rendered(1, 1, &[1e300], 300);
        assert_eq!(out.len(), 302); // 301 digits + '\n'; the fraction trims away
        assert!(out.starts_with('1'));
    }

    #[test]
    fn never_uses_scientific_notation() {
        let out = rendered(1, 1, &[1e21], DEFAULT_PRECISION);
        assert!(!out.contains('e'), "got {out:?}");
    }

    #[test]
    fn round_trips_through_parse() {
        let m = Matrix::from_vec(2, 3, vec![1.5, -2.25, 3.0, 0.5, 100.0, -0.125]).unwrap();
        assert_eq!(
            parse(&render(&m, DEFAULT_PRECISION), None, None).unwrap(),
            m
        );
    }
}
