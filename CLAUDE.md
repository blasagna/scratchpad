# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Personal scratchpad for learning exercises — small programs implemented across
several languages to practice tools and patterns. Each top-level directory is a
self-contained area with its own nested `CLAUDE.md` (loaded on demand when you
work in that subtree) and usually a `README.md` with the full narrative.

## Areas

| Area | What | Details |
|------|------|---------|
| `c_little_book/` | C exercises from the little book of C (Bazel) | [`c_little_book/CLAUDE.md`](c_little_book/CLAUDE.md) |
| `algo_little_book/` | Algorithm/data-structure exercises in Python (pixi) | [`algo_little_book/CLAUDE.md`](algo_little_book/CLAUDE.md) |
| `leetcode/` | LeetCode solutions in Python, one pixi workspace per problem | [`leetcode/CLAUDE.md`](leetcode/CLAUDE.md) |
| `copy_file/` | A `cp`-like file copier, ported to C / C++ / Rust | [`copy_file/CLAUDE.md`](copy_file/CLAUDE.md) |
| `text_analyzer/` | A text-stats CLI, ported to C / C++ / Rust with cross-port parity | [`text_analyzer/CLAUDE.md`](text_analyzer/CLAUDE.md) |
| `simple_logger/` | A timestamped log-appender CLI, ported to C / C++ / Rust | [`simple_logger/CLAUDE.md`](simple_logger/CLAUDE.md) |
| `matrix_ops/` | A 2D matrix arithmetic CLI, ported to C / C++ / Rust, benchmarked against Eigen, xtensor, faer, and nalgebra | [`matrix_ops/CLAUDE.md`](matrix_ops/CLAUDE.md) |
| `mini_shell/` | A prototype shell that forks and execs one program per line, ported to C / C++ / Rust | [`mini_shell/CLAUDE.md`](mini_shell/CLAUDE.md) |
| `morse_trainer/` | A terminal UI for practicing Morse code (Rust) | [`morse_trainer/CLAUDE.md`](morse_trainer/CLAUDE.md) |
| `rust_python_bindings/` | Python bindings for a Rust library, with PyO3 + maturin | [`rust_python_bindings/CLAUDE.md`](rust_python_bindings/CLAUDE.md) |
| `cpp_rust_bindings/` | Rust bindings for a C++ library, with cxx | [`cpp_rust_bindings/CLAUDE.md`](cpp_rust_bindings/CLAUDE.md) |
| `rust_hosted_cpp/` | A C++ library with no build system of its own, built/tested/run entirely from Rust | [`rust_hosted_cpp/CLAUDE.md`](rust_hosted_cpp/CLAUDE.md) |

## Build systems by language

| Language | Build system |
|----------|-------------|
| C        | Bazel       |
| C++      | Bazel       |
| Python   | pixi        |
| Rust     | cargo       |
| Rust → Python extension | maturin (driven by a pixi task) |
| C++ → Rust extension | cargo (a `build.rs` compiles the C++ a second time) |
| C++ with no build of its own | cargo (`build.rs` is the only build config, and sets the standard and warnings too) |

## Commands

Formatting is repo-wide via `pixi run fmt` (ruff + clang-format + cargo fmt) and
runs automatically on a `Stop` hook, so you rarely need to invoke it by hand.

## Repo-wide conventions

**Bazel builds are strict by default** (`.bazelrc`): `-Wall -Werror -Wextra
-pedantic`, and C++ compiles with `-std=c++20`. During iteration, opt out with
`--config=permissive` (warnings still shown, not fatal). The default build is
`fastbuild` (`-O0`); use `--config=opt` for any timing measurement.

**C tests with GoogleTest**: GoogleTest is a C++ library, so C test files wrap the
headers under test in `extern "C" { ... }` and their `cc_test` target adds
`copts = ["-x", "c++"]` to compile the C test file as C++. Pure C++ targets don't
need this.

