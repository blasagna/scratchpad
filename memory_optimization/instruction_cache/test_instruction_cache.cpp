#include "memory_optimization/instruction_cache/instruction_cache.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace memory_optimization::instruction_cache {
namespace {

std::vector<std::uint64_t> sample() {
  std::vector<std::uint64_t> v;
  for (std::uint64_t i = 0; i < 10'000; ++i) {
    v.push_back(i * 7 + 1);
  }
  return v;
}

// Inlining is a code-generation choice; both spellings must sum the same.
TEST(InstructionCache, InlinedMatchesNotInlined) {
  const auto data = sample();
  EXPECT_EQ(sum_inlined(data), sum_not_inlined(data));
}

// The hint direction cannot change the result, only the generated layout.
TEST(InstructionCache, HintDirectionDoesNotChangeResult) {
  auto data = sample();
  const std::uint64_t sentinel = data[123]; // appears at least once
  EXPECT_EQ(scan_well_hinted(data, sentinel), scan_mis_hinted(data, sentinel));
}

// A sentinel that never occurs exercises the pure common-path case.
TEST(InstructionCache, NoSentinelHits) {
  const auto data = sample();
  const std::uint64_t sentinel = 999'999'999; // not present
  const std::uint64_t well = scan_well_hinted(data, sentinel);
  EXPECT_EQ(well, scan_mis_hinted(data, sentinel));
  // With no sentinel the fold never runs, so it is a plain sum.
  std::uint64_t plain = 0;
  for (std::uint64_t x : data) {
    plain += x;
  }
  EXPECT_EQ(well, plain);
}

} // namespace
} // namespace memory_optimization::instruction_cache
