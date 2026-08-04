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

[`../rust_hosted_cpp`](../rust_hosted_cpp) is the same seam with the second
ecosystem removed — a C++ library with no build file, no test binary, and no CLI
of its own. Compare the two before adding a `BUILD` to a new C++ library: the
duplication documented below is the price of the C++ side staying independently
buildable, and it is a price worth naming out loud rather than paying by reflex.

## Commands

```sh
bazel test //cpp_rust_bindings/...            # the logic lives here (31 cases)
bazel run -- //cpp_rust_bindings/cpp:exprkit_cli '2 ^ 10'

cargo test -p exprkit                         # the seam (33 tests, incl. doctests)
cargo run -q -p exprkit -- '2 ^ 10'
```

`bazel run` and `cargo run` both need `--` before the program's own flags.

## Invariants

- **`cpp/` must never depend on cxx.** Logic goes there and is tested with
  GoogleTest; `rust/src/shim.{hpp,cpp}` only converts types, and has no logic of
  its own. This is the same rule as `statkit`'s "core must never depend on
  pyo3".
- **Every bridge function is declared `-> Result<T>`, without exception.** That
  declaration is what makes cxx generate the try/catch. Without it a `throw`
  unwinds into Rust frames that cannot handle it and the process aborts. This
  includes functions with no arithmetic error to report — `format_value`,
  `names`, `new_evaluator`, `clear`, `has` all allocate, and `std::bad_alloc` is
  a `throw` like any other. Where the public signature stays infallible, the
  wrapper funnels through `expect_no_throw`, converting an abort into a panic.
  There is no partial credit here; a new binding that is not `Result` is a
  latent crash.
- **`Evaluator::eval` is strongly exception-safe.** Assignments are buffered and
  applied only after the whole input parses, so `x = 1 2` defines nothing. Tests
  that only use inputs failing *before* the assignment (`y = 1 / 0`) pass
  vacuously — cover the after case or the guarantee is untested.
- **The parser is depth-capped at 256 recursion levels.** A stack overflow
  cannot be caught and so cannot become an `Err`; it is the one failure mode
  that would defeat the whole design. Any new recursive path in the parser needs
  a `DepthGuard`.
- **The two CLIs print byte-identical output** (results, error messages, exit
  codes `0`/`1`/`2`). This is structural, not conventional: both call
  `exprkit::format_value`, the C++ formatter, so there is no second
  implementation to keep in sync. **Do not "simplify" the Rust CLI by formatting
  floats in Rust** — `tests/evaluate.rs::the_two_clis_share_one_formatter` is
  the tripwire. Two documented exceptions remain, both conventions rather than
  exprkit behavior: `--help` text (CLI11 writes one, clap the other) and
  non-UTF-8 input (spelled `'�'` rather than the raw byte). Unknown-option
  wording and its exit code are *not* exceptions — both print
  `exprkit: unknown option: --x` and exit 2 — and a bare `--` now ends option
  parsing in both, which it did not before the C++ CLI had a parser. Nothing
  else may drift.
- **Neither CLI lets its parser decide what an option is.** Only a `--` prefix
  marks one; a single dash never does, because a leading minus is arithmetic
  here — `-2 ^ 2` is -4, `-e` is -2.718…, `-x + 1` reads a variable. Both ports
  therefore pre-scan argv with a `split_options` pass and hand the library only
  what is genuinely an option. **Do not delete either one.** Left to itself
  clap rejects `-2 ^ 2`, and CLI11 classifies every `-<non-digit>` as a short
  option — which rejects `-e` and `-pi`, exprkit's own constants, and silently
  prints *help* for `-h + 1`, since `-h` is a real flag. That last one is the
  reason this is a pre-scan and not a documented divergence: it is a wrong
  answer with exit 0, not a rejection.
- **The Rust CLI's option handling is not just `#[derive(Parser)]`.**
  `allow_hyphen_values` is required so `-2 ^ 2` is arithmetic, but it also makes
  clap ignore options after the first positional; `split_options` restores the
  position-independent scan. Do not delete either half. Likewise, stdin is
  read with `from_utf8_lossy`, not `BufRead::lines()`, which fails a whole run
  on one stray byte.
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
