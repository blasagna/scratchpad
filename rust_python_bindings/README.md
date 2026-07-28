# rust_python_bindings

How to give a Rust library a Python API, end to end, using [PyO3] for the glue and
[maturin] for the build.

The demo library is **statkit**: descriptive statistics (mean, median, min, max,
standard deviation, z-scores) over a list of numbers. It is small on purpose — the
interesting part is not the arithmetic, it's the seam. The same core is reachable
three ways:

```sh
$ printf '1 2 3 4 10\n' | cargo run -q -p statkit_core --bin statkit   # Rust CLI
$ printf '1 2 3 4 10\n' | pixi run cli                                 # Python CLI
$ pixi run python -c "import statkit; print(statkit.summarize([1,2,3,4,10]))"
```

The first two print byte-identical output. That equivalence is the whole claim
being demonstrated.

## Layout

```
rust_python_bindings/
├── core/                     crate statkit_core — pure Rust, no PyO3 anywhere
│   ├── src/lib.rs              the library + its unit tests
│   ├── src/main.rs             the Rust CLI (clap)
│   └── tests/summarize.rs      integration tests against the public API
├── bindings/                 crate statkit_py — the PyO3 glue, nothing else
│   └── src/lib.rs              #[pyfunction] / #[pyclass] / #[pymodule]
├── python/statkit/           the Python package (maturin's `python-source`)
│   ├── __init__.py             public API, wrapping the private extension
│   ├── _core.pyi               type stubs for the compiled module
│   ├── py.typed                marks the package as typed
│   ├── cli.py                  the Python CLI (argparse)
│   └── __main__.py             `python -m statkit`
├── tests/test_statkit.py     Python tests (unittest)
├── pyproject.toml            maturin build backend configuration
└── pixi.toml                 the Python environment and its tasks
```

## How it works

Four layers, each with one job:

```
  statkit.summarize([1, 2, 3])          python/statkit/__init__.py
        │  coerce any iterable to list[float]
        ▼
  _core.summarize(values)                the compiled extension module,
        │                                python/statkit/_core.abi3.so
        ▼
  #[pyfunction] fn summarize(...)        bindings/src/lib.rs
        │  Vec<f64> in, PySummary or PyErr out
        ▼
  statkit_core::summarize(&values)       core/src/lib.rs — plain Rust
```

At build time, `maturin develop` compiles `bindings/` into a `cdylib`, renames it
to `python/statkit/_core.abi3.so`, and installs the `statkit` package into the
active environment in editable mode. Python's import machinery then treats that
`.so` exactly like a `.py` module: `from . import _core` loads it, and CPython
calls the init function PyO3 generated from `#[pymodule] fn _core`.

Two details that make the layering work:

- **`crate-type = ["cdylib"]`** — a normal `rlib` is a Rust-only artifact. A
  `cdylib` is a plain shared object with C linkage, which is what `dlopen` wants.
- **`abi3-py311`** — builds against CPython's stable ABI, so one compiled file
  works on 3.11 and every later version, instead of one per interpreter version.
  The wheel maturin produces is named `...-cp311-abi3-...` for that reason.

## The techniques, one at a time

Excerpts below are trimmed; read `bindings/src/lib.rs` for the commented originals.

**A function.** Argument and return types are ordinary Rust types; PyO3 generates
the conversions from the signature.

```rust
#[pyfunction]
fn zscores(py: Python<'_>, values: Vec<f64>) -> PyResult<Vec<f64>> {
    py.detach(|| core::zscores(&values)).map_err(to_pyerr)
}
```

`Vec<f64>` arrives as a copy owned by Rust, so `Python::detach` can release the
GIL for the duration of the call and let other Python threads run. That only holds
because nothing borrowed from the interpreter is held across the boundary — the
sibling `parse_values` takes a `&str`, which borrows CPython's own buffer, and so
does *not* detach.

**A struct as a class.** Wrap the core type and expose `#[getter]`s:

```rust
#[pyclass(name = "Summary", frozen, module = "statkit._core")]
pub struct PySummary { inner: core::Summary }

#[pymethods]
impl PySummary {
    #[getter]
    fn mean(&self) -> f64 { self.inner.mean }
    fn __repr__(&self) -> String { /* ... */ }
}
```

`frozen` makes instances immutable from Python — honest for a computed snapshot,
and it skips PyO3's runtime borrow checking. Note `{:?}` rather than `{}` when
formatting the floats in `__repr__`: Rust's `Debug` prints `4.0`, matching
Python's `repr`, where `Display` would print `4`.

**An error as an exception.**

