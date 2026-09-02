#include "memory_optimization/support/cacheinfo.hpp"

#include <gtest/gtest.h>

namespace memory_optimization::support {
namespace {

// cache_line_size() must never return 0: it falls back to the 64-byte default
// when sysconf cannot answer. The whole point is that a caller can divide by it
// without guarding against zero.
TEST(CacheInfo, LineSizeIsNonZeroAndPowerOfTwo) {
  const std::size_t line = cache_line_size();
  EXPECT_GT(line, 0u);
  EXPECT_EQ(line & (line - 1), 0u)
      << "cache line size should be a power of two";
}

// On any real Linux/x86-64 box sysconf reports the sizes and they nest, but we
// only assert the invariants that must hold whatever the machine: sizes are
// either unknown (0) or ordered L1d <= L2 <= L3.
TEST(CacheInfo, SizesNestWhenKnown) {
  const CacheInfo info = query_cache_info();
  if (info.l1d_size != 0 && info.l2_size != 0) {
    EXPECT_LE(info.l1d_size, info.l2_size);
  }
  if (info.l2_size != 0 && info.l3_size != 0) {
    EXPECT_LE(info.l2_size, info.l3_size);
  }
}

} // namespace
} // namespace memory_optimization::support
