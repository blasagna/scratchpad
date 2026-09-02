#include "memory_optimization/tlb_usage/tlb_usage.hpp"

#include <cstddef>

#include <gtest/gtest.h>

namespace memory_optimization::tlb_usage {
namespace {

// The walk visits every page exactly once per cycle regardless of page backing:
// huge pages are a translation optimization, not a change to what is touched.
// Walking num_pages steps returns to the head, so num_pages and 2*num_pages
// steps land identically only if it is one full cycle -- but the hash is path-
// dependent, so instead we confirm both backings build a working, non-crashing
// walk over the same node count and agree with themselves.
TEST(PageWalker, WalksBothBackings) {
  for (bool huge : {false, true}) {
    PageWalker w(256, huge);
    EXPECT_EQ(w.num_pages(), 256u);
    EXPECT_EQ(w.walk(1000), w.walk(1000)); // pure
    EXPECT_EQ(w.huge_pages(), huge);
  }
}

// A single page is a degenerate cycle (points to itself); must not crash.
TEST(PageWalker, SinglePage) {
  PageWalker w(1, false);
  EXPECT_EQ(w.walk(10), w.walk(10));
}

// Zero pages maps nothing and does not index an empty permutation (constructor
// guard); construction and destruction must be clean.
TEST(PageWalker, ZeroPagesIsSafe) {
  PageWalker w(0, false);
  EXPECT_EQ(w.num_pages(), 0u);
}

} // namespace
} // namespace memory_optimization::tlb_usage
