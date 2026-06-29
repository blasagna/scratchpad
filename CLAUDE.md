# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Personal scratchpad for learning exercises:
- `c_little_book/` — C programming exercises built with Bazel + GoogleTest
- `leetcode/` — LeetCode solutions in Python, each problem in its own pixi workspace

## Build systems by language

| Language | Build system |
|----------|-------------|
| C        | Bazel       |
| C++      | Bazel       |
| Python   | pixi        |
| Rust     | cargo       |

## C and C++ with Bazel

Build and run a binary:
```sh
bazel build //c_little_book/hello_world:hello
bazel run //c_little_book/hello_world:hello
```

Run tests for a target:
```sh
bazel test //c_little_book/recursion:test_math
```

Run all Bazel tests:
```sh
bazel test //...
```

Build flags: strict mode (warnings as errors) is the default via `.bazelrc`. Override with `--config=permissive` if needed during iteration.

**C tests with GoogleTest**: GoogleTest is a C++ library, so C test files wrap headers in `extern "C" { ... }` and BUILD targets add `copts = ["-x", "c++"]` to compile the test file as C++. Pure C++ targets don't need this.

Typical BUILD rules to use:
- `cc_binary` — executables
- `cc_library` — shared code (`srcs` + `hdrs`)
- `cc_test` — test binaries (link `@googletest//:gtest_main`)

New C/C++ packages need a `BUILD` file; no changes to `MODULE.bazel` are needed for standard C/C++ targets since `rules_cc` and `googletest` are already declared.

## Python with pixi

Each project (e.g. each LeetCode problem) is an independent pixi workspace in its own directory. Work from within that directory:

```sh
cd leetcode/array_shuffle
pixi run test     # runs unittest
pixi run main     # runs solution.py directly
```

Tests use Python's built-in `unittest` module. New problems should follow the same pattern: `solution.py`, `test_solution.py`, and a `pixi.toml` with `test` and `main` tasks defined under `[tasks]`.

Initialize a new pixi workspace:
```sh
pixi init <directory>
```

## Rust with cargo

Each Rust project lives in its own directory with a standard `Cargo.toml`. Use cargo directly from within that directory:

```sh
cargo build
cargo run
cargo test                   # run all tests
cargo test <test_name>       # run a single test by name
```