```rust
create_exception!(_core, StatError, PyException, "…");

fn to_pyerr(err: core::StatError) -> PyErr {
    StatError::new_err(err.to_string())
}
```

Python callers get `StatError: not a number: 'two'` and can `except statkit.StatError`.
This is a free function rather than `impl From<core::StatError> for PyErr` because
the orphan rule forbids that impl: both types are foreign to the bindings crate.
`.map_err(to_pyerr)?` is the cost of the split, and it is small.

**The module.** `create_exception!` defines the exception type but does not attach
it; every name has to be added explicitly:

```rust
#[pymodule]
fn _core(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(summarize, m)?)?;
    m.add_class::<PySummary>()?;
    m.add("StatError", m.py().get_type::<StatError>())?;
    Ok(())
}
```

The function name must match `[lib] name` in `bindings/Cargo.toml` and the last
component of `module-name` in `pyproject.toml`. All three say `_core`.

## Best practices this demonstrates

- **Keep the core crate PyO3-free.** `statkit_core` has no idea Python exists, so
  it stays testable with `cargo test`, usable from other Rust code, and portable.
  The glue crate is under 170 lines, most of them comments and none of them
  arithmetic.
- **Never let a panic cross the boundary.** A Rust panic in an extension unwinds
  into C; PyO3 catches it and raises `pyo3_runtime.PanicException`, but that is a
  bug report, not an API. Fallible operations return `Result`, and the binding
  turns the error into an exception.
- **Errors become exceptions, not sentinel values.** `None`/`-1`/`nan` returns are
  a Rust habit that reads as a bug in Python.
- **Make the extension private and wrap it.** `_core` is an implementation detail;
  `statkit/__init__.py` is the API. That indirection is where Python-native
  conveniences live — accepting any iterable, `as_dict`, docstrings with doctests —
  none of which need to be written in Rust.
- **Ship `abi3`.** One artifact for every supported interpreter beats a build
  matrix.
- **Ship stubs.** A `.so` exposes no signatures to editors or type checkers.
  `_core.pyi` plus a `py.typed` marker fixes that, at the cost of keeping them in
  step with the Rust by hand.
- **Test the logic in Rust, the seam in Python.** `cargo test -p statkit_core`
  owns the statistics. `tests/test_statkit.py` deliberately does not re-check the
  arithmetic; it checks that types survive the crossing, that each error variant
  arrives as a catchable exception, and that the CLI is wired up.

## Build, test, run

Rust, from anywhere in the repo:

```sh
cargo test -p statkit_core                             # 16 tests
cargo run  -q -p statkit_core --bin statkit -- --help
printf '1 2 3 4 10\n' | cargo run -q -p statkit_core --bin statkit
printf '1 2 3 4 10\n' | cargo run -q -p statkit_core --bin statkit -- --zscores
```

Python, from this directory (`build` runs first automatically):

```sh
pixi run build                       # maturin develop: compile + editable install
pixi run test                        # unittest, including the doctests
printf '1 2 3 4 10\n' | pixi run cli
printf '1 2 3 4 10\n' | pixi run cli --zscores
pixi run python -c "import statkit; print(statkit.as_dict(statkit.summarize([1,2,3])))"
```

Both CLIs exit `0` on success, `1` on a bad input, and `2` on a usage error.

For a distributable wheel rather than an editable install, `pixi run maturin build
--release` writes one to the workspace's shared `target/wheels/` at the repo root.

## Gotchas

- **Rust edits need a rebuild.** The editable install links the *package
  directory*, not the Rust source. Python keeps loading the old `.so` until
  `pixi run build` runs again — which is why `test` and `cli` depend on `build`.
- **`extension-module` is set in `bindings/Cargo.toml`**, not in
  `[tool.maturin] features`. The other placement keeps `cargo test` able to link
  that crate, but then a plain `cargo build` needs a libpython development
  package — and `cargo test` at the repo root builds every workspace member. On
  Linux a `cdylib` with `extension-module` links fine with undefined symbols, so
  this placement keeps the repo-wide build working with no system dependency. The
  trade is that `bindings/` cannot host Rust tests; it has none by design.
- **The compiled module is not in git.** `*.so` is ignored repo-wide, so a fresh
  clone must run `pixi run build` before `import statkit` will work.
- **`pixi run cli --zscores` passes the flag through**; `pixi run cli -- --zscores`
  works too. The Rust CLI needs the `--` (`cargo run … -- --zscores`) because the
  first set of flags belongs to cargo.

[PyO3]: https://pyo3.rs
[maturin]: https://www.maturin.rs
