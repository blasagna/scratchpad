# false sharing

Reproduces **§6.4.1, "Concurrency Optimizations"** (Figure 6.10): the cost of
several threads writing independent variables that happen to share a cache line.

## What the paper shows

Threads share an address space, so their data can land on the same cache line
even when logically independent. Every write needs the line in the exclusive
('E') state, so if two cores each write their own variable on one line, the line
ping-pongs between them: each write sends a Request-For-Ownership to steal the
line back. The threads serialize on the cache coherence protocol despite touching
different bytes — **false sharing**.

The paper measures N threads each incrementing a counter 500M times, with the
counters packed onto one line versus spread one per line. Packed is several times
slower and gets worse with more threads; spread scales cleanly. The fix (the
paper's `rwstruct`) pads each per-thread datum out to a full cache line.

## This demo

Each thread hammers its own counter; the counters are either `Packed` (adjacent,
sharing lines) or `Padded` (`alignas(kCacheLine)`, one per line). `run()` in
[`false_sharing.hpp`](false_sharing.hpp) releases the threads together with a
`std::latch` and returns the exact total.

```sh
bazel run -c opt //memory_optimization/false_sharing:bench_false_sharing
```

## Results on this machine (20M increments/thread; total wall time)

| threads | packed | padded |
|---|---|---|
| 1 | 74 ms  | 74 ms |
| 2 | 220 ms | 74 ms |
| 4 | 416 ms | 74 ms |
| 8 | 656 ms | 84 ms |

Textbook Figure 6.10: packed time climbs steeply with thread count while padded
stays flat, and padded throughput scales linearly (269 M/s → 1.9 G/s from 1 to 8
threads). The whole slowdown is coherence traffic — no thread touches another's
bytes.

## Modern C++ vs the paper

- Padding is `alignas(kCacheLine)` instead of `__attribute__((aligned(CLSIZE)))`.
  `kCacheLine` is a pinned `constexpr 64` rather than
  `std::hardware_destructive_interference_size`, which trips GCC's
  `-Winterference-size` under the repo's `-Werror`; the test checks the constant
  covers the real line size from `sysconf`.
- Threads are `std::jthread` + `std::latch`. Each counter has a single writer, so
  the increment uses `std::atomic_ref` with `relaxed` ordering only to force a
  real per-iteration memory access (not for cross-thread atomicity — that is the
  [`atomicity`](../atomicity/) demo). We do not pin threads to cores; affinity is
  §6.4.3, which is out of scope.
