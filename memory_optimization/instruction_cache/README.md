# instruction cache

Reproduces **§6.2.2, "Optimizing Level 1 Instruction Cache Access"**: the two
levers a programmer has over code layout — inlining and branch hints.

## What the paper shows

Code, unlike data, is mostly the compiler's to lay out, but the programmer steers
it:

- **Inlining** a small hot function removes the call/return and lets the optimizer
  work across the boundary (vectorize, propagate constants). The cost is a larger
  code footprint — more L1i pressure — so it is a trade, not a free win. gcc's
  `always_inline` / `noinline` force the decision either way.
- **Branch hints and block layout.** A lopsided `if` whose unlikely side is a big
  chunk of code pollutes the hot path's I-cache and can mislead the branch
  predictor. `__builtin_expect` — the paper's `likely()`/`unlikely()` macros —
  tells the compiler which way to lay the blocks, so the common path stays linear
  and the cold code is moved out of line.

## This demo

- Inlining: `sum_inlined` vs `sum_not_inlined` apply the same per-element
  transform, one `[[gnu::always_inline]]`, one `[[gnu::noinline]]`.
- Branch hints: `scan_well_hinted` marks a rare expensive path `[[unlikely]]`
  (correct); `scan_mis_hinted` marks it `[[likely]]` (wrong). Same output.

```sh
bazel run -c opt //memory_optimization/instruction_cache:bench_instruction_cache
```

## Results on this machine (2M elements)

| | time |
|---|---|
| `sum_not_inlined` | 1.50 ms |
| `sum_inlined`     | 0.58 ms |
| `scan_mis_hinted`  | 0.453 ms |
| `scan_well_hinted` | 0.440 ms |

Inlining is a clear **~2.6×**: with the call gone, the loop vectorizes. The branch
hint is only ~3% here — an honest result. Modern branch predictors and
out-of-order front-ends recover most of a mispredicted-layout penalty at runtime,
so the hint's value today is mostly in **code layout** (keeping cold paths out of
the hot I-cache footprint), which shows up in large codebases far more than in a
micro-benchmark. To *see* the layout change, compare the disassembly:

```sh
objdump -d bazel-bin/memory_optimization/instruction_cache/bench_instruction_cache \
  | grep -A40 scan_well_hinted   # cold rare_fold call is placed out of line
```

## Modern C++ vs the paper

- `[[likely]]`/`[[unlikely]]` attributes replace the `__builtin_expect`-based
  `likely()`/`unlikely()` macros (the builtin still works; the attribute is the
  standard C++20 spelling for the same hint).
- `[[gnu::always_inline]]`/`[[gnu::noinline]]` replace the `__attribute__((...))`
  spellings — still gcc attributes, since C++ has no standard force-inline.
