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
| `copy_file/` | A `cp`-like file copier, ported to C / C++ / Rust | [`copy_file/CLAUDE.md`](copy_file/CLAUDE.md) |
| `text_analyzer/` | A text-stats CLI, ported to C / C++ / Rust with cross-port parity | [`text_analyzer/CLAUDE.md`](text_analyzer/CLAUDE.md) |
| `simple_logger/` | A timestamped log-appender CLI, ported to C / C++ / Rust | [`simple_logger/CLAUDE.md`](simple_logger/CLAUDE.md) |
| `matrix_ops/` | A 2D matrix arithmetic CLI, ported to C / C++ / Rust, benchmarked against Eigen, xtensor, faer, and nalgebra | [`matrix_ops/CLAUDE.md`](matrix_ops/CLAUDE.md) |
| `mini_shell/` | A prototype shell that forks and execs one program per line, ported to C / C++ / Rust | [`mini_shell/CLAUDE.md`](mini_shell/CLAUDE.md) |
| `tiny_http_server/` | An HTTP server that serves one hello-world page, one connection at a time, ported to C / C++ / Rust with cross-port parity | [`tiny_http_server/CLAUDE.md`](tiny_http_server/CLAUDE.md) |
| `morse_trainer/` | A terminal UI for practicing Morse code (Rust) | [`morse_trainer/CLAUDE.md`](morse_trainer/CLAUDE.md) |
| `rust_python_bindings/` | Python bindings for a Rust library, with PyO3 + maturin | [`rust_python_bindings/CLAUDE.md`](rust_python_bindings/CLAUDE.md) |
| `cpp_rust_bindings/` | Rust bindings for a C++ library, with cxx | [`cpp_rust_bindings/CLAUDE.md`](cpp_rust_bindings/CLAUDE.md) |
| `rust_hosted_cpp/` | A C++ library with no build system of its own, built/tested/run entirely from Rust | [`rust_hosted_cpp/CLAUDE.md`](rust_hosted_cpp/CLAUDE.md) |
| `dfg/` | A dataflow graph framework for real-time and batch processing — a language-independent design contract plus a Python port (pixi) | [`dfg/CLAUDE.md`](dfg/CLAUDE.md) |
| `microbit_v2_zephyr/` | A Zephyr RTOS application for the BBC micro:bit V2 — sensors, buzzer, on-device FFT, and BLE notifications (west), plus host-side programs in their own pixi env — a BLE throughput reader, a rerun visualizer, and a tone sweep that checks the reported peak frequency over the console shell | [`microbit_v2_zephyr/CLAUDE.md`](microbit_v2_zephyr/CLAUDE.md) |
| `rpi_pico_rust_debug/` | A minimal embassy `no_std` firmware for the Raspberry Pi Pico W (RP2040), for exercising `probe-rs` debugging — breakpoints, watchpoints, panic backtraces — via a deliberate bug | [`rpi_pico_rust_debug/CLAUDE.md`](rpi_pico_rust_debug/CLAUDE.md) |
| `rpi_pico_zephyr_debug/` | The C++/Zephyr counterpart of `rpi_pico_rust_debug` for the same Pico W: the same deliberate ring-buffer bug, for exercising OpenOCD + Raspberry Pi Debug Probe debugging (breakpoints, watchpoints, fatal-error backtraces) via `west`, plus a `native_sim` ztest suite for the pure conversion | [`rpi_pico_zephyr_debug/CLAUDE.md`](rpi_pico_zephyr_debug/CLAUDE.md) |
| `memory_optimization/` | C++ demos of the cache/memory optimization techniques in Section 6 of Drepper's "What Every Programmer Should Know About Memory" — one small project per technique (Bazel + Google Benchmark; NUMA excluded) | [`memory_optimization/CLAUDE.md`](memory_optimization/CLAUDE.md) |
| `feather_sense_zephyr/` | A Zephyr RTOS application for the Adafruit Feather Sense (nRF52840) — IMU (an out-of-tree LSM6DS33/LSM6DS3TR-C driver over ST's vendored stmemsc code), magnetometer, environmental, battery and button streams over both USB CDC and BLE, with an RPC channel and a NeoPixel battery indicator (west), plus `native_sim` ztests and host-side rate CLIs and a rerun viewer in their own pixi env | [`feather_sense_zephyr/CLAUDE.md`](feather_sense_zephyr/CLAUDE.md) |

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
| C / C++ on Zephyr RTOS | west (CMake), as a freestanding application against `~/zephyrproject` |

## Commands

Formatting is repo-wide via `pixi run fmt` (ruff + clang-format + cargo fmt) and
runs automatically on a `Stop` hook, so you rarely need to invoke it by hand.
`fmt-c` takes its file list from `git ls-files` rather than a `**` glob, so that
gitignored build directories are skipped. That matters because some of them hold
symlinks that lead out of the repo — the west areas' `build/` directories
(`microbit_v2_zephyr/build/`, `rpi_pico_zephyr_debug/build/`, and
`feather_sense_zephyr/build/`) link to the Zephyr installation, and a glob that follows them reformats it in place.

`pixi run lint-c` runs cppcheck over the 7 Bazel C/C++ areas. Memory checking for a
given area is `bazel test <targets> --config=valgrind` (needs the system package
`libc6-dbg`, see [`README.md`](README.md)) or `--config=asan` for a faster
alternative; see the per-area `CLAUDE.md` for exact invocations.

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

`google_benchmark` is the one other benchmarking dependency, used by every
`bench_*` binary under `//memory_optimization/...` (the Section-6 demos) and by
nothing else. It is a bare `bazel_dep` (in the Bazel Central Registry, no overlay
needed); its bench binaries link `@google_benchmark//:benchmark_main`, or
`@google_benchmark//:benchmark` when the demo supplies its own `main()`. See
[`memory_optimization/CLAUDE.md`](memory_optimization/CLAUDE.md).

The Rust side has the same shape: the ports themselves depend only on `clap`,
and the linear-algebra crates (`faer`, `nalgebra`) and `criterion` are confined
to `matrix_ops/bench/rust`. Unlike the Bazel benchmark, that one is hermetic —
both libraries are pure Rust, so there is no system BLAS to link.

**Argument parsing is the library's, and so is the grammar — where a library
check can carry it.** Each C++ port declares its options to CLI11 and lets it
write `--help` and reject unknown options. Whether CLI11 may also own an
option's *grammar* depends on what the option is bound to:

- **Numeric options are bound to their real type and checked with
  `CLI::Range`** — `matrix_ops` (`int`/`double`), `text_analyzer` (`unsigned
  int`), and `tiny_http_server` (`--port`, an `int`) all do this. CLI11's
  integer conversion reads base 0, strips `_` and
  `'` group separators, and skips surrounding whitespace, so these ports accept
  spellings (`--rows 0x10`, `--rows 1_000`, `--top-n "5 "`) that C refuses, and
  `010` means eight here and ten there. The divergence is deliberate and
  tabulated per area, in
  [`matrix_ops/README.md`](matrix_ops/README.md#known-divergence-argument-parsers)
  and
  [`text_analyzer/README.md`](text_analyzer/README.md#known-divergence-argument-parsers),
  and
  [`tiny_http_server/README.md`](tiny_http_server/README.md#known-divergences).
  Prefer `CLI::Range` over `CLI::PositiveNumber`: the latter is a
  `Range<double>`, so `2.5` clears the check and fails later in the conversion.
- `simple_logger` has one constrained option, `--level`, bound to a
  `std::string` and checked with `CLI::IsMember`. Nothing converts a string, so
  the library's own check compares the bytes as typed and accepts exactly what
  C's `strcmp` does. The trap there is `CLI::CheckedTransformer`, the obvious way
  to map names onto an enum: it also accepts the enum's underlying integers.
- **A hand-written validator behind `->check()` is the last resort**, for a rule
  no built-in can state — `matrix_ops` rejects a NaN `--scalar` by hand, since
  `CLI::Range` tests `val < min || val > max` and both are false for a NaN, and
  `tiny_http_server/cpp` checks `--host` with `inet_pton`, since the built-in
  `CLI::ValidIPV4` splits on `.` and range-checks four numbers of its own
  parsing rather than asking the resolver's own parser. **The last resort is
  per-port, not per-option**: `tiny_http_server/rust` declares that same `--host`
  as an `Ipv4Addr` and writes no validator at all, because Rust's parser for
  that type already *is* `inet_pton`'s grammar, leading zeros and all. A rule
  one library cannot state is not a rule the next one cannot.
  `text_analyzer` used to hand-check its integers to keep them byte-identical
  with C, and that was a mistake: the validator was stricter than C's `strtol`
  (which skips leading whitespace and takes a `+`), so it pinned the C++ port to
  a third dialect no port actually had.

See the per-area `CLAUDE.md` files.

**New pixi problems** follow the same pattern: `solution.py`, `test_solution.py`,
and a `pixi.toml` with `test` and `main` tasks (`pixi init <directory>` to start).
Tests use Python's built-in `unittest`.

That script-pair shape fits a single exercise, not a library. `dfg/python/` is the
first Python area that is package-shaped instead — a `dfg/` package beside
`examples/` and `tests/`, run with `python -m unittest discover -s tests` and no
`pyproject.toml`, since `python -m` from the manifest directory puts it on
`sys.path`. It is also the only area with scientific dependencies (numpy, pyarrow),
and they are confined to its examples by a test rather than by a pixi environment;
see [`dfg/CLAUDE.md`](dfg/CLAUDE.md). Because those dependencies are absent from the
root environment, `dfg/python` type-checks with its own `pixi run type` rather than
through the root `type-py` — the same arrangement `rust_python_bindings` uses. It is
also the only area with a `pyrefly.toml`: without a config pyrefly runs its `basic`
preset, which is lenient enough to miss `x: int = "s"`, and `dfg/python` opts into
`default` instead. The other areas still run at `basic`.

`microbit_v2_zephyr/host/` is the third area with its own environment, for the same
reason: it needs bleak to talk to the board over BLE and rerun-sdk to plot what it hears,
and the root environment holds only dev tools. Its `pixi run type` passes `-p default` on
the command line rather than adding a `pyrefly.toml`. Both of its programs are plain
scripts rather than a package, and the second one imports the first directly — the wire
format is defined once, in `ble_stream.py`.

**New Rust crates** use the 2024 edition (`edition = "2024"`) and are added to the
`members` list in the root `Cargo.toml` — unless, like `rpi_pico_rust_debug/`, the
crate targets something other than the host (an embedded target with its own
`.cargo/config.toml` pinning `[build] target`), in which case it declares its own
`[workspace]` table and stays out of the root workspace instead.

That exception is about the *target*, not the directory. `rpi_pico_rust_debug/`
keeps a host-buildable leaf crate, `rpi_pico_rust_debug/rp2040_temp/`, that **is**
a root member, so that `cargo test` from the root covers it — inside that
directory the pinned `thumbv6m-none-eabi` target has no `std` for libtest to link
(`error[E0463]: can't find crate for 'test'`), so tests can't run there at all.
The firmware workspace therefore also carries `exclude = ["rp2040_temp"]`, since
a path dependency inside a workspace directory would otherwise be claimed as a
member of it. Pure logic worth testing goes in the leaf; anything touching the
chip stays in the firmware crate.

## Skills

Two skills cover the `text_analyzer` maintenance workflows (their descriptions load
on demand):
- **text-analyzer-bench** — benchmark and parity-check the three ports.
- **text-analyzer-goldens** — regenerate the golden test outputs after an
  intentional behavior change.
