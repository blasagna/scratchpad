# atomicity

Reproduces **§6.4.2, "Atomicity Optimizations"** (Figure 6.12): incrementing a
shared counter correctly under contention, and what it costs.

## What the paper shows

When several threads modify the same location, the processor guarantees nothing
unless atomic operations are used. A plain read-modify-write can read a stale
value between another thread's read and write, and an update is lost. Processors
provide atomic primitives instead — the paper lists bit-test, load-locked/
store-conditional, compare-and-swap (CAS), and (on x86/x86-64) atomic arithmetic.
It shows three shapes of atomic increment: add-and-fetch, fetch-and-add, and a
CAS retry loop. x86-64 can do the increment with a single locked instruction,
while a CAS loop is the general form that works on any architecture — but the CAS
loop re-runs its body every time it loses a race, so it costs more under
contention.

## This demo

`run()` increments one shared counter from many threads three ways
([`atomicity.hpp`](atomicity.hpp)):

- `kFetchAdd` — `std::atomic::fetch_add` (a single `lock xadd` on x86-64).
- `kCasLoop` — a `compare_exchange_weak` retry loop (the portable primitive).
- `kPlainUnsafe` — a non-atomic read-modify-write (separate relaxed atomic
  load + store, so it is defined C++ rather than a UB data race) that *loses*
  updates under contention.

```sh
bazel run -c opt //memory_optimization/atomicity:bench_atomicity
```

## Results on this machine (5M increments/thread, one shared counter)

| threads | fetch_add | CAS loop |
|---|---|---|
| 1 | 18.8 ms | 19.0 ms |
| 2 | 54 ms   | 137 ms  |
| 4 | 109 ms  | 448 ms  |
| 8 | 232 ms  | 1033 ms |

Uncontended, the two are identical. Under contention `fetch_add` degrades
gracefully (throughput 265 → 172 M/s from 1 to 8 threads) while the CAS loop
collapses (264 → 39 M/s), because every lost race forces a retry — the paper's
argument for preferring the single locked add where the hardware offers it.

The correctness test confirms `fetch_add` and the CAS loop are always exact, and
that `kPlainUnsafe` loses updates once threads contend (it is exact with one
thread).

## Modern C++ vs the paper

- `std::atomic<uint64_t>::fetch_add` and `compare_exchange_weak` with explicit
  `std::memory_order_relaxed` replace the paper's gcc `__sync_*` builtins.
  `compare_exchange_weak` refreshes its `expected` argument on failure, so the
  retry loop needs no manual reload.
- Threads are `std::jthread` + `std::latch` (released together to maximise the
  contended window).
