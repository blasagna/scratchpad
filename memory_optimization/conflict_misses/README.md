# conflict misses

Reproduces **§6.2.1**'s cache-associativity discussion (Figure 6.5, "Cache
Associativity Effects"): how a power-of-two data stride triggers conflict misses.

## What the paper shows

An L1d cache is set-associative: an address maps to exactly one *set* by its
middle bits, and only `associativity` lines can live in a set at once. If many
hot addresses share the same set, they evict each other even while the rest of
the cache sits idle — a **conflict miss**. The classic trigger is a power-of-two
layout: an array of power-of-two-sized structs, or one field at a fixed offset
across many objects, sends address after address into the same few sets.

The paper chases a linked list whose nodes sit a fixed distance apart. When that
distance is a multiple of 4096 bytes and the list is longer than the
associativity, every node maps into the same handful of sets and the average
cycles-per-element jumps sharply. A non-power-of-two distance spreads the nodes
across sets and stays fast.

Key points:
- Set index comes from middle address bits, so a large power-of-two stride pins
  them, forcing collisions.
- The effect appears once list length > associativity (8 in the paper's L1d; 12
  on this machine).
- Because the L1d is virtually indexed, the programmer *can* control this by
  avoiding power-of-two strides for co-accessed data.

## This demo

`ChaseList` places `count` nodes `stride` bytes apart inside one 4096-aligned
buffer and links them in a random cycle (so the hardware prefetcher can't hide
the latency). [`bench_conflict_misses.cpp`](bench_conflict_misses.cpp) holds
`count = 32` (above associativity) and sweeps the stride.

```sh
bazel run -c opt //memory_optimization/conflict_misses:bench_conflict_misses
```

## Results on this machine (32-node list, cycles per element)

| stride (bytes) | 64 | 256 | 3968 | **4096** | 4160 | **8192** |
|---|---|---|---|---|---|---|
| cycles/element | 2.80 | 2.80 | 2.80 | **9.83** | 2.80 | **9.80** |

The two powers of two (4096, 8192) cost **3.5×** the benign strides — their
neighbour 3968/4160 stay fast. That is Figure 6.5's spike: 32 nodes cannot fit in
the 12-way L1d set they all collide into, so each hop misses.

## Modern C++ vs the paper

- The over-aligned buffer comes from over-aligned `new`
  (`new (std::align_val_t{4096}) std::byte[]`), replacing the paper's
  `posix_memalign`.
- The random single cycle is built with `std::shuffle` over an index permutation.
- `chase()` returns an address-derived hash so the dependent-load chain can't be
  optimised away; it is comparable only within one list (addresses differ across
  allocations), which the test accounts for.
