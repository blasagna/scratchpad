//! Integration tests: these see `statkit_core` exactly as the binding crate and
//! any other consumer does, through its public API only.

use statkit_core::{StatError, parse_values, summarize, zscores};

#[test]
fn text_to_summary_end_to_end() {
    let values = parse_values("2, 4, 4, 4, 5, 5, 7, 9").unwrap();
    let summary = summarize(&values).unwrap();

    assert_eq!(summary.count, 8);
    assert_eq!(summary.mean, 5.0);
    assert_eq!(summary.median, 4.5);
    assert_eq!(summary.min, 2.0);
    assert_eq!(summary.max, 9.0);
    // Sample variance: 32 / 7.
    assert!((summary.stddev - (32.0_f64 / 7.0).sqrt()).abs() < 1e-12);
}

#[test]
fn text_to_zscores_end_to_end() {
    let values = parse_values("1 2 3").unwrap();
    let scores = zscores(&values).unwrap();

    assert_eq!(scores.len(), 3);
    assert!((scores.iter().sum::<f64>()).abs() < 1e-12);
}

#[test]
fn errors_carry_a_readable_message() {
    let err = parse_values("1 oops").unwrap_err();
    assert_eq!(err.to_string(), "not a number: 'oops'");

    assert_eq!(summarize(&[]).unwrap_err().to_string(), "no values given");
    assert_eq!(
        zscores(&[1.0, 1.0]).unwrap_err(),
        StatError::Constant(1.0),
        "a flat sample has no spread to divide by"
    );
}