**Typical BUILD rules**: `cc_binary` (executables), `cc_library` (shared code:
`srcs` + `hdrs`), `cc_test` (link `@googletest//:gtest_main`). A new C/C++ package
needs its own `BUILD`; no `MODULE.bazel` change is needed for standard targets
since `rules_cc` and `googletest` are already declared.

**Third-party dependencies** are declared in `MODULE.bazel` and fall into two
groups. CLI11 parses the command line for every C++ binary — it is the C++
counterpart to `getopt_long` in the C ports and `clap` in the Rust ones, and it
is the only library the ports themselves depend on. Eigen, xtensor, and
xtensor-blas are used by exactly one package, `//matrix_ops/bench`, for a
comparison benchmark. `third_party/` holds BUILD overlays for archives that are
not in the Bazel Central Registry (only xtensor-blas needs one; CLI11 is in the
registry, so it is a bare `bazel_dep`). Two things are worth knowing before
adding another: `.bazelrc` sets `--features=external_include_paths` so the
repo's `-Werror` does not apply to external headers, and `//matrix_ops/bench` is
the only non-hermetic target here — it links a system OpenBLAS. See
[`matrix_ops/CLAUDE.md`](matrix_ops/CLAUDE.md).

The Rust side has the same shape: the ports themselves depend only on `clap`,
and the linear-algebra crates (`faer`, `nalgebra`) and `criterion` are confined
to `matrix_ops/bench/rust`. Unlike the Bazel benchmark, that one is hermetic —
both libraries are pure Rust, so there is no system BLAS to link.

**Argument parsing is the library's, and so is the grammar — where a library
check can carry it.** Each C++ port declares its options to CLI11 and lets it
write `--help` and reject unknown options. Whether CLI11 may also own an
option's *grammar* depends on what the option is bound to:

- **Numeric options are bound to their real type and checked with
  `CLI::Range`** — `matrix_ops` (`int`/`double`) and `text_analyzer` (`unsigned
  int`) both do this. CLI11's integer conversion reads base 0, strips `_` and
  `'` group separators, and skips surrounding whitespace, so these ports accept
  spellings (`--rows 0x10`, `--rows 1_000`, `--top-n "5 "`) that C refuses, and
  `010` means eight here and ten there. The divergence is deliberate and
  tabulated per area, in
  [`matrix_ops/README.md`](matrix_ops/README.md#known-divergence-argument-parsers)
  and
  [`text_analyzer/README.md`](text_analyzer/README.md#known-divergence-argument-parsers).
  Prefer `CLI::Range` over `CLI::PositiveNumber`: the latter is a
  `Range<double>`, so `2.5` clears the check and fails later in the conversion.
- `simple_logger` has one constrained option, `--level`, bound to a
  `std::string` and checked with `CLI::IsMember`. Nothing converts a string, so
  the library's own check compares the bytes as typed and accepts exactly what
  C's `strcmp` does. The trap there is `CLI::CheckedTransformer`, the obvious way
  to map names onto an enum: it also accepts the enum's underlying integers.
- **A hand-written validator behind `->check()` is the last resort**, for a rule
  no built-in can state — `matrix_ops` rejects a NaN `--scalar` by hand, since
  `CLI::Range` tests `val < min || val > max` and both are false for a NaN.
  `text_analyzer` used to hand-check its integers to keep them byte-identical
  with C, and that was a mistake: the validator was stricter than C's `strtol`
  (which skips leading whitespace and takes a `+`), so it pinned the C++ port to
  a third dialect no port actually had.

See the per-area `CLAUDE.md` files.

**New pixi problems** follow the same pattern: `solution.py`, `test_solution.py`,
and a `pixi.toml` with `test` and `main` tasks (`pixi init <directory>` to start).
Tests use Python's built-in `unittest`.

**New Rust crates** use the 2024 edition (`edition = "2024"`) and are added to the
`members` list in the root `Cargo.toml`.

## Skills

Two skills cover the `text_analyzer` maintenance workflows (their descriptions load
on demand):
- **text-analyzer-bench** — benchmark and parity-check the three ports.
- **text-analyzer-goldens** — regenerate the golden test outputs after an
  intentional behavior change.
