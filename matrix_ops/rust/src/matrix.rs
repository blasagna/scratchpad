//! A dense 2D matrix of `f64` and the four operations the CLI exposes.

use std::fmt;
use std::ops::{Index, IndexMut};

/// What went wrong. Every variant carries the numbers that explain it, so the
/// CLI can print a useful message without re-deriving them.
///
/// There is no out-of-memory variant, unlike the C and C++ ports: Rust aborts
/// on allocation failure, and threading `try_reserve` through every growth
/// point to convert that into a clean exit is not worth what it costs to read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// `add`/`sub` shapes differ, or `mul`'s inner dimensions do.
    DimMismatch {
        a: (usize, usize),
        b: (usize, usize),
    },
    /// A token was not a finite number.
    BadNumber(String),
    /// Input rows have differing column counts.
    Ragged { expected: usize, found: usize },
    /// The input held no values.
    Empty,
    /// A dimension was zero. Every operation here is defined in terms of
    /// elements, so a matrix with none has no useful answer to give.
    ZeroDimension,
    /// The requested dimensions do not fit the value count.
    BadShape {
        count: usize,
        rows: Option<usize>,
        cols: Option<usize>,
    },
    /// `rows * cols` does not fit in a `usize`.
    Overflow { rows: usize, cols: usize },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::DimMismatch { a, b } => write!(
                f,
                "incompatible matrix dimensions: {}x{} and {}x{}",
                a.0, a.1, b.0, b.1
            ),
            Error::BadNumber(token) => {
                write!(f, "value is not a finite number: '{token}'")
            }
            Error::Ragged { expected, found } => write!(
                f,
                "rows do not all have the same number of values: \
                 expected {expected}, found {found}"
            ),
            Error::Empty => write!(f, "no values given"),
            Error::ZeroDimension => write!(f, "a matrix dimension may not be zero"),
            Error::BadShape { count, rows, cols } => {
                write!(f, "requested dimensions do not fit the {count} values: ")?;
                match (rows, cols) {
                    (Some(r), Some(c)) => write!(f, "{r}x{c}"),
                    (Some(r), None) => write!(f, "{r} rows"),
                    (None, Some(c)) => write!(f, "{c} columns"),
                    (None, None) => write!(f, "no dimensions given"),
                }
            }
            Error::Overflow { rows, cols } => {
                write!(f, "matrix is too large: {rows}x{cols}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// A dense 2D matrix of `f64`, stored row-major: element `(r, c)` lives at
/// `data[r * cols + c]`.
///
/// A `Matrix` is always non-empty — the constructors reject a zero dimension —
/// so nothing downstream has to reason about an empty one.
#[derive(Debug, Clone, PartialEq)]
pub struct Matrix {
    rows: usize,
    cols: usize,
    data: Vec<f64>,
}

impl Matrix {
    /// Allocates a `rows x cols` matrix with every element zero.
    pub fn zeros(rows: usize, cols: usize) -> Result<Matrix, Error> {
        let count = Matrix::element_count(rows, cols)?;
        Ok(Matrix {
            rows,
            cols,
            data: vec![0.0; count],
        })
    }

    /// Wraps values already in row-major order. `data.len()` must be exactly
    /// `rows * cols`.
    pub fn from_vec(rows: usize, cols: usize, data: Vec<f64>) -> Result<Matrix, Error> {
        let count = Matrix::element_count(rows, cols)?;
        if data.len() != count {
            return Err(Error::BadShape {
                count: data.len(),
                rows: Some(rows),
                cols: Some(cols),
            });
        }
        Ok(Matrix { rows, cols, data })
    }

    /// Checked `rows * cols`, rejecting a zero dimension first.
    ///
    /// The multiplication is checked rather than tested afterwards because a
    /// wrapped product is a plausible-looking small number that the allocator
    /// would happily satisfy.
    fn element_count(rows: usize, cols: usize) -> Result<usize, Error> {
        if rows == 0 || cols == 0 {
            return Err(Error::ZeroDimension);
        }
        rows.checked_mul(cols).ok_or(Error::Overflow { rows, cols })
    }

    pub fn rows(&self) -> usize {
        self.rows
    }

    pub fn cols(&self) -> usize {
        self.cols
    }

    pub fn shape(&self) -> (usize, usize) {
        (self.rows, self.cols)
    }

    /// The elements in row-major order.
    pub fn as_slice(&self) -> &[f64] {
        &self.data
    }

    /// Element-wise sum. Both matrices must have the same shape.
    pub fn add(&self, other: &Matrix) -> Result<Matrix, Error> {
        self.add_scaled(other, 1.0)
    }

    /// Element-wise difference. Both matrices must have the same shape.
    pub fn sub(&self, other: &Matrix) -> Result<Matrix, Error> {
        self.add_scaled(other, -1.0)
    }

    /// Shared body of `add` and `sub`: both walk the elements in step and
    /// differ only in the sign applied to `other`, which keeps the inner loop a
    /// plain multiply-add.
    fn add_scaled(&self, other: &Matrix, sign: f64) -> Result<Matrix, Error> {
        if self.shape() != other.shape() {
            return Err(Error::DimMismatch {
                a: self.shape(),
                b: other.shape(),
            });
        }
        let data = self
            .data
            .iter()
            .zip(&other.data)
            .map(|(a, b)| a + sign * b)
            .collect();
        Ok(Matrix {
            rows: self.rows,
            cols: self.cols,
            data,
        })
    }

    /// Matrix product. `self` is `m x n`, `other` must be `n x p`, and the
    /// result is `m x p` — only the shared inner dimension has to agree. Not
    /// commutative, so the argument order is the CLI's operand order.
    ///
    /// This is the naive triple loop on purpose. It is what
    /// `matrix_ops/bench/rust` measures against faer and nalgebra, and it is
    /// deliberately the same `i`/`j`/`k` order as the C and C++ ports so the
    /// two benchmark tables compare the same algorithm.
    pub fn mul(&self, other: &Matrix) -> Result<Matrix, Error> {
        if self.cols != other.rows {
            return Err(Error::DimMismatch {
                a: self.shape(),
                b: other.shape(),
            });
        }

        let mut result = Matrix::zeros(self.rows, other.cols)?;
        for i in 0..self.rows {
            for j in 0..other.cols {
                let mut sum = 0.0;
                for k in 0..self.cols {
                    sum += self[(i, k)] * other[(k, j)];
                }
                result[(i, j)] = sum;
            }
        }
        Ok(result)
    }

    /// Multiplies every element by `scalar`. Cannot fail — the shape is
    /// unchanged and the storage is already the right size.
    pub fn scale(&self, scalar: f64) -> Matrix {
        Matrix {
            rows: self.rows,
            cols: self.cols,
            data: self.data.iter().map(|v| v * scalar).collect(),
        }
    }
}

/// Element access by `(row, col)`. The indices are a caller precondition, and
/// out-of-range ones panic like any other slice index.
impl Index<(usize, usize)> for Matrix {
    type Output = f64;

    fn index(&self, (row, col): (usize, usize)) -> &f64 {
        &self.data[row * self.cols + col]
    }
}

impl IndexMut<(usize, usize)> for Matrix {
    fn index_mut(&mut self, (row, col): (usize, usize)) -> &mut f64 {
        &mut self.data[row * self.cols + col]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a matrix from a row-major literal, panicking on a bad shape —
    /// every use here is a fixture with dimensions known at the call site.
    fn m(rows: usize, cols: usize, values: &[f64]) -> Matrix {
        Matrix::from_vec(rows, cols, values.to_vec()).expect("fixture shape")
    }

    #[test]
    fn zeros_fills_and_sets_the_shape() {
        let a = Matrix::zeros(2, 3).unwrap();
        assert_eq!(a.shape(), (2, 3));
        assert_eq!(a.as_slice(), [0.0; 6]);
    }

    #[test]
    fn rejects_a_zero_dimension() {
        assert_eq!(Matrix::zeros(0, 3), Err(Error::ZeroDimension));
        assert_eq!(Matrix::zeros(3, 0), Err(Error::ZeroDimension));
    }

    #[test]
    fn rejects_an_overflowing_product() {
        let huge = usize::MAX / 2 + 1;
        assert_eq!(
            Matrix::zeros(huge, 4),
            Err(Error::Overflow {
                rows: huge,
                cols: 4
            })
        );
    }

    #[test]
    fn from_vec_requires_an_exact_value_count() {
        assert!(Matrix::from_vec(2, 3, vec![1.0; 6]).is_ok());
        assert!(Matrix::from_vec(2, 3, vec![1.0; 5]).is_err());
        assert!(Matrix::from_vec(2, 3, vec![1.0; 7]).is_err());
    }

    #[test]
    fn access_is_row_major() {
        let a = m(2, 3, &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
        assert_eq!(a[(0, 0)], 1.0);
        assert_eq!(a[(0, 2)], 3.0);
        assert_eq!(a[(1, 0)], 4.0);
        assert_eq!(a[(1, 2)], 6.0);
    }

    #[test]
    fn sums_element_wise() {
        let a = m(2, 2, &[1.0, 2.0, 3.0, 4.0]);
        let b = m(2, 2, &[10.0, 20.0, 30.0, 40.0]);
        assert_eq!(a.add(&b).unwrap().as_slice(), [11.0, 22.0, 33.0, 44.0]);
    }

    #[test]
    fn subtracts_element_wise_and_is_not_commutative() {
        let a = m(1, 2, &[1.0, 2.0]);
        let b = m(1, 2, &[10.0, 20.0]);
        assert_eq!(a.sub(&b).unwrap().as_slice(), [-9.0, -18.0]);
        assert_eq!(b.sub(&a).unwrap().as_slice(), [9.0, 18.0]);
    }

    #[test]
    fn add_then_sub_round_trips() {
        let a = m(2, 2, &[1.5, -2.5, 0.0, 4.25]);
        let b = m(2, 2, &[0.5, 2.5, -1.0, 0.75]);
        assert_eq!(a.add(&b).unwrap().sub(&b).unwrap(), a);
    }

    #[test]
    fn add_rejects_mismatched_shapes() {
        // Same element count, different shape: the count is not the rule.
        let a = m(2, 3, &[1.0; 6]);
        let b = m(3, 2, &[1.0; 6]);
        assert_eq!(
            a.add(&b),
            Err(Error::DimMismatch {
                a: (2, 3),
                b: (3, 2)
            })
        );
        assert!(a.sub(&b).is_err());
    }

    #[test]
    fn multiplies_a_non_square_pair() {
        let a = m(2, 3, &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
        let b = m(3, 2, &[7.0, 8.0, 9.0, 10.0, 11.0, 12.0]);
        let product = a.mul(&b).unwrap();
        assert_eq!(product.shape(), (2, 2));
        assert_eq!(product.as_slice(), [58.0, 64.0, 139.0, 154.0]);
    }

    #[test]
    fn identity_leaves_a_matrix_unchanged() {
        let a = m(2, 2, &[1.0, 2.0, 3.0, 4.0]);
        let i = m(2, 2, &[1.0, 0.0, 0.0, 1.0]);
        assert_eq!(a.mul(&i).unwrap(), a);
        assert_eq!(i.mul(&a).unwrap(), a);
    }

    #[test]
    fn multiplication_is_not_commutative() {
        let a = m(2, 2, &[1.0, 2.0, 3.0, 4.0]);
        let b = m(2, 2, &[0.0, 1.0, 0.0, 0.0]);
        assert_ne!(a.mul(&b).unwrap(), b.mul(&a).unwrap());
    }

    #[test]
    fn a_row_times_a_column_is_one_by_one() {
        let row = m(1, 3, &[1.0, 2.0, 3.0]);
        let col = m(3, 1, &[4.0, 5.0, 6.0]);
        let product = row.mul(&col).unwrap();
        assert_eq!(product.shape(), (1, 1));
        assert_eq!(product.as_slice(), [32.0]);
    }

    #[test]
    fn a_column_times_a_row_is_the_outer_product() {
        let col = m(3, 1, &[1.0, 2.0, 3.0]);
        let row = m(1, 2, &[10.0, 20.0]);
        let product = col.mul(&row).unwrap();
        assert_eq!(product.shape(), (3, 2));
        assert_eq!(product.as_slice(), [10.0, 20.0, 20.0, 40.0, 30.0, 60.0]);
    }

    #[test]
    fn mul_rejects_a_mismatched_inner_dimension() {
        let a = m(2, 3, &[1.0; 6]);
        let b = m(2, 3, &[1.0; 6]);
        assert!(a.mul(&b).is_err());
    }

    #[test]
    fn multiplication_is_associative() {
        let a = m(2, 3, &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
        let b = m(3, 2, &[7.0, 8.0, 9.0, 10.0, 11.0, 12.0]);
        let c = m(2, 2, &[1.0, 2.0, 3.0, 4.0]);
        let left = a.mul(&b).unwrap().mul(&c).unwrap();
        let right = a.mul(&b.mul(&c).unwrap()).unwrap();
        assert_eq!(left, right);
    }

    #[test]
    fn scale_multiplies_every_element_and_keeps_the_shape() {
        let a = m(2, 3, &[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);
        let scaled = a.scale(2.5);
        assert_eq!(scaled.shape(), (2, 3));
        assert_eq!(scaled.as_slice(), [2.5, 5.0, 7.5, 10.0, 12.5, 15.0]);
    }

    #[test]
    fn scale_by_zero_and_by_negative_one() {
        let a = m(1, 3, &[1.0, -2.0, 3.0]);
        assert_eq!(a.scale(-1.0).as_slice(), [-1.0, 2.0, -3.0]);
        // Zero times a negative is a negative zero; the formatter, not the
        // arithmetic, is what normalizes that away.
        assert!(a.scale(0.0).as_slice().iter().all(|v| *v == 0.0));
    }
}
