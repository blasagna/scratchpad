# matrix operations

A mini project from the little book of c.

Build a library, tests, and a CLI program that implements basic operations on 2D matrices.

Requirements:
1. take arguments for the matrix dimensions (rows and columns)
1. read matrix values from arguments or from a file
1. perform the requested matrix operation: addition, subtraction, multiplication, scalar multiplication
1. display results neatly formatted to stdout

## Contract

The behavior all ports implement. `c/` and `cpp/` exist so far and are held to
this byte-for-byte by `check_parity.sh` (56 cases, comparing stdout, stderr, and
exit status). Rust follows.

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
| `-p, --precision N` | decimal places in the output, `N >= 0` (default `4`) |
| `-h, --help` | show help, exit `0` |

The operation is the only positional argument and there must be exactly one;
option parsing permutes, so it may appear anywhere. `--` ends option parsing,
and a lone `-` is an ordinary argument. A value may be **attached or separate**:
`--rows 2`, `--rows=2`, and `-r2` are all equivalent.

#### Known divergence: abbreviated long options

The C port gets its parsing from `getopt_long`, which accepts any *unambiguous
prefix* of a long option — `--row` for `--rows`, `--val` for `--values`. The C++
port's hand-rolled loop matches full names only and reports `error: unknown
option '--row'`.

This is deliberate: reproducing the abbreviation rule also means reproducing
`getopt`'s ambiguity diagnostics (`option '--r' is ambiguous; possibilities:
…`), which is more machinery than the feature earns. **The supported surface is
the intersection — spell long options in full.** `check_parity.sh` only asserts
that the ports agree, so it cannot pin a difference and has no case for this;
the divergence lives here instead. Same treatment `simple_logger/README.md`
gives its own argument-parsing divergences.

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

`0` success, `2` usage error (unknown operation or option, wrong operand count,
bad number, bad or ragged shape, dimensions with no operand to attach to), `1`
operational error (a file that cannot be opened or read, a failed write, out of
memory).

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
| `-march=native`, 16 threads | ~3200 ms | **2.93 ms** | ~2.1–3.5 ms | **~1.0x** |

So the honest decomposition of the original "OpenBLAS is 4x Eigen":

- **3.2x of it was our compiler flags.** Giving Eigen the instruction set the
  CPU actually has takes it from 66.8 ms to 21.1 ms and closes most of the gap.
  This is the single biggest lever here and it costs one flag.
- **~1.25x is genuine kernel quality** — hand-written assembly micro-kernels and
  per-CPU-tuned blocking and packing, versus Eigen's portable C++ templates.
  Real, but far smaller than the flags made it look.
- **Threading is worth ~7x to each of them, and neither wins it.** With
  `-fopenmp`, Eigen goes 21.1 → 2.93 ms; OpenBLAS lands in the same 2–3.5 ms
  band. The apparent 5.7x OpenBLAS lead in the earlier version of this table was
  entirely "we forgot to give Eigen any threads".

Against our own implementation, Eigen is **146x** at matched ISA and **1100x**
with threads.

#### Caveat: the threaded numbers are the least trustworthy here

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
dependencies, warning-clean out of the box, and 46x on the operation that
matters. Header-only and hermetic.

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
