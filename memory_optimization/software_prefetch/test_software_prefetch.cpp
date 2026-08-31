#include "memory_optimization/software_prefetch/software_prefetch.hpp"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace memory_optimization::software_prefetch {
namespace {

// Prefetching must not change results: it is a latency hint, nothing more. The
// plain and prefetching walks must sum the same payloads over the same steps.
TEST(SoftwarePrefetch, PrefetchMatchesPlain) {
  ChaseList list(1000, 8);
  for (std::size_t steps : {0u, 1u, 500u, 3000u}) {
    EXPECT_EQ(list.chase_plain(steps), list.chase_prefetch(steps))
        << "steps=" << steps;
  }
}

// Over exactly `count` hops the walk visits every node once, so the payload sum
// is 0 + 1 + ... + (count-1) -- confirms it is a single cycle covering all.
TEST(SoftwarePrefetch, VisitsEveryNodeOncePerCycle) {
  constexpr std::size_t kCount = 256;
  ChaseList list(kCount, 5);
  const std::uint64_t expected = kCount * (kCount - 1) / 2;
  EXPECT_EQ(list.chase_plain(kCount), expected);
}

// Each node is padded to a full cache line so a hop is a guaranteed miss.
TEST(SoftwarePrefetch, NodeIsCacheLineSized) { EXPECT_EQ(sizeof(Node), 64u); }

} // namespace
} // namespace memory_optimization::software_prefetch
