# matrix_ops

A CLI that adds, subtracts, multiplies, and scales 2D matrices of real numbers,
implemented three times: `c/` and `cpp/` (both Bazel) with matching semantics
down to the byte, and `rust/` (cargo) with the same behavior but its own
command-line surface. The full contract — CLI surface, shape rules, output
format, exit codes — is in [`README.md`](README.md), along with benchmarks and
adoption writeups comparing the ports against Eigen, xtensor, faer, and
nalgebra. Each port has its own README with design notes ([c](c/README.md),
[cpp](cpp/README.md), [rust](rust/README.md)).

## Commands

```sh
bazel run  //matrix_ops/c:matrix_ops   -- <add|sub|mul|scale> [operand...]
bazel run  //matrix_ops/cpp:matrix_ops -- <add|sub|mul|scale> [operand...]
cargo run -p matrix_ops --                <add|sub|mul|scale> [operand...]

bazel test //matrix_ops/c:all
bazel test //matrix_ops/cpp:all
cargo test -p matrix_ops

./matrix_ops/check_parity.sh     # C and C++ only, 85 cases
./matrix_ops/bench/run.sh        # ours vs Eigen vs xtensor       (C++)
./matrix_ops/bench/rust/run.sh   # ours vs faer vs nalgebra       (Rust)
```

**`check_parity.sh` compares results, not diagnostics.** Each port reaches for
its own argument parser — `getopt_long` in C, CLI11 in C++, clap in Rust — so
the script splits its cases by who reports the outcome. `run_case` compares
stdout and the exit status, and covers every happy path plus every error the
*program* reports (exit 2 or 1). `run_case_parser_error` covers the ones the
*parser* reports and asserts only that both ports reject the command line;
`run_case_status_only` covers `--help`. Stderr is not compared at all any more.

**The Rust port is still not in the script**, for a reason that has nothing to
do with wording: its dimensions pair with operands by index rather than by the
order typed, so it disagrees on command lines both other ports accept — and this
script can only assert agreement. Its surface is pinned by
`matrix_ops/rust/tests/cli.rs`, and the divergences are tabulated in
[`README.md`](README.md#known-divergence-the-rust-port).

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
  reasoning is written up in [`cpp/README.md`](cpp/README.md).

  **The Rust port answers the question this bullet used to leave open**: it uses
  `parse::<f64>()` with an `is_finite()` guard. That lands every case right —
  `+3`, `.5`, `4.`, `1e-400` → `0`, and `1e400`/`inf`/`nan` rejected — except
  hex floats, which `strtod` accepts and `f64::from_str` does not. Rejecting
  them is the one documented gap in the shared number set; hand-rolling hex
  parsing carries a rounding-correctness burden that a syntax nobody types into
  a matrix does not earn.
- **Output is `printf("%.*f")` with trailing zeros trimmed, never `%g`.** `%g`
  counts significant digits rather than decimals and switches to scientific
  notation for large values, which reads badly in a column. A port reaching for
  its own "shortest representation" formatter will diverge here — the C++ port
  uses `std::format("{:.{}f}")`, *not* `std::format("{}")`.

- **Each port's option parser writes its own diagnostics, and that is not a
  shared behavior.** The C port sets `opterr = 0` and uses a leading `:` in the
  option string so it can report unknown options and missing values itself,
  because `getopt_long` prefixes its own with `argv[0]` — the full runfiles path
  under `bazel run`. The C++ port hands the whole job to CLI11 and takes CLI11's
  wording and CLI11's exit codes (109 for an unknown option, 114 for a missing
  value, 105 for a rejected value). Do not try to reconcile the two; the parity
  script no longer compares stderr, and `--help=x` is a live divergence — the C
  port rejects it, CLI11 reads it as a request for help and exits 0.

  **Which command lines are accepted is no longer fully shared either**, since
  the C++ port now takes CLI11's number grammar as well as its diagnostics — see
  the next bullet. `run_case_parser_error` still catches a divergence in every
  spelling outside that grammar, and a divergence there is still a real bug.
