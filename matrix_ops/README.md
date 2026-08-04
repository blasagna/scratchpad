# matrix operations

A mini project from the little book of c.

Build a library, tests, and a CLI program that implements basic operations on 2D matrices.

Requirements:
1. take arguments for the matrix dimensions (rows and columns)
1. read matrix values from arguments or from a file
1. perform the requested matrix operation: addition, subtraction, multiplication, scalar multiplication
1. display results neatly formatted to stdout

## Contract

The behavior all ports implement. `c/` and `cpp/` are held to this by
`check_parity.sh` (85 cases, comparing stdout and exit status). Every port
delegates argument parsing to its ecosystem's library — `getopt_long` in C,
CLI11 in C++, `clap` in Rust — so `--help` text, diagnostics, and the exit code
for a bad argument are each parser's own and are not compared; see
[Known divergence: argument parsers](#known-divergence-argument-parsers).
`rust/` implements the same operations, shape rules, and output format but is
deliberately outside the script — see
[Known divergence: the Rust port](#known-divergence-the-rust-port).

### CLI

```
matrix_ops <add|sub|mul|scale> [operand...] [options]
```

Each `--values` or `--file` introduces one operand, and any `--rows`/`--cols`
written *before* it describes that operand. That is what lets the two operands of
a product have different shapes. `add`, `sub`, and `mul` take two operands;
`scale` takes one plus `--scalar`.

| Flag | Meaning |
|---|---|
| `-v, --values "..."` | values separated by whitespace or newlines; closes an operand |
| `-f, --file PATH` | read values from a file, `-` for stdin; closes an operand |
| `-r, --rows N` | rows for the next operand (optional, `N >= 1`) |
| `-c, --cols N` | columns for the next operand (optional, `N >= 1`) |
| `-k, --scalar X` | the multiplier for `scale`; a finite number |
| `-p, --precision N` | decimal places in the output, `0 <= N <= 1100` (default `4`) |
| `-h, --help` | show help, exit `0` |

The operation is the only positional argument and there must be exactly one;
option parsing permutes, so it may appear anywhere. `--` ends option parsing,
and a lone `-` is an ordinary argument. A value may be **attached or separate**:
`--rows 2`, `--rows=2`, and `-r2` are all equivalent.

The integer options (`--rows`, `--cols`, `--precision`) accept `+?[0-9]+` and
nothing else. The spelling is pinned here rather than left to each language's
integer parser, which is what the number set below does for values and for the
same reason: `strtol` skips leading whitespace and `from_chars` rejects a
leading `+`, so `--rows +2` and `--rows " 2"` each worked in exactly one port
until this rule was written down. `+2` is accepted, matching the values
contract; `" 2"` is not, since as an option value it is a typo rather than a
request.

`--precision` is bounded above as well. A double's smallest subnormal is about
`5e-324`, so past ~1074 places every digit printed is a zero the trimming then
removes, and the ceiling is set at `1100`. It is not only cosmetic: a rendering
of a large value needs 309 digits before the point plus the precision after it,
so an uncapped precision lets a single cell demand gigabytes.

#### Known divergence: argument parsers

Each port declares its options to a library and takes what that library gives
back. So the following differ by design and nothing compares them:

| | C (`getopt_long`) | C++ (CLI11) |
|---|---|---|
| `--help` | the hand-written text below | CLI11's rendering |
| unknown option | `error: unknown option '--x'` | `The following argument was not expected: --x` |
| bad-argument exit code | `2` | CLI11's: `104` conversion, `105` validation, `109` extras, `114` missing value |
| `--help=x` | `error: option '--help' does not take a value`, exit 2 | read as a request for help, exit 0 |
| abbreviated long options (`--row`) | accepted when unambiguous | rejected |

**What does not differ is which command lines are accepted**, `--help=x` and
abbreviations aside. Every option value is validated against the contract below
by hand in both ports rather than by the parser: CLI11's own numeric conversion
skips leading whitespace and accepts `nan` and `inf`, so `--rows " 2"` and
`--scalar inf` would otherwise succeed in C++ and stay usage errors in C. The
parity script's `run_case_parser_error` cases assert exactly that — both ports
reject the same input — without comparing how they say so.

`check_parity.sh` only ever asserts that the ports *agree*, so it cannot pin a
difference; the two rows above where they genuinely disagree live here instead.
Same treatment `simple_logger/README.md` gives its own.

#### Known divergence: the Rust port

The Rust port is not in `check_parity.sh`, and the reason is not its
diagnostics — every port's diagnostics are its parser's now. It is the operand
ordering: clap cannot report the order two different options were interleaved
in, so the Rust port pairs dimensions with operands by index. (CLI11 *can*, via
`App::parse_order()`, which is how the C++ port keeps C's rule.) That difference
shows up in stdout, which the script does compare, so including the port would
mean exempting the cases the script exists for.

| | C and C++ | Rust |
|---|---|---|
| dimensions | bind to the *next* operand, in the order typed | the Nth `--rows` describes the Nth operand |
| mixed operand sources | interleaved in the order typed | inline operands ordered before file ones |
| hex floats (`0x1p3`) | accepted, via `strtod` | rejected |
| out of memory | `matrix_ops: out of memory`, exit 1 | the process aborts |

Everything below this section — the shape rules, the value set apart from hex
floats, the output format, the exit codes — holds in all three. Written the way
C requires, with each dimension before the operand it describes, the Rust port
produces byte-identical output; the ordering divergence only shows in spellings
C rejects outright. [`rust/README.md`](rust/README.md) has the reasoning.

### Shape

Dimensions are optional and inferred by default:

1. A single non-blank line of values is a `1 x N` **row vector**; several
   non-blank lines are rows. Blank lines and surrounding whitespace are ignored,
   and a trailing newline is optional.
2. **Rows of differing length are always an error**, even when `--rows`/`--cols`
   would make the layout irrelevant.
3. Then the requested dimensions, if any, are applied:
   - neither given → the layout's own shape
   - **both** given → the values are reshaped row-major, and their count must be
     exactly `rows * cols`
   - **one** given → the other is derived, and it must divide the count evenly

So `--values "1 2 3 4 5 6"` is a `1x6` vector, `--rows 2 --cols 3` over the same
values is a `2x3` matrix, and `--rows 2` alone gets there too.

### Values

A value is anything the platform's `strtod` accepts — sign, decimal point,
exponent — **except** `nan`, `inf`, `infinity`, and anything that overflows to
infinity, all of which are errors. A value that underflows to zero is accepted.
An embedded NUL byte in a file is an error rather than a silent truncation.

`strtod` also accepts C99 hex floats (`0x1p3`), which Rust's `f64::from_str`
does not, so the Rust port rejects them. That is the one place the accepted
number set is not identical across the three, and it is the cost of not
hand-rolling a hex-float parser for a syntax nobody types into a matrix.

### Output

One row per line, each line ending in a newline including the last. Every element
is rendered with a fixed number of decimals and then stripped of trailing zeros
and a bare trailing `.`, so integral values print as integers. The widest
rendering sets a common column width; all elements are right-justified into it
and separated by two spaces, so decimal points line up.

- Scientific notation is never used, at any magnitude. The column width follows
  the data, so a matrix holding `1e300` prints a very wide column.
- A negative zero always prints as `0`, whether it is the double `-0.0` or a
  small negative value the precision rounded away.
- Rounding is the ties-to-even rule `printf("%.*f")` and Rust's formatting share:
  `0.25` at one decimal is `0.2`, not `0.3`.

### Exit codes

`0` success, `2` usage error (unknown operation, wrong operand count, bad or
ragged shape, a bad number inside an operand, dimensions with no operand to
attach to), `1` operational error (a file that cannot be opened or read, a
failed write, out of memory).

These are the codes for what the *program* reports. A malformed command line —
an unknown option, a missing value, an option value the contract rejects — is
reported by the parser and carries the parser's code: `2` in the C port, which
suppresses `getopt_long`'s own diagnostics and reports them itself, and one of
CLI11's in the C++ port. See
[Known divergence: argument parsers](#known-divergence-argument-parsers).

Out of memory is an exit code and a message — `matrix_ops: out of memory` —
rather than a crash, in every port. For the C++ one that means an allocation
failure has to be caught rather than left to `std::terminate`, which is what
`err_oom_file` and `err_oom_stdin` in `check_parity.sh` run both ports under a
`ulimit -v` to check.

## Comparison against Eigen and xtensor

Reproduce with `./bench/run.sh`, which builds optimized, checks that all three
implementations agree elementwise, and prints both tables below. The
correctness check gates the timings — a benchmark comparing three different
answers is worse than none.

All three run on the same row-major `double` data. Eigen defaults to
column-major and is pinned to row-major here so the layouts match; otherwise
the fastest library would also be the one measured on a different memory
layout.

### Speed

`mul` is the whole story; the elementwise operations are memory-bound and land
within ~3x of each other everywhere. Measured on a Ryzen 7 9700X (Zen 5, 8
cores / 16 threads, AVX-512).

Two variables have to be separated before any of these numbers mean anything,
and getting either wrong silently decides the winner:

- **Instruction set.** OpenBLAS ships precompiled assembly kernels for dozens of
  microarchitectures and picks one at runtime via CPUID. It gets AVX-512 here
  *no matter what flags we pass*. Eigen only gets what we compile it with, and
  `--config=opt` alone means baseline `x86-64` — SSE2, two doubles per vector,
  no FMA.
- **Threading.** OpenBLAS threads by default. Eigen is header-only, so there is
  no "Eigen built with OpenMP" to depend on — its GEMM is guarded by
  `#ifdef EIGEN_HAS_OPENMP`, and compiling *our own* target with `-fopenmp` is
  the whole of enabling it. `bench/BUILD` now does.

**1024x1024 `mul`:**

| build | ours | Eigen | xtensor + OpenBLAS | Eigen vs OpenBLAS |
|---|---|---|---|---|
| baseline ISA, 1 thread | 3067 ms | 66.8 ms | 16.6 ms | **4.0x** |
| `-march=native`, 1 thread | 3077 ms | **21.1 ms** | 16.7 ms | **1.26x** |
| `-march=native`, 16 threads | ~3200 ms | **2.93 ms** | 2.6–6.1 ms | **~1.0x** (0.9–2.1x run to run) |

So the honest decomposition of the original "OpenBLAS is 4x Eigen":

- **3.2x of it was our compiler flags.** Giving Eigen the instruction set the
  CPU actually has takes it from 66.8 ms to 21.1 ms and closes most of the gap.
  This is the single biggest lever here and it costs one flag.
- **~1.25x is genuine kernel quality** — hand-written assembly micro-kernels and
  per-CPU-tuned blocking and packing, versus Eigen's portable C++ templates.
  Real, but far smaller than the flags made it look.
- **Threading is worth ~7x to each of them, and neither wins it.** With
  `-fopenmp`, Eigen goes 21.1 → 2.93 ms; OpenBLAS lands in the same band, 2.6 to
  6.1 ms across five runs. That spread is wider than the difference between the
  two libraries, which is the honest summary of this row — it says they are the
  same speed and not much more. The apparent 5.7x OpenBLAS lead in the earlier
  version of this table was entirely "we forgot to give Eigen any threads".

Against our own implementation, Eigen is **146x** at matched ISA and **1100x**
with threads.

#### Caveat: the threaded numbers are the least trustworthy here

How untrustworthy is measurable. Five runs of the 1024x1024 `mul`, same binary,
same machine, nothing else changed: xtensor came in at 2.6, 3.6, 4.3, 4.6, and
6.1 ms, and Eigen's column in that same unbound table — the one you are told to
ignore — swung from 3.0 to 42.1 ms. The single-threaded tables repeat to within
a few percent.

Both libraries thread, and their pools do not compose inside one process:

- **OpenBLAS's idle threads busy-wait** for a long timeout, so they are still
  burning cores while Eigen is being measured. `compare.cpp` pins the idle
  library to one thread via `Eigen::setNbThreads` / `openblas_set_num_threads`,
  which helps but cannot fully settle a pool that is already spinning.
- **`OMP_PROC_BIND=true` is what makes Eigen's numbers stable** — without it the
  OS migrates its threads and the 256x256 result swings by 20x — **but it binds
  the master thread**, and OpenBLAS's pthread pool inherits that affinity mask
  and collapses onto a single core.

Every setting that helps one library handicaps the other. `bench/run.sh`
therefore prints two threaded tables, each configured in one library's favour,
and you read only that library's column from each. Measuring this properly would
need one process per library. The single-threaded tables have none of this
problem and are the ones to trust.

**Our naive loop gets nothing from `-march=native`** (3067 → 3443 ms, i.e. noise
in the wrong direction). That is not a compiler failure: the inner loop is
`sum += a.at(i,k) * b.at(k,j)`, which strides through `b` by a whole row per
iteration. No instruction set fixes a memory access pattern — you have to block
the loops first, which is exactly what Eigen and OpenBLAS spend their complexity
on. The 150x is cache blocking, not SIMD.

### Cost

| | ours | Eigen | xtensor + xtensor-blas |
|---|---|---|---|
| Compile time, one TU | 0.10 s | 0.88 s | 1.37 s |
| Preprocessed lines | 33k | 147k | 281k |
| Bazel integration | — | one `bazel_dep` | `bazel_dep` + `http_archive` + a BUILD file we maintain + a system BLAS |
| Hermetic build | yes | yes | **no** |

### Ergonomics

For the four operations here, both libraries are a clear improvement on writing
it yourself:

```cpp
ours:     matrix_ops::add(a, b)          // -> std::optional<Matrix>
Eigen:    a + b
xtensor:  a + b

ours:     matrix_ops::mul(a, b)
Eigen:    a * b
xtensor:  xt::linalg::dot(a, b)          // needs the separate xtensor-blas
```

Eigen and xtensor both use expression templates, so `a + b + c` fuses into one
pass with no temporaries — something our version cannot express at all, since
every operation materializes its result.

The signature difference is real, though. `a + b` on mismatched shapes is
undefined behavior in Eigen (an assertion in debug builds, nothing in release);
`matrix_ops::add` returns `std::nullopt`. For a CLI taking arbitrary user input,
the checked version is what you want, and a library wrapper would have to add
the check back.

### What actually bit us

Three findings that matter more than the timings if you are choosing a library:

1. **xtensor's core package has no matrix multiply.** `xt::linalg::dot` lives in
   **xtensor-blas**, which is *not in the Bazel Central Registry* — it needs an
   `http_archive` and a BUILD file you maintain — and which needs a system BLAS.
   Neither OpenBLAS nor any other BLAS is in the registry either. Multiplying two
   matrices is the reason most people reach for a linear algebra library, and for
   xtensor it is the part that is not packaged.

2. **xtensor-blas silently falls back to a 50x-slower path.** Its
   `xblas_config.hpp` does `#define HAVE_CBLAS 1`, but the vendored cxxblas
   headers are preprocessed *before* that header is reached, so every
   `#ifdef HAVE_CBLAS` block — including the `cblas_dgemm` overloads — is
   skipped. cxxblas then uses its own generic C++ gemm. It still gives the right
   answer, `ldd` still shows `libopenblas.so` because it is linked, and nothing
   warns you. Our first run measured 3264 ms and looked like a plausible result
   for a library that "wasn't as fast as Eigen"; it was actually our own naive
   loop's speed, because that is essentially what was running. Upstream's CMake
   passes `-DHAVE_CBLAS` on the command line and never hits this. The fix is in
   `//third_party/xtensor_blas.BUILD`, and `bench/run.sh` now fails outright if
   the binary references no `cblas_*` symbols.

3. **Third-party headers are not warning-clean.** This repo builds with
   `-Wall -Werror -Wextra -pedantic`, which by default applies to external
   dependencies too. Eigen escapes it by accident — its BUILD uses `includes`,
   which yields `-isystem` — but xtensor and the FLENS tree do not.
   `build --features=external_include_paths` in `.bazelrc` fixes this properly
   by making all external headers `-isystem`; weakening the warnings repo-wide
   would have been the wrong trade.

### Would we use them?

**Eigen: yes, easily.** One `bazel_dep`, no build friction, no system
dependencies, warning-clean out of the box, and 146x on the operation that
matters — 46x if you leave `-march` alone, which is the same 3.2x flag story as
everywhere else on this page. Header-only and hermetic.

**xtensor: only if you specifically want its NumPy-shaped n-dimensional API.**
At matched flags and one thread its lead is 1.26x, and with both libraries
threaded it disappears entirely — Eigen with `-fopenmp` matches OpenBLAS. The
speed was OpenBLAS's rather than xtensor's in the first place. Getting to it cost
an
out-of-registry package, a hand-written BUILD file, a non-hermetic system
dependency, the slowest compile times of the three, and a silent-fallback bug
that made the benchmark wrong until the symbol table was checked. If you want
fast dense linear algebra in C++ and not tensors specifically, Eigen gets you
there with none of that. If you later need OpenBLAS's kernels anyway, Eigen can
call them directly — the registry's BUILD already exposes `EIGEN_USE_BLAS`
through an `@eigen//:use_blas` flag — which is a far shorter path than adopting
xtensor to get at them.

**And before reaching for either: check your `-march`.** A 3.3x speedup from one
compiler flag outranks the entire library choice at this problem size. The
repo's `--config=opt` deliberately does not set it, since `-march=native`
produces binaries that do not run on other machines; `--config=native` is
available for benchmarks.

## Comparison against faer and nalgebra

The Rust counterpart of the section above. Reproduce with `./bench/rust/run.sh`,
which runs the agreement test first — a disagreement stops the run before any
timing is printed — and then benchmarks the same four operations at the same
sizes as `bench/compare.cpp`, so the two sets of tables describe the same work.

Two differences in method, both worth knowing before comparing a number here
against one above:

- **These are criterion means, not best-of-5.** criterion's default sampling
  runs 1, 2, 3, … N iterations per sample, which is fine at microseconds and
  absurd for the ~3-second naive multiply, so anything over a millisecond uses
  flat sampling at criterion's minimum sample size of 10.
- **faer and nalgebra are both column-major**, where Eigen was pinned to
  row-major to match. Converting per element rather than reinterpreting the
  buffer is what keeps this honest; see the note in `CLAUDE.md`.

**1024x1024 `mul`**, on the same Ryzen 7 9700X:

| build | ours | faer | nalgebra |
|---|---|---|---|
| baseline ISA, 1 thread | 2993 ms | 13.95 ms | 15.93 ms |
| `-C target-cpu=native`, 1 thread | 2554 ms | **13.94 ms** | 16.23 ms |
| `-C target-cpu=native`, 16 threads | 2581 ms | **4.28 ms** | 16.27 ms |

### The instruction-set lever does not exist here

This is the sharpest contrast with the C++ results. There, the single biggest
finding was that one compiler flag was worth 3.2x to Eigen — more than the
library choice itself. In Rust that lever is simply absent:

- faer: 13.95 → 13.94 ms. **No change at all.**
- nalgebra: 15.93 → 16.23 ms. No change.
- ours: 2993 → 2554 ms, about 1.17x — the only column the flag moves.

Both libraries ship runtime-dispatched kernels and pick one by CPU feature
detection, the way OpenBLAS does and the way header-only Eigen cannot. So the
advice that closes the C++ section — *check your `-march` before reaching for a
library* — inverts here: the flag helps only code you wrote yourself, and the
libraries were already using AVX-512 before you asked.

It also means faer at the **default** `--release` build (13.95 ms) is already
faster than Eigen at `-march=native` (21.1 ms) and level with xtensor's OpenBLAS
(16.6 ms). No flags, no system dependency, one `cargo add`.

### Threading is a smaller win, and below 1024 it is a loss

faer threads through rayon; nalgebra does not thread at all, which the flat
nalgebra column across all three tables confirms rather than assumes.

faer gets 3.3x from 16 threads (13.94 → 4.28 ms), against the 7.2x Eigen got
from OpenMP. But the more useful finding is at the small end, where threading
actively hurts:

| `mul` | 1 thread | 16 threads | |
|---|---|---|---|
| 64x64 | 0.004 ms | 0.527 ms | **132x worse** |
| 256x256 | 0.223 ms | 0.592 ms | 2.7x worse |
| 1024x1024 | 13.94 ms | 4.28 ms | 3.3x better |

Below roughly 1024x1024 the pool dispatch costs more than the work. faer
defaults to threading, so a program doing many small multiplies gets the bad
end of this without asking for it — `faer::set_global_parallelism(Par::Seq)` is
worth reaching for well before the sizes where it stops mattering.

### Our naive loop, across languages

The same `i`/`j`/`k` triple loop, deliberately kept identical so the two
benchmarks compare the same algorithm:

| | baseline ISA | native ISA |
|---|---|---|
| C++ (`bench/compare.cpp`) | 3067 ms | 3077 ms |
| Rust (`bench/rust`) | 2993 ms | 2554 ms |

Rust is 1.2x faster at native ISA, *with* bounds checks on every element access
that the C++ version does not have. LLVM gets something out of AVX-512 here that
GCC does not, despite the loop-carried dependency on `sum` that should defeat
vectorization in both. Not a large enough gap to draw conclusions from, but it
is the opposite of the direction the bounds checks would predict.

### Would we use them?

**faer: yes.** Fastest of everything measured on this page at the default build,
hermetic, pure Rust, one `cargo add`, and the parallelism control is a single
global call. The API needed no wrestling — `Mat::from_fn`, the arithmetic
operators, and `Scale` compiled first try. Two cautions: it is pre-1.0 and has
reorganized its API within recent minor versions, so pin it; and its default
threading is a pessimization at small sizes.

**nalgebra: yes, for anything that is not a large GEMM.** Never worse than 1.5x
faer on the elementwise operations, the most familiar API of the three, and the
only one here with no threading behavior to think about. At 1024x1024 `mul` it is
only 1.16x behind faer single-threaded — but 3.8x behind once faer uses the
machine, since nalgebra cannot. If large dense products are the workload, that
is the whole decision; if they are not, the gap never shows up.

**And our own implementation: no, same as in C++.** 183x behind faer at matched
flags, 603x with threads on. The naive triple loop is worth keeping here because
it is the thing being measured, not because anyone should ship it.
