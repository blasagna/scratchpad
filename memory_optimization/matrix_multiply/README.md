# matrix multiply

Reproduces the flagship example of **§6.2.1, "Optimizing Level 1 Data Cache
Access"**: multiplying two N×N `double` matrices, and the two rewrites that leave
the math untouched but transform the memory access pattern.

## What the paper shows

The textbook triple loop `res[i][j] += mul1[i][k] * mul2[k][j]` walks the second
matrix **down a column** in the inner loop. Column elements are N doubles apart in
a row-major layout, so every inner step lands on a different cache line, and each
output element re-streams all of `mul2` through the cache. For a 1000×1000 matrix
none of that stays resident.

Two fixes:

- **Transpose** `mul2` first, so the inner loop reads it row-wise (sequentially).
  The transpose costs N² writes but removes N column walks. The paper measures a
  **~76% speed-up** (Table 6.2) despite the extra copy.
- **Block/tile** the loops so an `SM × SM` sub-block of each matrix stays in L1d,
  where `SM = cache_line_size / sizeof(double)` (= 8 on a 64-byte line). No
  transpose, no extra memory — faster still in the paper.

Key points:
- The optimization is pure locality; the arithmetic and the result are identical.
- `SM` is derived from the *runtime* cache-line size (the paper: query it with
  `sysconf`/`getconf`, don't hardcode).
- The paper goes further (SSE vectorization) to reach ~10% of the original time;
  this demo stops at blocking, which is where the *memory* story is.

## This demo

`mul_naive`, `mul_transposed`, `mul_blocked` in [`matmul.hpp`](matmul.hpp).
[`test_matmul.cpp`](test_matmul.cpp) checks the transposed and blocked results
match the naive one for sizes including non-multiples of `SM`.

```sh
bazel run -c opt //memory_optimization/matrix_multiply:bench_matmul
```

## Results on this machine (Ryzen 7 9700X)

| N | naive | transposed | blocked |
|---|-------|-----------|---------|
| 1024 | 3162 ms | 389 ms | 408 ms |
| 512  | 183 ms  | 46 ms  | 51 ms  |

The transpose alone is **~8×** here — a bigger win than the paper's 2007 machine,
because the naive column walk is even more punishing against today's deeper
hierarchy. Blocking lands next to the transpose at N≤1024 rather than clearly
ahead: this CPU has a 32 MiB L3 and aggressive prefetchers, so the transposed
sequential stream is already near-optimal at this scale. Blocking's edge shows
when the working set greatly exceeds cache; the mechanism (L1d-resident tiles) is
the same one the paper describes.

## Modern C++ vs the paper

- Buffers are `std::vector<double>` behind a small `Matrix` struct, not raw
  `double[N][N]`; the hot-loop index math is kept identical so the cache
  behaviour matches.
- `mul_blocked` keeps the paper's raw row-pointer arithmetic (`rres`/`rmul1`
  advancing by N) for fidelity, but **clamps the inner block bounds** so any N
  works — the paper's listing assumes `N % SM == 0`. That clamp is the one
  deliberate divergence, flagged in the code.