- **The integer options accept `+?[0-9]+` in C and Rust, and whatever CLI11
  accepts in C++.** This is the one part of the contract the ports do not share,
  and it is deliberate: `--rows`, `--cols`, `--precision`, and `--scalar` are
  bound to `int`/`double` and checked with `CLI::Range`, so CLI11 owns the range
  *and* the grammar. Its grammar is a superset of C's — `strtoull` in base 0,
  `_` and `'` stripped as digit separators, surrounding whitespace skipped — so
  `--rows " 2"`, `--rows 0x10` and `--rows 1_000` run here and are usage errors
  in C, and `--rows 010` means eight rows here and ten there. The table is in
  [`README.md`](README.md#known-divergence-argument-parsers).

  **`check_parity.sh` cannot hold this line, so do not expect it to.** The
  script only asserts that the ports agree; the cases that used to pin the
  strict spelling (`err_spaced_rows`, `err_spaced_precision`) are gone, and what
  is left are the spellings both still reject — `++2`, `2.5` for a dimension,
  `0`, past `INT_MAX`, precision over `1100`.

  **NaN is the exception, and it is checked by hand in `run()`.** No CLI11
  validator can reject it: `CLI::Range` tests `val < min || val > max` and every
  comparison against a NaN is false, so a NaN is inside every range there is.
  Infinities need no such help, being greater than `DBL_MAX`. `err_scalar_nan`
  is a `run_case` (not a `run_case_parser_error`) because that check makes the
  C++ port report it itself, at C's exit 2.

  `--precision` is additionally capped at
  `MATRIX_MAX_PRECISION` / `kMaxPrecision` (1100): past ~1074 places every digit
  is a zero the trimming removes, and uncapped it let one cell demand gigabytes
  — where C's `snprintf` overflowed the `int` it returns and reported an
  allocation failure while C++ went ahead and printed 6.3 GB worth.
- **The Rust port pairs dimensions with operands by index, not by position.**
  The Nth `--rows` describes the Nth operand wherever it appears, and inline
  operands are ordered before file ones. Reconstructing C's typed order under
  clap is possible — `ArgMatches::indices_of` draws from one counter shared by
  every argument, so sorting on it recovers the command line — but it is a lot
  of machinery for a rule that only bites in spellings C rejects anyway. Written
  C's way, dimension before operand, all three ports agree.
- **Out of memory is `matrix_ops: out of memory` and exit 1, not a crash** in C
  and C++. The Rust port aborts instead; see the divergence table in
  [`README.md`](README.md#known-divergence-the-rust-port). For C and C++: the
  C port returns `MATRIX_ERR_NOMEM`; the C++ one has to catch `std::bad_alloc`
  to match, which it does in `parse_text`, `read_stream`, and `write`, plus a
  backstop around `main`. Note `read_stream` cannot see the exception at all
  when the read buffer is what fails to grow — the stream's sentry catches it
  and sets `failbit`, indistinguishable from a failed read except by `errno`
  being `ENOMEM`. `check_parity.sh` runs both ports under `ulimit -v` for this.
- **Rounding ties go to the even digit** (`0.25` at one decimal is `0.2`).
  `printf` and Rust's formatter both do this;
  `BreaksARoundingTieTowardsTheEvenDigit` in `c/test_matrix_io.c` pins it, and it
  is the first test to check if a port's output starts drifting in the last
  place.
- **A negative zero always prints as `0`.** It arrives two ways — the double
  `-0.0` from `scale --scalar 0`, and a small negative rounded away by the
  precision — so the check is on the *rendering*, not on the value.
- **Every line of output ends in a newline, including the last.**
- **Exit codes**: `2` usage, `1` operational, `0` success — for what the
  *program* reports. A dimension mismatch is a usage error (`2`), since it
  always traces back to what was typed. Failures the argument parser reports
  carry its own code instead: `getopt_long` never gets to, since the C port
  reports those itself at `2`, but CLI11 does, so the C++ port exits 104/105/109/
  114 on a bad argument. That difference is deliberate and is why those cases
  are `run_case_parser_error` in the script.

## Gotchas

- **`bazel run` and relative paths.** `bazel run` executes from Bazel's runfiles
  directory, not your shell's cwd, so a relative `--file` path resolves somewhere
  surprising. Pass an absolute path or run `bazel-bin/matrix_ops/c/matrix_ops`
  directly. (Same trap as `copy_file` and `simple_logger`.)
- **`valgrind` needs `libc6-dbg` installed** (see root [`README.md`](../README.md)) or
  it fails at startup against the system's stripped `ld.so` ("a function redirection
  which is mandatory for this platform-tool combination cannot be set up"):

  ```sh
  bazel test //matrix_ops/c:all //matrix_ops/cpp:all --config=valgrind
  ```

  `--config=asan` is the faster alternative for iterating:

  ```sh
  bazel test //matrix_ops/c:all //matrix_ops/cpp:all --config=asan
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

- **faer and nalgebra are column-major; our `Matrix` is row-major.** Handing the
  row-major buffer to a column-major constructor silently yields the transpose —
  and the transpose still agrees elementwise on `add`, `sub`, and `scale`, so
  three quarters of the correctness gate would pass while `mul` was wrong.
  `bench/rust/src/lib.rs` builds faer's `Mat` with `from_fn(|i, j| …)` and
  nalgebra's with `from_row_slice`, and every comparison goes through `(i, j)`
  rather than through backing slices. `the_conversions_preserve_orientation…` in
  `bench/rust/tests/agreement.rs` pins it.

- **criterion needs a per-benchmark budget here, not a global one.** Costs span
  five orders of magnitude, from a microsecond `scale` to the ~3-second naive
  1024x1024 `mul`. criterion's default *linear* sampling runs 1, 2, 3, … N
  iterations per sample — about 5000 at the default sample size — which is fine
  at microseconds and absurd at seconds. `benches/compare.rs` switches anything
  over a millisecond to `SamplingMode::Flat` with `sample_size(10)`, criterion's
  minimum. Warm-up always runs at least one full iteration, so the floor for the
  slowest benchmark is `11 * per_iter` however small `warm_up_time` is set.

- **The Rust benchmark needs no thread-pool juggling, and that is a finding.**
  nalgebra never threads (its f64 GEMM goes through `matrixmultiply` without the
  `threading` feature) and faer's parallelism is one global call,
  `faer::set_global_parallelism(Par::Seq | Par::rayon(n))`. So `bench/rust` has
  three tables where `bench/` needs four and has to tell you which column to read
  from each. Do not copy the `OMP_PROC_BIND` apparatus over; there is nothing
  here for it to fix.

- **Third-party headers need `--features=external_include_paths`**, set in
  `.bazelrc`. The repo's `-Werror -Wextra -pedantic` otherwise applies to Eigen,
  xtensor, and the FLENS tree, none of which are warning-clean. Eigen alone
  would have survived, since its BUILD uses `includes` (which yields
  `-isystem`); xtensor uses `strip_include_prefix`, which does not.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
