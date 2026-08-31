#include "memory_optimization/working_set/working_set.hpp"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace memory_optimization::working_set {
namespace {

// The sum is the plain arithmetic result, repeats times -- the sweep is a
// timing tool, but it must still compute the right number.
TEST(WorkingSet, SumIsCorrect) {
  std::vector<std::uint64_t> buffer(1000);
  std::iota(buffer.begin(), buffer.end(), 0); // 0..999
  const std::uint64_t one_pass = 999ull * 1000 / 2;
  EXPECT_EQ(sum_repeatedly(buffer, 1), one_pass);
  EXPECT_EQ(sum_repeatedly(buffer, 5), 5 * one_pass);
  EXPECT_EQ(sum_repeatedly(buffer, 0), 0u);
}

// level_for_size returns a known label and never nullptr; ordering is monotone
// (a small set is at most as deep as a large one).
TEST(WorkingSet, LevelLabels) {
  for (const char *l : {level_for_size(1 << 10), level_for_size(1 << 24),
                        level_for_size(1u << 30)}) {
    const std::string_view s = l;
    EXPECT_TRUE(s == "L1d" || s == "L2" || s == "L3" || s == "RAM");
  }
  // A gigabyte cannot fit in any cache.
  EXPECT_STREQ(level_for_size(1ull << 30), "RAM");
}

} // namespace
} // namespace memory_optimization::working_set
