# matrix_ops

A CLI that adds, subtracts, multiplies, and scales 2D matrices of real numbers,
implemented twice with matching semantics: `c/` and `cpp/` (both Bazel). Rust
follows. The full contract — CLI surface, shape rules, output format, exit
codes — is in [`README.md`](README.md), along with a benchmark and adoption
writeup comparing the ports against Eigen and xtensor. Each port has its own
README with design notes ([c](c/README.md), [cpp](cpp/README.md)).

## Commands

```sh
bazel run  //matrix_ops/c:matrix_ops   -- <add|sub|mul|scale> [operand...]
bazel run  //matrix_ops/cpp:matrix_ops -- <add|sub|mul|scale> [operand...]

bazel test //matrix_ops/c:all
bazel test //matrix_ops/cpp:all

./matrix_ops/check_parity.sh   # both ports, byte for byte, 56 cases
./matrix_ops/bench/run.sh      # ours vs Eigen vs xtensor
```

## Shared behavior (keep the ports in sync)

- **Dimensions are optional, and the layout is the default.** One line of values
  is a `1 x N` row vector; several lines are rows. `--rows`/`--cols` override, and
  a single one of them derives the other by division. The full rule set is in
  [`README.md`](README.md#shape) and lives in one function, `resolve_shape`, so a
  port has one thing to reproduce rather than a scattering of special cases.
- **Ragged input is an error even when `--rows`/`--cols` are given**, though the
  layout is otherwise ignored in that case. It is a deliberate asymmetry: a file
  whose rows differ in length is far more often a typo than a request to reshape.
- **`--rows`/`--cols` bind to the *next* operand, not the previous one**, and
  reset once it closes. Dimensions left over at the end of the command line are a
  usage error rather than silently ignored — otherwise a misplaced `--rows` would
  quietly produce a differently-shaped answer.
- **The accepted number set is written down, not inherited from the standard
  library.** `nan`, `inf`, `infinity`, and an overflow to infinity are rejected;
  an underflow to zero is accepted. Same reasoning as `simple_logger`'s
  hand-checked `-?[0-9]+`.

  **Both ports use `strtod`, and the C++ one deliberately does not use
  `std::from_chars`** — reversing what this file originally predicted. For
  `double`, `from_chars` rejects a leading `+` (the contract accepts `+3`) and
  reports `result_out_of_range` for underflow as well as overflow, so it cannot
  accept `1e-400` while refusing `1e400` the way the contract requires. The
  reasoning is written up in [`cpp/README.md`](cpp/README.md). The Rust port
  will have to make the same call about `parse::<f64>`.
- **Output is `printf("%.*f")` with trailing zeros trimmed, never `%g`.** `%g`
  counts significant digits rather than decimals and switches to scientific
  notation for large values, which reads badly in a column. A port reaching for
  its own "shortest representation" formatter will diverge here — the C++ port
  uses `std::format("{:.{}f}")`, *not* `std::format("{}")`.

- **Neither port lets its option parser write the error message.** `getopt_long`
  prefixes its diagnostics with `argv[0]`, which under Bazel is the full path to
  the binary, so no other port can ever match the wording. The C port sets
  `opterr = 0` and uses a leading `:` in the option string so it can report
  unknown options and missing values itself. Both ports print
  `error: unknown option '--x'` and `error: option '--x' requires a value`.
- **Rounding ties go to the even digit** (`0.25` at one decimal is `0.2`).
  `printf` and Rust's formatter both do this;
  `BreaksARoundingTieTowardsTheEvenDigit` in `c/test_matrix_io.c` pins it, and it
  is the first test to check if a port's output starts drifting in the last
  place.
- **A negative zero always prints as `0`.** It arrives two ways — the double
  `-0.0` from `scale --scalar 0`, and a small negative rounded away by the
  precision — so the check is on the *rendering*, not on the value.
- **Every line of output ends in a newline, including the last.**
- **Exit codes**: `2` usage, `1` operational, `0` success. A dimension mismatch is
  a usage error (`2`), since it always traces back to what was typed.

## Gotchas

- **`bazel run` and relative paths.** `bazel run` executes from Bazel's runfiles
  directory, not your shell's cwd, so a relative `--file` path resolves somewhere
  surprising. Pass an absolute path or run `bazel-bin/matrix_ops/c/matrix_ops`
  directly. (Same trap as `copy_file` and `simple_logger`.)
- **`valgrind` does not run on this machine.** The pixi-provided build fails at
  startup against the system's stripped `ld.so` ("a function redirection which is
  mandatory for this platform-tool combination cannot be set up"). Use the
  sanitizers instead:

  ```sh
  bazel test //matrix_ops/c:all //matrix_ops/cpp:all --config=permissive \
    --copt=-fsanitize=address --copt=-g --linkopt=-fsanitize=address
  ```

- **`bench/` is the only non-hermetic target in the repo.** xtensor-blas links
  a *system* OpenBLAS (`linkopts = ["-lopenblas"]`) because neither it nor any
  BLAS is in the Bazel Central Registry. Nothing else depends on it, and neither
  port does.

- **`HAVE_CBLAS=1` in `//third_party/xtensor_blas.BUILD` is load-bearing, and
  its absence fails silently.** Without it xtensor-blas skips its `cblas_dgemm`
  overloads and uses a generic C++ gemm that is ~50x slower — while still
  producing correct results, and while `ldd` still shows `libopenblas.so`
  because it is linked but never called. `bench/run.sh` now hard-fails if the
  binary references no `cblas_*` symbols; do not relax that check into an `ldd`
  grep, which is what originally reported a confident green on a wrong number.

- **A benchmark against OpenBLAS decides on `-march`, not on the libraries.**
  OpenBLAS picks an AVX-512 kernel at runtime via CPUID whatever we compile
  with, but `--config=opt` alone builds Eigen and our code for baseline
  `x86-64` — SSE2, no FMA. That one flag is worth 3.3x to Eigen's GEMM and was
  most of what first looked like a 4x kernel-quality gap. `bench/run.sh` reports
  baseline and `--config=native` separately so the two cannot be confused
  again. `--config=native` is not folded into `opt` because it produces
  binaries that will not run on another machine.

- **Eigen's parallelism is a flag on *our* target, not a build of Eigen.** It is
  header-only; its GEMM is guarded by `#ifdef EIGEN_HAS_OPENMP`, which keys off
  `_OPENMP`, so `copts = ["-fopenmp"]` + the matching `linkopts` on
  `//matrix_ops/bench:compare` is all of it. Also set `OMP_PROC_BIND=true` when
  measuring Eigen or its numbers swing wildly — but note that binding the master
  thread makes OpenBLAS's pthread pool inherit the affinity mask and collapse
  onto one core, so the two cannot be measured fairly in the same run. See the
  caveat in [`README.md`](README.md).

- **Third-party headers need `--features=external_include_paths`**, set in
  `.bazelrc`. The repo's `-Werror -Wextra -pedantic` otherwise applies to Eigen,
  xtensor, and the FLENS tree, none of which are warning-clean. Eigen alone
  would have survived, since its BUILD uses `includes` (which yields
  `-isystem`); xtensor uses `strip_include_prefix`, which does not.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
