//! PyO3 glue: exposes `statkit_core` to CPython as the extension module
//! `statkit._core`.
//!
//! This crate deliberately contains **no logic**. Every function here does three
//! things and nothing else: convert Python values into Rust values, call into
//! `statkit_core`, convert the result (or the error) back. That is the whole
//! point of the core/bindings split -- if you find yourself computing something
//! in this file, it belongs in the core crate where it can be tested with
//! `cargo test`.

use pyo3::exceptions::PyException;
use pyo3::prelude::*;
use pyo3::{create_exception, wrap_pyfunction};

use statkit_core as core;

// ---------------------------------------------------------------------------
// Errors: one Rust enum -> one Python exception type
// ---------------------------------------------------------------------------

// The first argument is stringified into the exception's `__module__`, so it has
// to be the *fully qualified* module path, not the bare crate/module name. Get it
// wrong and tracebacks name a module that does not exist and pickle -- and with
// it multiprocessing -- cannot round-trip the exception.
create_exception!(
    statkit._core,
    StatError,
    PyException,
    "Raised when a statistic cannot be computed from the given input."
);

/// Converts a core error into the Python exception.
///
/// This is a plain function rather than `impl From<core::StatError> for PyErr`
/// because the orphan rule forbids it: both `StatError` and `PyErr` are foreign
/// to this crate, so the blanket `?` conversion is not available. `.map_err(to_pyerr)?`
/// costs one call and keeps the error type owned by the core crate, which is the
/// right trade -- the alternative is a newtype wrapper that buys nothing.
///
/// Note what is preserved: the `Display` message. Python callers see
/// `StatError: not a number: 'two'`, not a stringified Rust enum.
fn to_pyerr(err: core::StatError) -> PyErr {
    StatError::new_err(err.to_string())
}

// ---------------------------------------------------------------------------
// A Rust struct as a Python class
// ---------------------------------------------------------------------------

/// The `Summary` class as Python sees it.
///
/// `frozen` makes it immutable from Python, which is both honest (the fields are
/// a computed snapshot) and free of runtime borrow checking. `module = ...` makes
/// `repr(type(s))` report the real home of the class instead of `builtins`.
#[pyclass(name = "Summary", frozen, module = "statkit._core")]
pub struct PySummary {
    inner: core::Summary,
}

#[pymethods]
impl PySummary {
    #[getter]
    fn count(&self) -> usize {
        self.inner.count
    }

    #[getter]
    fn mean(&self) -> f64 {
        self.inner.mean
    }

    #[getter]
    fn median(&self) -> f64 {
        self.inner.median
    }

    #[getter]
    fn min(&self) -> f64 {
        self.inner.min
    }

    #[getter]
    fn max(&self) -> f64 {
        self.inner.max
    }

    #[getter]
    fn stddev(&self) -> f64 {
        self.inner.stddev
    }

    /// `{:?}` rather than `{}` for the floats: Rust's `Debug` for `f64` prints
    /// `4.0`, matching Python's `repr`, whereas `Display` would print `4`.
    fn __repr__(&self) -> String {
        let core::Summary {
            count,
            mean,
            median,
            min,
            max,
            stddev,
        } = self.inner;
        format!(
            "Summary(count={count}, mean={mean:?}, median={median:?}, \
             min={min:?}, max={max:?}, stddev={stddev:?})"
        )
    }
}

impl From<core::Summary> for PySummary {
    fn from(inner: core::Summary) -> Self {
        // Allowed where `to_pyerr` was not: `PySummary` is local to this crate.
        PySummary { inner }
    }
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/// Summarize a sequence of numbers.
///
/// The `Vec<f64>` parameter accepts any Python sequence of floats and copies it
/// into Rust memory. Because that copy is owned by Rust, the call into the core
/// crate can run with the GIL released (`Python::detach`) -- other Python threads
/// keep running while it works. For a five-element list this is pure overhead;
/// it is here to show where the seam goes for a computation big enough to matter.
#[pyfunction]
fn summarize(py: Python<'_>, values: Vec<f64>) -> PyResult<PySummary> {
    let summary = py.detach(|| core::summarize(&values)).map_err(to_pyerr)?;
    Ok(summary.into())
}

/// Return each value's distance from the mean, in standard deviations.
///
/// `Vec<f64>` comes back out as a Python `list[float]`.
#[pyfunction]
fn zscores(py: Python<'_>, values: Vec<f64>) -> PyResult<Vec<f64>> {
    py.detach(|| core::zscores(&values)).map_err(to_pyerr)
}

/// Parse whitespace- and comma-separated numbers out of a string.
///
/// `&str` borrows CPython's own string buffer -- no copy, but also no
/// `Python::detach` around the call, since the borrow is only valid while this
/// thread is attached to the interpreter. Take a `String` instead if you need to
/// release the GIL.
#[pyfunction]
fn parse_values(text: &str) -> PyResult<Vec<f64>> {
    core::parse_values(text).map_err(to_pyerr)
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

/// Compiled statistics kernels backing the `statkit` package.
///
/// Import `statkit` instead; this module is private and its signatures are
/// documented in `_core.pyi`.
#[pymodule]
fn _core(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(summarize, m)?)?;
    m.add_function(wrap_pyfunction!(zscores, m)?)?;
    m.add_function(wrap_pyfunction!(parse_values, m)?)?;
    m.add_class::<PySummary>()?;
    // `create_exception!` defines the type; it still has to be attached to the
    // module for `statkit._core.StatError` to exist and for `except` to work.
    m.add("StatError", m.py().get_type::<StatError>())?;
    Ok(())
}
