# rust_python_bindings

A worked example of exposing a Rust library to Python with PyO3 + maturin. The
demo library is `statkit` (descriptive statistics). Four pieces: `core/` (crate
`statkit_core`, pure Rust lib + CLI), `bindings/` (crate `statkit_py`, the PyO3
cdylib), `python/statkit/` (the Python package), `tests/` (Python tests). The full
walkthrough is in [`README.md`](README.md).

## Commands

```sh
cargo test -p statkit_core                                  # the logic lives here
printf '1 2 3\n' | cargo run -q -p statkit_core --bin statkit

cd rust_python_bindings
pixi run build          # maturin develop: compile the cdylib + editable install
pixi run test           # unittest + doctests; depends on build
printf '1 2 3\n' | pixi run cli --zscores
```

## Invariants

- **`core/` must never depend on pyo3.** Logic goes there and is tested with
  `cargo test`; `bindings/` only converts values and errors, and has no tests of
  its own.
- **The two CLIs print byte-identical output on success.** `format_summary`/
  `format_zscores` exist in both `core/src/main.rs` and `python/statkit/cli.py`;
  change one and you change the other. `StatError` messages match too, since both
  come from Rust; OS-error wording differs (each runtime words its own) and that
  is fine. Exit codes are `0` / `1` runtime error / `2` usage error, as elsewhere
  in this repo.
- **Three names must agree**: `[lib] name` in `bindings/Cargo.toml`, the
  `#[pymodule]` function name, and the last component of `module-name` in
  `pyproject.toml`. All are `_core`.
- **`_core.pyi` is hand-written** and has to be updated whenever the binding
  signatures change — nothing checks it against the Rust.
- **A Rust edit is invisible to Python until `pixi run build` reruns.** `test` and
  `cli` declare `depends-on = ["build"]` for that reason; don't remove it.

## Gotcha: where `extension-module` lives

The pyo3 `extension-module` feature is enabled in `bindings/Cargo.toml`, not in
`[tool.maturin] features`. Repo-wide `cargo test` builds every workspace member,
and without that feature the cdylib tries to link libpython and needs a system
development package. The cost is that `bindings/` can't host Rust tests — fine,
since it has no logic to test. See README "Gotchas" for the full reasoning.
