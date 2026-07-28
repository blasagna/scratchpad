# cpp_rust_bindings

A worked example of exposing a C++ library to Rust with [cxx](https://cxx.rs).
The demo library is `exprkit` (an arithmetic expression evaluator with
variables). Two pieces: `cpp/` (the Bazel `cc_library`, its GoogleTest suite,
and a C++ CLI) and `rust/` (crate `exprkit` — the `#[cxx::bridge]`, a shim,
the safe API, and a Rust CLI). The full walkthrough is in
[`README.md`](README.md).

This is the mirror of [`../rust_python_bindings`](../rust_python_bindings),
which exports Rust *to* a higher-level language. Same layering discipline,
different hazards: exceptions rather than error returns, object lifetime rather
than reference counting, two build systems rather than one.

## Commands

```sh
bazel test //cpp_rust_bindings/...            # the logic lives here (26 cases)
bazel run -- //cpp_rust_bindings/cpp:exprkit_cli '2 ^ 10'

cargo test -p exprkit                         # the seam (24 tests, incl. doctests)
cargo run -q -p exprkit -- '2 ^ 10'
```

`bazel run` and `cargo run` both need `--` before the program's own flags.

## Invariants

- **`cpp/` must never depend on cxx.** Logic goes there and is tested with
  GoogleTest; `rust/src/shim.{hpp,cpp}` only converts types, and has no logic of
  its own. This is the same rule as `statkit`'s "core must never depend on
  pyo3".
- **Every fallible C++ function is declared `-> Result<T>` in the bridge.** That
  declaration is what makes cxx generate the try/catch. Without it a `throw`
  unwinds into Rust frames that cannot handle it and the process aborts. There
  is no partial credit here — a new binding that can throw and is not `Result`
  is a latent crash.
- **The two CLIs print byte-identical output** (results, error messages, exit
  codes `0`/`1`/`2`). This is structural, not conventional: both call
  `exprkit::format_value`, the C++ formatter, so there is no second
  implementation to keep in sync. **Do not "simplify" the Rust CLI by formatting
  floats in Rust** — `tests/evaluate.rs::the_two_clis_share_one_formatter` is
  the tripwire. `--help` text does differ, and should; each argument parser
  words its own.
- **Test the logic in C++, the seam in Rust.** `cpp/test_exprkit.cpp` owns
  precedence, parsing, and the error taxonomy. The Rust tests deliberately do
  not re-check arithmetic; they check what only they can — bit-for-bit floats,
  every throw site arriving as an `Err`, `UniquePtr` ownership surviving moves,
  and the environment still being usable after a failure.
- **`build.rs` must list every C++ input** in its `rerun-if-changed` loop. The
  sources in `../cpp` are outside the crate, and printing any `rerun-if-changed`
  disables cargo's default directory watching. Add a `.cpp` and forget this, and
  edits silently do not rebuild.
- **`Pin<&mut Evaluator>`, never `&mut Evaluator`.** Safe Rust can `mem::swap`
  through a `&mut`, which is unsound for a C++ object whose address matters.

## Gotcha: the C++ is compiled twice

Bazel builds `cpp/` for its tests and CLI; cargo cannot see Bazel, so `build.rs`
hands the same `.cpp` files to `cxx_build` and links the result into the crate.
Sources are shared, object files are not.

Consequences worth remembering: **`bazel test` passing does not prove
`cargo test` builds**, so check a change to `cpp/` with both. And the C++
standard is set in two places — `--cxxopt=-std=c++20` in `//.bazelrc` for Bazel,
`.std("c++20")` in `build.rs` for cargo. `exprkit.cpp` uses `std::format`,
`std::numbers`, and floating-point `from_chars`, so neither may be lowered.

## Gotcha: strings cross asymmetrically

Arguments travel as `rust::Str`, a pointer/length pair, so an interior NUL is
data. Exception messages travel as `what()`, a C string, so they **truncate at
the first NUL** — and a message that is not valid UTF-8 is **converted lossily**
to U+FFFD rather than erroring. Both behaviors are pinned by tests in
`rust/src/lib.rs`; they are cxx's, not bugs here.
