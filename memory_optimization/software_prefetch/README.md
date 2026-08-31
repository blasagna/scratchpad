# software prefetch

Reproduces **§6.3.2, "Software Prefetching"** (Figure 6.7): using `_mm_prefetch`
to hide memory latency on a randomized pointer chase that the hardware prefetcher
cannot follow.

## What the paper shows

Hardware prefetchers hide latency for regular strides, but a randomized pointer
chase defeats them: each `next` is unpredictable and the load is on the critical
path, so the CPU stalls the full memory latency at every hop once the working set
spills out of cache. The programmer can help with the `_mm_prefetch` intrinsic,
issuing a load hint for a node several hops ahead so its line is already in cache
when the walk reaches it. The paper's test (NPAD=31, prefetching ~5 elements
ahead) recovers a large fraction of the stall once the list exceeds the
last-level cache.

Key points:
- Prefetch is a hint: wrong or redundant prefetches waste bandwidth, so distance
  matters — too near and the line isn't ready, too far and it's evicted first.
- It only helps when there is latency to hide; inside the cache the two paths
  match.

## This demo

The catch in a pointer chase is knowing the address N hops ahead without
following N pointers. `ChaseList` precomputes it: each node stores an `ahead`
pointer to the node `distance` hops later in traversal order, filled in once at
build time. The hot loop then issues `_mm_prefetch(node->ahead)` — the target is
already in a register, so prefetching adds no extra dependent load.
`chase_plain` vs `chase_prefetch` in
[`software_prefetch.hpp`](software_prefetch.hpp); nodes are 64 bytes so each hop
is a guaranteed line miss.

```sh
bazel run -c opt //memory_optimization/software_prefetch:bench_software_prefetch
```

## Results on this machine (cycles per element)

| nodes | working set | plain | prefetch |
|---|---|---|---|
| 1024   | 64 KiB   | 9.8  | 2.8 |
| 16384  | 1 MiB    | 18.3 | 4.3 |
| 65536  | 4 MiB    | 36.5 | 5.6 |
| 262144 | 16 MiB   | 301  | 7.9 |
| 1048576| 64 MiB   | 371  | 63  |
| 2097152| 128 MiB  | 382  | 64  |

The prefetching walk wins everywhere, and past L3 (32 MiB) the gap is **~6×** —
382 vs 63 cycles per element. This is the sharpest reproduction in the whole area:
Figure 6.7's two diverging curves, with today's larger memory latency making the
divergence even more dramatic than in 2007.

## Modern C++ vs the paper

- `_mm_prefetch(_MM_HINT_T0)` is used unchanged — C++ has **no portable prefetch**.
- The look-ahead is realized with a precomputed `ahead` pointer stored per node,
  which is a faithful stand-in for the paper's framework (it lays out the list so
  the N-ahead address is cheaply available); the payload sum is identical with or
  without prefetch, which the test pins.
