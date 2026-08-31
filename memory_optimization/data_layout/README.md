# data layout

Reproduces two data-structure levers from **§6.2.1, "Optimizing Level 1 Data
Cache Access"**: hot/cold field splitting, and alignment. It also carries the
paper's `pahole` example.

## What the paper shows

**Hot/cold splitting.** The paper's `struct order { double price; bool paid;
const char *buyer[5]; long buyer_id; }` bundles a hot field a billing job reads
(`price`/`paid`) with cold fields it doesn't (`buyer`, `buyer_id`). Iterating an
array of these to total the outstanding payments drags the cold fields through
the cache, so each useful value costs most of a cache line — "up to 5 times worse
than it could be." Storing the hot fields in their own compact array packs
several per line.

**Alignment.** An access that straddles a cache-line boundary touches two lines
instead of one (Figure 6.4, "Overhead of Unaligned Accesses"). Naturally aligned
data keeps each access on one line. The paper measures roughly a 300% penalty for
L2-resident working sets and 20–30% for very large ones.

**pahole.** The `pahole` tool reads a binary's debug info and prints struct
layout with the holes and cache-line boundaries marked, so you can *see* where
padding lands and which fields share a line — and, with `--reorganize`, suggests a
tighter order.

## This demo

- `total_due_fat` (over the paper's `Order`) vs `total_due_split` (over a packed
  `HotOrder{price,paid}`) in [`data_layout.hpp`](data_layout.hpp).
- `SlotBuffer` increments 64-bit slots laid out aligned inside a line
  (`offset 0`) or straddling the 64-byte boundary (`offset 60`).

```sh
bazel run -c opt //memory_optimization/data_layout:bench_data_layout
```

## Results on this machine (4M orders; fat = 256 MiB, hot = 64 MiB)

| | time | throughput |
|---|---|---|
| `total_due_fat`   | 5.70 ms | 0.70 G orders/s |
| `total_due_split` | 1.50 ms | 2.67 G orders/s |

The split layout is **~3.8×** faster — the paper's "up to 5×." For alignment, the
straddling walk is ~18% slower than the aligned one at this (large) working-set
size, consistent with the paper's 20–30% band for large sets; Zen 5 handles
unaligned accesses well, so the effect is at the low end.

## Using pahole

`pahole` is not installed here (`sudo apt install dwarves` to get it). Build with
debug info and run it on the resulting binary — any target that pulls in the
struct's debug info works:

```sh
bazel build -c dbg //memory_optimization/data_layout:bench_data_layout
pahole -C order            bazel-bin/memory_optimization/data_layout/bench_data_layout
pahole --reorganize -C order bazel-bin/memory_optimization/data_layout/bench_data_layout
```

`test_data_layout.cpp` pins `sizeof(Order) == 64` (one cache line) and
`sizeof(HotOrder) == 16` (~4 per line), so the pahole output stays as the paper
describes.

## Modern C++ vs the paper

- `Order` is laid out exactly as the paper's `struct order` so its pahole output
  matches. Splitting produces a second array rather than a pointer-linked
  side-structure.
- The unaligned read-modify-write uses `std::memcpy` (defined in C++, lowered to a
  single unaligned `mov` on x86) where the paper dereferences a misaligned
  pointer directly. The paper's `posix_memalign` is not needed here — the
  straddle is created by an explicit byte offset, not by requesting alignment.
