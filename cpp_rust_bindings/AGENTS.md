# cpp_rust_bindings

A worked example of exposing a C++ library to Rust with [cxx](https://cxx.rs).
The demo library is `exprkit` (an arithmetic expression evaluator with
variables). Two pieces: `cpp/` (the Bazel `cc_library`, its GoogleTest suite,
and a C++ CLI) and `rust/` (crate `exprkit` — the `#[cxx::bridge]`, a shim,
the safe API, and a Rust CLI). The full walkthrough is in
[`README.md`](README.md).

The hazards of this direction are exceptions rather than error returns, object
lifetime rather than reference counting, and two build systems rather than one.

Think before adding a `BUILD` to a new C++ library here: the duplication
documented below is the price of the C++ side staying independently buildable —
its own Bazel target, its own GoogleTest suite, its own CLI — and it is a price
worth naming out loud rather than paying by reflex.

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
  the tripwire. What is guaranteed is the *result* of evaluating an expression
  and the exit code for it (`0` ok, `1` evaluation error), over the shared
  surface: one quoted EXPRESSION, stdin, and `--names`. Outside that sit the
  things the two argument parsers own, none of them exprkit behavior:

  | | C++ (CLI11) | Rust (clap) |
  |---|---|---|
  | `--help` text | CLI11's | clap's |
  | argument-error wording and exit code | CLI11's, `104`/`109`/… | clap's, `2` |
  | `-e`, `-x + 1` without `--` | rejected | evaluated |

  A leading *negative number* works bare in both — `exprkit '-2^2'` is `-4`
  either way. The row above is the remainder: clap is the more permissive of the
  two about a dash followed by a non-digit. Both agree once `--` is used, which
  is the spelling to prefer in docs and examples.
- **Each CLI takes exactly one expression, and lets its parser do the parsing.**
  Both did accept a list, with a hand-written `split_options` pre-scan on each
  side deciding what counted as an option. One quoted argument removed the need
  for both. An expression beginning with `-` is written after `--`.
- **Neither declares a `-h` alias; only `--help`.** Both parsers classify
  `-<non-digit>` as a short option, so with `-h` declared, `exprkit '-h + 1'`
  matched the help flag and printed help with **exit 0** — a wrong answer that
  looks like success. Undeclared, the same argument is an ordinary rejection.
  Do not add the alias back for symmetry with other tools.
- **`allow_hyphen_values` on the Rust expression needs its `value_parser`.**
  The attribute is what makes `exprkit '-2^2'` arithmetic rather than an unknown
  flag, matching what CLI11 does for free. Alone it goes too far: clap hands
  `--bogus` over as the expression, so a typo'd flag is *evaluated*
  (`unknown name: 'bogus'`, exit 1) instead of reported. `expression_value`
  rejects a `--` prefix and puts that back as a usage error. Do not delete
  either half, and do not reach for `clap::Arg::allow_negative_numbers` instead
  — it requires the whole value to parse as a number, so it takes `-4` and still
  refuses `-2^2`.
- **stdin is read with `from_utf8_lossy`, not `BufRead::lines()`**, which fails
  a whole run on one stray byte where the C++ CLI carries on.
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
