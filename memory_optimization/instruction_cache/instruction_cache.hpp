#ifndef MEMORY_OPTIMIZATION_INSTRUCTION_CACHE_INSTRUCTION_CACHE_HPP
#define MEMORY_OPTIMIZATION_INSTRUCTION_CACHE_INSTRUCTION_CACHE_HPP

#include <cstdint>
#include <span>

// Section 6.2.2, "Optimizing Level 1 Instruction Cache Access". Unlike data,
// code layout is mostly the compiler's job, but the programmer steers it two
// ways the paper highlights:
//
//   - Inlining. Inlining a small hot function removes the call/return and lets
//     the optimizer work across the boundary; the cost is a bigger code
//     footprint, so it is a trade against L1i pressure. gcc's always_inline /
//     noinline attributes force the decision.
//
//   - Branch hints and block layout. A lopsided branch whose unlikely side is a
//     big chunk of code pollutes the hot path's I-cache and can mislead the
//     branch predictor. __builtin_expect (the paper's likely()/unlikely()
//     macros) -- spelled [[likely]]/[[unlikely]] in modern C++ -- tells the
//     compiler which way to lay the code so the common path stays linear and
//     the cold code is moved out of line.
//
// All variants below compute identical results; only the generated code
// differs.

namespace memory_optimization::instruction_cache {

// --- Inlining ---------------------------------------------------------------

// A tiny per-element transform, provided in two forms the compiler must
// respect. paper: __attribute__((always_inline)) / __attribute__((noinline));
// the modern spelling is the [[gnu::...]] attribute.
[[gnu::always_inline]] inline std::uint64_t transform_inline(std::uint64_t x) {
  return x * 2654435761u + (x >> 3);
}
[[gnu::noinline]] std::uint64_t transform_noinline(std::uint64_t x);

std::uint64_t sum_inlined(std::span<const std::uint64_t> data);
std::uint64_t sum_not_inlined(std::span<const std::uint64_t> data);

// --- Branch hints -----------------------------------------------------------

// Scans `data` accumulating a cheap running value, but on the rare occasion a
// value hits a sentinel it runs a more expensive fold. The two versions differ
// only in which branch is marked hot: `well_hinted` marks the rare path
// [[unlikely]] (correct), `mis_hinted` marks it [[likely]] (wrong), so the
// compiler lays the cold code inline and mispredicts. Same output either way.
std::uint64_t scan_well_hinted(std::span<const std::uint64_t> data,
                               std::uint64_t sentinel);
std::uint64_t scan_mis_hinted(std::span<const std::uint64_t> data,
                              std::uint64_t sentinel);

} // namespace memory_optimization::instruction_cache

#endif // MEMORY_OPTIMIZATION_INSTRUCTION_CACHE_INSTRUCTION_CACHE_HPP
