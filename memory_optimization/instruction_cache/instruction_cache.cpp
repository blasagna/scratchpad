#include "memory_optimization/instruction_cache/instruction_cache.hpp"

#include <cstdint>
#include <span>

namespace memory_optimization::instruction_cache {

std::uint64_t transform_noinline(std::uint64_t x) {
  // Same body as transform_inline, but the attribute forbids inlining, so every
  // element pays a real call/return.
  return x * 2654435761u + (x >> 3);
}

std::uint64_t sum_inlined(std::span<const std::uint64_t> data) {
  std::uint64_t acc = 0;
  for (std::uint64_t x : data) {
    acc += transform_inline(x);
  }
  return acc;
}

std::uint64_t sum_not_inlined(std::span<const std::uint64_t> data) {
  std::uint64_t acc = 0;
  for (std::uint64_t x : data) {
    acc += transform_noinline(x);
  }
  return acc;
}

namespace {

// The "expensive" rare-path fold. Marked noinline so the hint's effect on code
// layout (inline vs moved out of line) is what the two scanners actually differ
// on.
[[gnu::noinline]] std::uint64_t rare_fold(std::uint64_t acc, std::uint64_t x) {
  for (int i = 0; i < 16; ++i) {
    acc = (acc ^ (x + i)) * 1099511628211u;
  }
  return acc;
}

} // namespace

std::uint64_t scan_well_hinted(std::span<const std::uint64_t> data,
                               std::uint64_t sentinel) {
  std::uint64_t acc = 0;
  for (std::uint64_t x : data) {
    if (x == sentinel) [[unlikely]] { // paper: unlikely(x == sentinel)
      acc = rare_fold(acc, x);
    } else {
      acc += x;
    }
  }
  return acc;
}

std::uint64_t scan_mis_hinted(std::span<const std::uint64_t> data,
                              std::uint64_t sentinel) {
  std::uint64_t acc = 0;
  for (std::uint64_t x : data) {
    if (x == sentinel) [[likely]] { // deliberately wrong hint
      acc = rare_fold(acc, x);
    } else {
      acc += x;
    }
  }
  return acc;
}

} // namespace memory_optimization::instruction_cache
