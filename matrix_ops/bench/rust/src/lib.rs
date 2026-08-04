//! Shared fixtures for the `matrix_ops` Rust benchmark.
//!
//! The data, the sizes, and the operations are the same ones
//! `matrix_ops/bench/compare.cpp` uses against Eigen and xtensor, so the two
//! tables in `matrix_ops/README.md` describe the same work.
//!
//! Both libraries are constructed and compared through `(i, j)` indexing, never
//! through their backing storage — see [`to_faer`] for why that matters.

use faer::{Mat, Scale};
use matrix_ops::Matrix;
use nalgebra::DMatrix;

/// Square sizes benchmarked. 64 fits in L1 and shows call overhead; 1024 is
/// large enough that a real GEMM's cache blocking is the whole story.
pub const SIZES: [usize; 3] = [64, 256, 1024];

/// Tolerance for the cross-library agreement check. The implementations
/// accumulate in different orders, so bitwise equality is not expected; this is
/// relative to the magnitude of the entries.
pub const TOLERANCE: f64 = 1e-9;

/// The multiplier used by the `scale` benchmark.
pub const SCALAR: f64 = 2.5;

/// One size's worth of operands, in all three representations.
pub struct Fixture {
    pub n: usize,
    pub ours: (Matrix, Matrix),
    pub faer: (Mat<f64>, Mat<f64>),
    pub nalgebra: (DMatrix<f64>, DMatrix<f64>),
}

impl Fixture {
    pub fn new(n: usize) -> Fixture {
        let a = Matrix::from_vec(n, n, values(n * n, 1)).expect("fixture shape");
        let b = Matrix::from_vec(n, n, values(n * n, 2)).expect("fixture shape");
        Fixture {
            n,
            faer: (to_faer(&a), to_faer(&b)),
            nalgebra: (to_nalgebra(&a), to_nalgebra(&b)),
            ours: (a, b),
        }
    }
}

/// Deterministic pseudo-random values in `[-1, 1)`.
///
/// A small xorshift rather than a dependency on `rand`: the benchmark needs
/// values that are identical on every run and not degenerate, and reproducing
/// `compare.cpp`'s `mt19937` stream bit for bit would not make the timings any
/// more comparable than this does.
fn values(count: usize, seed: u64) -> Vec<f64> {
    let mut state = seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1;
    (0..count)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            // The top 53 bits, the mantissa width of an f64.
            let unit = (state >> 11) as f64 / (1u64 << 53) as f64;
            unit * 2.0 - 1.0
        })
        .collect()
}

/// Converts to faer's `Mat`, which is **column-major** where ours is row-major.
///
/// Built element by element rather than by handing the row-major buffer to a
/// column-major constructor. That shortcut silently produces the transpose, and
/// the transpose still agrees elementwise on add, sub, and scale — only `mul`
/// breaks, because `Aᵀ·Bᵀ` is not `(A·B)ᵀ`. A layout bug here would pass three
/// quarters of the correctness gate.
pub fn to_faer(m: &Matrix) -> Mat<f64> {
    Mat::from_fn(m.rows(), m.cols(), |i, j| m[(i, j)])
}

/// Converts to nalgebra's `DMatrix`, also column-major. `from_row_slice` does
/// the transpose for us, which is why passing the row-major slice is correct
/// here and would not be for faer.
pub fn to_nalgebra(m: &Matrix) -> DMatrix<f64> {
    DMatrix::from_row_slice(m.rows(), m.cols(), m.as_slice())
}

/// Compares a library's result against ours elementwise, by `(i, j)`.
///
/// Returns a description of the first disagreement so a caller can report it
/// and suppress the timings, the way `compare.cpp` does.
pub fn agrees(
    library: &str,
    op: &str,
    ours: &Matrix,
    theirs: impl Fn(usize, usize) -> f64,
) -> Result<(), String> {
    for i in 0..ours.rows() {
        for j in 0..ours.cols() {
            let a = ours[(i, j)];
            let b = theirs(i, j);
            let scale = 1.0_f64.max(a.abs()).max(b.abs());
            if (a - b).abs() > TOLERANCE * scale {
                return Err(format!(
                    "{library} {op} disagrees at ({i}, {j}): ours={a:.17e} theirs={b:.17e}"
                ));
            }
        }
    }
    Ok(())
}

/// Runs every operation at one size against both libraries.
///
/// This is the gate `compare.cpp` runs before it prints any timing. criterion
/// has no equivalent hook, so it lives in `tests/agreement.rs`, and `run.sh`
/// runs the tests before the benchmarks.
pub fn check(fixture: &Fixture) -> Result<(), String> {
    let (a, b) = &fixture.ours;
    let (fa, fb) = &fixture.faer;
    let (na, nb) = &fixture.nalgebra;

    let sum = a.add(b).map_err(|e| e.to_string())?;
    let f = fa + fb;
    let n = na + nb;
    agrees("faer", "add", &sum, |i, j| f[(i, j)])?;
    agrees("nalgebra", "add", &sum, |i, j| n[(i, j)])?;

    let diff = a.sub(b).map_err(|e| e.to_string())?;
    let f = fa - fb;
    let n = na - nb;
    agrees("faer", "sub", &diff, |i, j| f[(i, j)])?;
    agrees("nalgebra", "sub", &diff, |i, j| n[(i, j)])?;

    let product = a.mul(b).map_err(|e| e.to_string())?;
    let f = fa * fb;
    let n = na * nb;
    agrees("faer", "mul", &product, |i, j| f[(i, j)])?;
    agrees("nalgebra", "mul", &product, |i, j| n[(i, j)])?;

    let scaled = a.scale(SCALAR);
    let f = fa * Scale(SCALAR);
    let n = na * SCALAR;
    agrees("faer", "scale", &scaled, |i, j| f[(i, j)])?;
    agrees("nalgebra", "scale", &scaled, |i, j| n[(i, j)])?;

    Ok(())
}
