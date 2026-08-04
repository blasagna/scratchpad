//! The correctness gate for the benchmark.
//!
//! `bench/compare.cpp` verifies every operation elementwise before it prints a
//! single timing, on the principle that a benchmark silently comparing three
//! different answers is worse than no benchmark. criterion has no equivalent
//! hook, so the check lives here and `run.sh` runs it first.

use matrix_ops_bench::{Fixture, check};

/// Sizes checked, which are deliberately *not* `SIZES`.
///
/// What this test catches is a layout or algorithm mistake, and every such bug
/// is visible at small sizes — 7 is the useful one, being square but a multiple
/// of nothing, so it exercises any blocked kernel's ragged edge. Running the
/// naive 1024x1024 multiply here would add minutes to `cargo test` and catch
/// nothing that 256 does not.
const CHECKED_SIZES: [usize; 3] = [7, 64, 256];

#[test]
fn ours_faer_and_nalgebra_agree_on_every_operation() {
    for n in CHECKED_SIZES {
        let fixture = Fixture::new(n);
        if let Err(disagreement) = check(&fixture) {
            panic!("at {n}x{n}: {disagreement}");
        }
    }
}

/// Pins the trap that makes the rest of this file worth having.
///
/// faer is column-major and our `Matrix` is row-major. Handing the row-major
/// buffer to a column-major constructor yields the transpose — and the
/// transpose agrees elementwise on add, sub, and scale, so only `mul` would
/// catch it. This asserts the conversion is the real thing and not its
/// transpose, on a matrix that is not symmetric.
#[test]
fn the_conversions_preserve_orientation_rather_than_transposing() {
    let m = matrix_ops::Matrix::from_vec(2, 3, vec![1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).unwrap();
    let f = matrix_ops_bench::to_faer(&m);
    let n = matrix_ops_bench::to_nalgebra(&m);

    assert_eq!((f.nrows(), f.ncols()), (2, 3));
    assert_eq!(n.shape(), (2, 3));
    for i in 0..2 {
        for j in 0..3 {
            assert_eq!(f[(i, j)], m[(i, j)], "faer at ({i}, {j})");
            assert_eq!(n[(i, j)], m[(i, j)], "nalgebra at ({i}, {j})");
        }
    }
}
