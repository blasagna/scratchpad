#include "memory_optimization/conflict_misses/conflict_misses.hpp"

#include <cstddef>

#include <gtest/gtest.h>

namespace memory_optimization::conflict_misses {
namespace {

// The list must be a single cycle over all nodes: the cycle through the head
// has length count, so every node is reachable and the traversal never gets
// stuck in a shorter sub-loop.
TEST(ChaseList, IsOneCycleOverAllNodes) {
  constexpr std::size_t kCount = 100;
  ChaseList list(kCount, 256);
  EXPECT_EQ(list.cycle_length(), kCount);
  EXPECT_EQ(list.count(), kCount);
}

// chase() is a pure read of the structure: the same list chased twice yields
// the same value (it mutates nothing). Note the value is address-based, so it
// is not comparable across two separately-allocated lists, only within one.
TEST(ChaseList, ChaseIsRepeatable) {
  ChaseList list(64, 4096, 7);
  EXPECT_EQ(list.chase(1000), list.chase(1000));
}

// A stride smaller than a node is clamped up to sizeof(Node) so nodes never
// overlap, and the list is still one valid cycle.
TEST(ChaseList, ClampsTinyStride) {
  ChaseList list(8, 1);
  EXPECT_EQ(list.cycle_length(), 8u);
}

// A zero-count list allocates nothing and does not index an empty permutation
// (the constructor guards it). Construction and destruction must be clean --
// exercised under --config=asan for the aligned alloc/dealloc pairing too.
TEST(ChaseList, ZeroCountIsSafe) {
  ChaseList list(0, 256);
  EXPECT_EQ(list.count(), 0u);
}

} // namespace
} // namespace memory_optimization::conflict_misses
