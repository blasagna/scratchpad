#include "memory_optimization/bypassing_cache/bypass.hpp"

#include <cstddef>

#include <gtest/gtest.h>

namespace memory_optimization::bypassing_cache {
namespace {

bool all_equal(const Matrix &m, double value) {
  for (double x : m.data) {
    if (x != value) {
      return false;
    }
  }
  return true;
}

// The non-temporal path must produce byte-identical results to the normal one
// -- bypassing the cache is a performance choice, not a semantic one. Both
// orders, since column-major exercises the strided address pattern.
TEST(Bypass, NonTemporalWritesSameAsNormal) {
  constexpr std::size_t kN = 64;
  constexpr double kValue = 3.5;

  for (Order order : {Order::kRowMajor, Order::kColMajor}) {
    Matrix normal(kN);
    fill_normal(normal, kValue, order);
    EXPECT_TRUE(all_equal(normal, kValue));

    Matrix nt(kN);
    fill_nontemporal(nt, kValue, order);
    EXPECT_TRUE(all_equal(nt, kValue));

    EXPECT_EQ(normal.data, nt.data);
  }
}

// Odd N (not a multiple of the 2-doubles-per-16-bytes streaming granularity)
// still fills completely, since we stream one double at a time.
TEST(Bypass, HandlesOddSize) {
  constexpr std::size_t kN = 17;
  Matrix m(kN);
  fill_nontemporal(m, 1.0, Order::kRowMajor);
  EXPECT_TRUE(all_equal(m, 1.0));
}

} // namespace
} // namespace memory_optimization::bypassing_cache
