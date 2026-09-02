# bypassing the cache

Reproduces **§6.1, "Bypassing the Cache"**: writing data that will not be read
again soon, without polluting the cache, using non-temporal (streaming) stores.

## What the paper shows

An ordinary store reads the target cache line, modifies it, and keeps it cached.
When you are *producing* a large result that is consumed much later — filling a
big matrix, `memset`ing a large block — that is doubly wasteful: it evicts data
you still need and caches data you don't. x86 provides **non-temporal store**
intrinsics (`_mm_stream_*`) that write straight to memory through the
write-combining buffer, skipping the cache. The C library's `memset` uses exactly
this for large blocks.

The paper's Table 6.1 initialises a large matrix four ways — {normal,
non-temporal} × {row-order inner loop (sequential), column-order inner loop
(strided)} — and finds:

- **Sequential writes are as fast bypassing the cache as through it**, because
  write-combining fills whole lines either way — and the non-temporal version
  *saves* the cache for useful data.
- **Column-order writes are much slower**, since each touches a fresh line so no
  write-combining is possible; non-temporal is then no better, even worse.
- Non-temporal stores are weakly ordered, so an `sfence` is needed before the
  results are read back.

## This demo

`fill_normal` / `fill_nontemporal` over an N×N matrix in row- or column-major
order, in [`bypass.hpp`](bypass.hpp). The non-temporal path uses
`_mm_stream_si64` per double and an `_mm_sfence` at the end.
[`test_bypass.cpp`](test_bypass.cpp) asserts the streamed result is byte-identical
to the normal one.

```sh
bazel run -c opt //memory_optimization/bypassing_cache:bench_bypass
```

## Results on this machine (3000×3000, ~68 MiB — larger than L3)

| | row-order (sequential) | column-order (strided) |
|---|---|---|
| normal store | 3.13 ms | 7.21 ms |
| non-temporal | 2.95 ms | 23.3 ms |

Exactly the paper's shape: streaming the sequential fill is as fast as (here a hair
faster than) the cached fill, while streaming the strided fill is much worse
because it defeats write-combining. `arg 0` is row-major, `arg 1` column-major.

## Modern C++ vs the paper

- The paper streams 16 bytes at a time with `_mm_stream_si128`. This demo streams
  one 8-byte `double` with `_mm_stream_si64`, so the *same* per-element loop
  serves both row and column order (128-bit streaming only makes sense for the
  contiguous row case). C++ has **no portable non-temporal store**, so the
  intrinsic stays; the value is formed with `std::bit_cast`.
