# memory optimization

Small C++ programs that demonstrate the programmer-facing techniques from
**Section 6, "What Programmers Can Do"**, of Ulrich Drepper's *What Every
Programmer Should Know About Memory* (2007). Each subdirectory reproduces one
example or figure from the paper: a `cc_library` with the technique's variants, a
GoogleTest that pins the fast variant to the same result as the naive one, and a
[Google Benchmark](https://github.com/google/benchmark) executable that measures
the effect. The paper reports most results in CPU cycles, so the benches that
match a paper table attach a cycle counter (via `support/rdtsc.hpp`) alongside
Google Benchmark's wall-clock time.

The goal is to *see the paper's numbers reproduce on real hardware*, in modern
C++20 rather than the paper's 2007 C. Where the modern idiom differs from the
paper's code (`alignas` for `__attribute__((aligned))`, `[[unlikely]]` for
`__builtin_expect`, `std::atomic` for `__sync_*`, `std::jthread` for pthreads),
the divergence is flagged in a `// paper:` comment at the site and summarised in
each project's README.

## Scope

Covered (everything programmer-controllable in Section 6 except NUMA):

| Dir | § | Technique |
|-----|---|-----------|
| [`bypassing_cache/`](bypassing_cache/) | 6.1 | Non-temporal / streaming stores |
| [`matrix_multiply/`](matrix_multiply/) | 6.2.1 | Loop transpose and blocking (the paper's flagship example) |
| [`data_layout/`](data_layout/) | 6.2.1 | Hot/cold struct splitting and alignment (with `pahole`) |
| [`conflict_misses/`](conflict_misses/) | 6.2.1 | Cache associativity and conflict misses |
| [`instruction_cache/`](instruction_cache/) | 6.2.2 | Inlining and branch hints |
| [`working_set/`](working_set/) | 6.2.3 | Matching the working set to cache size |
| [`tlb_usage/`](tlb_usage/) | 6.2.4 | Reducing TLB pressure with huge pages |
| [`software_prefetch/`](software_prefetch/) | 6.3.2 | `_mm_prefetch` on a pointer chase |
| [`false_sharing/`](false_sharing/) | 6.4.1 | Concurrent cache-line contention |
| [`atomicity/`](atomicity/) | 6.4.2 | Atomic increment and CAS |

Deliberately **out of scope**: NUMA (§6.5) and the thread-affinity parts of
§6.4.3, per the exercise brief; and the two techniques that are not
programmer-demonstrable in portable code — IA-64 speculative-load prefetch
(§6.3.3) and hardware Direct Cache Access (§6.3.5).

`support/` holds two shared helpers: `cacheinfo` (cache sizes and line size via
`sysconf`, the paper's §6.2.3 advice to query rather than hardcode) and `rdtsc`
(a header-only timestamp-counter read).

## Commands

```sh
# Build and test everything (correctness gtests -- run at any optimization level)
bazel test //memory_optimization/...

# Run a benchmark. ALWAYS pass -c opt: the default fastbuild is -O0 and the
# numbers are meaningless without optimization.
bazel run -c opt //memory_optimization/matrix_multiply:bench_matmul
bazel run -c opt //memory_optimization/false_sharing:bench_false_sharing
# ... one bench_<name> target per project.

# Optionally add --config=native to compile for this exact CPU (AVX etc.). Not
# needed to reproduce any result here -- the paper's intrinsics are all SSE2,
# which is x86-64 baseline.
```

Timing runs are noisy under CPU frequency scaling (Google Benchmark warns about
it). For steady numbers, pin the frequency or just read the ratios between
variants, which are stable — that is what the paper reports too.

## Reproduced results (AMD Ryzen 7 9700X, Zen 5)

Every project's README has its own table; the headline reproductions:

- **matrix_multiply** (N=1024): naive 3162 ms → transposed 389 ms (**8×**),
  blocking level with transpose at this scale.
- **bypassing_cache**: sequential non-temporal store as fast as normal (~3 ms),
  column-order non-temporal much worse (23 ms) — Table 6.1's shape.
- **software_prefetch**: past L3, plain chase 382 cyc/element vs prefetch 63 —
  a **6×** latency-hiding win (Figure 6.7).
- **false_sharing**: 8 threads, packed 656 ms vs padded 84 ms; padded throughput
  scales linearly (Figure 6.10).
- **conflict_misses**: 2.8 cyc/element at benign strides, 9.8 at a 4096-byte
  power-of-two stride (Figure 6.5).

Modern hardware sometimes shows the effect *more* strongly than the 2007 paper
(the memory wall grew) and sometimes *less* (sequential bandwidth and branch
prediction improved a lot); each README notes which and why.

## Conventions

Standard repo rules apply (see the root [`CLAUDE.md`](../CLAUDE.md)): Bazel with
`-Wall -Werror -Wextra -pedantic` and `-std=c++20`, `cc_library` + `cc_test`
(GoogleTest) + `cc_binary`. The one new third-party dependency is
`google_benchmark`, declared in the root `MODULE.bazel` and used only by the
`bench_*` binaries here.
