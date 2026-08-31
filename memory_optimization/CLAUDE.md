# memory_optimization

C++ demos of the programmer-facing cache and memory techniques in **Section 6**
of Drepper's *What Every Programmer Should Know About Memory*. Each subdirectory
is a self-contained project reproducing one paper example: a `cc_library` of
variants, a GoogleTest pinning correctness, and a Google Benchmark executable.
The full narrative and the scope decisions (NUMA and non-portable techniques are
excluded) are in [`README.md`](README.md); each project has its own README with
the paper's context, the measured results, and the modern-C++ divergences.

## Commands

```sh
bazel test //memory_optimization/...                                   # all correctness gtests
bazel test //memory_optimization/matrix_multiply:test_matmul           # one project's test

# Benchmarks -- ALWAYS -c opt (default fastbuild is -O0, so timings are junk):
bazel run -c opt //memory_optimization/<project>:bench_<name>
# e.g.
bazel run -c opt //memory_optimization/software_prefetch:bench_software_prefetch
bazel run -c opt //memory_optimization/false_sharing:bench_false_sharing

bazel run -c opt --config=native //memory_optimization/...:bench_...    # compile for this CPU (optional)
```

Target naming per project: library `<name>`, test `test_<name>`, benchmark
`bench_<name>`. `support/` exposes `cacheinfo` and header-only `rdtsc`, visible
to `//memory_optimization:__subpackages__` only.

## Shared behavior (keep in mind across projects)

- **Google Benchmark is the timing harness**, declared once as
  `google_benchmark` in the root [`MODULE.bazel`](../MODULE.bazel). Bench
  binaries link `@google_benchmark//:benchmark_main`, *except* `working_set`,
  which registers its sweep at runtime and so provides its own `main()` and links
  `@google_benchmark//:benchmark`.
- **The paper reports cycles; we report both.** Benches matching a paper table
  read the timestamp counter (`support/rdtsc.hpp`, the `__rdtsc()` intrinsic) and
  expose a `cycles*` counter next to Google Benchmark's wall time.
- **Modern C++20, with the paper's idiom noted at the site.** `alignas` +
  a pinned `kCacheLine = 64` (not `std::hardware_destructive_interference_size`,
  which trips `-Winterference-size` under `-Werror`), `[[likely]]`/`[[unlikely]]`,
  `[[gnu::always_inline]]`/`[[gnu::noinline]]`, `std::atomic`/`std::atomic_ref`,
  `std::jthread` + `std::latch`. The two intrinsics with no standard equivalent —
  `_mm_stream_si64` (non-temporal store) and `_mm_prefetch` — stay as intrinsics.
- **Intrinsics are SSE2, i.e. x86-64 baseline.** No `--config=native` is needed
  to reproduce anything; `-c opt` is enough. This is an AMD box, and every
  intrinsic used (`_mm_stream_si64`, `_mm_prefetch`, `_mm_sfence`) is fine on it.
- **Correctness tests run at any optimization level and never measure time.**
  They assert the cache-friendly variant equals the naive one (matmul), the total
  is exact (false_sharing, atomicity), or the structure is a valid cycle
  (conflict_misses, software_prefetch, tlb_usage).

## Gotchas

- **`pahole`, `perf`, and `valgrind` are not installed here** (valgrind is a
  pixi-managed tool for the Bazel areas; see the root [`README.md`](../README.md)).
  The `data_layout` README documents running `pahole` on the built binary to see
  struct/cache-line layout, and several READMEs point at
  `valgrind --tool=cachegrind` (the paper's §7 tool) for cache-miss counts — but
  no demo *requires* any of them. Everything runs and passes on the runtime
  benchmark alone.
- **Timing needs `-c opt`.** Bench binaries built at the default `fastbuild` are
  `-O0` and the loops the compiler would otherwise vectorize or hoist dominate the
  measurement. Every bench file's header comment repeats this.
- **Threaded demos scale with `nproc`.** `false_sharing` and `atomicity` sweep
  1..8 threads; this box has 8 cores / 16 threads. Their libraries carry
  `linkopts = ["-pthread"]`.
- **`tlb_usage` depends on transparent huge pages** being enabled
  (`/sys/kernel/mm/transparent_hugepage/enabled`). If the kernel does not promote
  the region to 2 MiB pages, the two curves converge — the README says so, and the
  demo still runs.
- **CPU frequency scaling makes absolute times noisy** (Google Benchmark warns).
  The ratios between variants are what reproduce the paper and are stable.

Strict warnings, C++20, and formatting (`pixi run fmt`) are repo-wide conventions
from the root [`CLAUDE.md`](../CLAUDE.md).
