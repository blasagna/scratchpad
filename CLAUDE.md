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

Bazel (C and C++):
```sh
bazel build //c_little_book/hello_world:hello
bazel run //c_little_book/hello_world:hello
bazel test //c_little_book/recursion:test_math
bazel test //...                    # all Bazel tests
```

pixi (Python) — run from within a project directory. Task names vary per project
(see the area docs); LeetCode problems expose `test` and `main`:
```sh
cd leetcode/array_shuffle
pixi run test     # runs unittest
pixi run main     # runs solution.py directly
```

cargo (Rust) — the Rust crates form a single workspace (root `Cargo.toml`, members
`text_analyzer/rust`, `morse_trainer`, `copy_file/rust`, `cpp_rust_bindings/rust`,
`rust_hosted_cpp/rust`, `rust_python_bindings/{core,bindings}`; shared `target/` at
the repo root):
```sh
cargo test                    # all workspace members
cargo test -p morse_trainer   # a single member
cd copy_file/rust && cargo test   # or work from within the member
```

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
