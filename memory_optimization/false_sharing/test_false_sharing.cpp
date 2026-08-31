#include "memory_optimization/false_sharing/false_sharing.hpp"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "memory_optimization/support/cacheinfo.hpp"

namespace memory_optimization::false_sharing {
namespace {

// Whatever the layout, the arithmetic must be exact: false sharing is a
// performance defect, not a correctness one. Each thread owns its counter, so
// no updates are lost either way.
TEST(FalseSharing, TotalIsExact) {
  constexpr std::size_t kThreads = 8;
  constexpr std::uint64_t kIters = 100'000;
  for (bool padded : {false, true}) {
    EXPECT_EQ(run(kThreads, kIters, padded), kThreads * kIters);
  }
}

TEST(FalseSharing, SingleThread) {
  EXPECT_EQ(run(1, 12345, false), 12345u);
  EXPECT_EQ(run(1, 12345, true), 12345u);
}

// The padding must actually cover a real cache line. If sysconf reports a line
// larger than our pinned constant, the constant would be too small to separate
// counters -- catch that here rather than in a silent benchmark regression.
TEST(FalseSharing, PaddingCoversCacheLine) {
  EXPECT_GE(kCacheLine, support::cache_line_size());
  EXPECT_EQ(sizeof(Padded) % kCacheLine, 0u);
}

} // namespace
} // namespace memory_optimization::false_sharing
