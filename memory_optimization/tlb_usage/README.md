# tlb usage

Reproduces **§6.2.4, "Optimizing TLB Usage"**: touching fewer pages, and using
larger pages, to cut the cost of address translation.

## What the paper shows

Every memory access needs its virtual address translated to physical, and the TLB
caches those translations. It is small — a few thousand entries — and each entry
covers one page (4 KiB), so a working set spread across many pages thrashes the
TLB even when the data itself would fit in cache: each access then pays a
page-table walk on top of the data fetch. Two levers reduce it: touch fewer pages
(tighter data layout), or use **larger pages** so one TLB entry covers more
address space. A 2 MiB huge page covers 512× the range of a 4 KiB page.

## This demo

`PageWalker` touches one cache line per 4 KiB page across `num_pages` pages, in a
random cycle (defeating both the cache and TLB prefetchers), from an `mmap`'d
region that is either left on ordinary 4 KiB pages (`MADV_NOHUGEPAGE`) or advised
for 2 MiB transparent huge pages (`MADV_HUGEPAGE`). The set of cache lines touched
is identical in both, so the difference isolates the translation cost.

```sh
bazel run -c opt //memory_optimization/tlb_usage:bench_tlb_usage
cat /sys/kernel/mm/transparent_hugepage/enabled   # THP must be 'always' or 'madvise'
```

## Results on this machine (cycles per access)

| pages | address span | 4 KiB | 2 MiB |
|---|---|---|---|
| 512    | 2 MiB   | 14.8 | 32.0 |
| 4096   | 16 MiB  | 41.1 | 31.0 |
| 32768  | 128 MiB | 290  | 229  |
| 131072 | 512 MiB | 399  | 359  |

Once the page count blows past the TLB's reach, huge pages help — **21%** at 32Ki
pages, 10% at 128Ki. At small counts they don't (and cost a fixed setup): 512
pages is a single 2 MiB region, so there is no TLB pressure to relieve.

The effect is real but muted because each random access is *also* a cache miss to
RAM (hundreds of cycles), and the TLB saving is a fraction of that combined cost —
the two latencies overlap. This box has no reserved hugetlb pool
(`HugePages_Total: 0`), so the demo relies on **transparent** huge pages; if the
kernel declines to promote the region, the two curves converge, which is itself
worth seeing.

## Modern C++ vs the paper

- The region is `mmap`/`munmap` with `MADV_HUGEPAGE` (RAII in the `PageWalker`
  destructor); the paper discusses huge pages more than it codes them.
- The random cycle uses `std::shuffle`, matching the other pointer-chase demos.
