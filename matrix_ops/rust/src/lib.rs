//! A CLI that adds, subtracts, multiplies, and scales 2D matrices of real
//! numbers — the Rust port of `matrix_ops`.
//!
//! The shape rules and the output format match the C and C++ ports; the
//! command-line surface deliberately does not. See `README.md` in this
//! directory for what that trade buys and what it costs.

pub mod matrix;
pub mod matrix_io;

pub use matrix::{Error, Matrix};
pub use matrix_io::{DEFAULT_PRECISION, MAX_PRECISION, parse, render};
