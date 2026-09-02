#include "memory_optimization/data_layout/data_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace memory_optimization::data_layout {
namespace {

// The fat and split layouts must compute the same total -- splitting is a cache
// optimization, not a change of meaning.
TEST(DataLayout, FatAndSplitAgree) {
  std::vector<Order> fat;
  std::vector<HotOrder> hot;
  double expected = 0.0;
  for (int i = 0; i < 1000; ++i) {
    const bool paid = (i % 3) == 0;
    const double price = i * 1.5;
    fat.push_back(
        Order{.price = price, .paid = paid, .buyer = {}, .buyer_id = i});
    hot.push_back(HotOrder{.price = price, .paid = paid});
    if (!paid) {
      expected += price;
    }
  }
  EXPECT_DOUBLE_EQ(total_due_fat(fat), expected);
  EXPECT_DOUBLE_EQ(total_due_split(hot), expected);
}

// The paper's struct order is a single 64-byte cache line on x86-64; pin that
// so the pahole example in the README stays accurate.
TEST(DataLayout, OrderIsOneCacheLine) {
  EXPECT_EQ(sizeof(Order), 64u);
  EXPECT_EQ(sizeof(HotOrder), 16u); // ~4 per line
}

// increment_all touches every slot exactly once regardless of alignment, and
// leaves each slot holding the number of times it was incremented.
TEST(DataLayout, IncrementIsCorrectAlignedAndStraddling) {
  for (std::size_t offset : {std::size_t{0}, std::size_t{60}}) {
    SlotBuffer buf(100, 64, offset);
    EXPECT_EQ(buf.increment_all(), 100u);
    EXPECT_EQ(buf.increment_all(), 100u);
  }
}

} // namespace
} // namespace memory_optimization::data_layout
