# working set

Reproduces the message of **§6.2.3, "Optimizing Level 2 and Higher Cache
Access"**: keep the working set within a cache when data is reused, and know the
cache sizes at runtime instead of hardcoding them.

## What the paper shows

Every cache level has a size; as a reused working set grows past L1d, then L2,
then L3, the average access cost steps up at each boundary and the achievable
bandwidth drops. For code that touches its data more than once, the job is to
work in chunks that fit — the same idea as the blocked matrix multiply, applied
to the larger caches. The paper stresses reading the real sizes with
`sysconf`/`getconf` rather than baking in constants, since they vary widely across
machines.

## This demo

`sum_repeatedly` streams a reused buffer; the benchmark sweeps the buffer size
from 4 KiB to 256 MiB and labels each row with the smallest cache that holds it,
using `support::query_cache_info()`. The detected sizes are printed as benchmark
context.

```sh
bazel run -c opt //memory_optimization/working_set:bench_working_set
getconf -a | grep -i cache        # compare against what the sweep detected
```

## Results on this machine (Ryzen 7 9700X: L1d 48 KiB, L2 1 MiB, L3 32 MiB)

The per-row label transitions L1d → L2 → L3 → RAM at exactly the sysconf sizes.
The achieved bandwidth holds ~40 GiB/s through L3 and drops to ~34 GiB/s once the
buffer exceeds 32 MiB (L3 → RAM).

That last drop is the honest headline: for a **sequential** stream the cliffs at
L1/L2 are shallow, because the hardware prefetcher hides most of the latency and
the loop is bandwidth-bound, not latency-bound. The sharp per-level cliffs the
paper draws show up under **random** access — which is exactly what the
[`conflict_misses`](../conflict_misses/) and [`software_prefetch`](../software_prefetch/)
demos exhibit (their pointer chases jump from single-digit to hundreds of cycles
per element across the same boundaries). This demo's real payload is the
*runtime cache-size query* and the L3→RAM bandwidth wall.

## Modern C++ vs the paper

- Sizes come from `sysconf(_SC_LEVEL*_CACHE_SIZE)` wrapped in `support/cacheinfo`
  — the paper's `getconf`/`sysconf` advice, made a reusable helper.
- This bench registers its size sweep at runtime, so it provides its own `main()`
  and links `@google_benchmark//:benchmark` (not `:benchmark_main`).
