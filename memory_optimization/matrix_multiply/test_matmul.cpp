#include "memory_optimization/matrix_multiply/matmul.hpp"

#include <cstddef>

#include <gtest/gtest.h>

namespace memory_optimization::matrix_multiply {
namespace {

void expect_equal(const Matrix &x, const Matrix &y) {
  ASSERT_EQ(x.n, y.n);
  for (std::size_t i = 0; i < x.n; ++i) {
    for (std::size_t j = 0; j < x.n; ++j) {
      // The three variants sum in different orders, so results are close but
      // not bit-identical; a small relative tolerance is the right check.
      EXPECT_NEAR(x.at(i, j), y.at(i, j), 1e-9)
          << "at (" << i << ", " << j << ")";
    }
  }
}

// The whole point of the demo: the cache-friendly variants must compute the
// same product as the naive one. Sizes deliberately include non-multiples of
// the blocking factor SM (=8 on a 64-byte line) to exercise the clamp.
class MatmulEquivalence : public testing::TestWithParam<std::size_t> {};

TEST_P(MatmulEquivalence, TransposedMatchesNaive) {
  const Matrix a = random_matrix(GetParam(), 1);
  const Matrix b = random_matrix(GetParam(), 2);
  expect_equal(mul_naive(a, b), mul_transposed(a, b));
}

TEST_P(MatmulEquivalence, BlockedMatchesNaive) {
  const Matrix a = random_matrix(GetParam(), 3);
  const Matrix b = random_matrix(GetParam(), 4);
  expect_equal(mul_naive(a, b), mul_blocked(a, b));
}

INSTANTIATE_TEST_SUITE_P(Sizes, MatmulEquivalence,
                         testing::Values(1u, 7u, 8u, 15u, 16u, 31u, 64u));

// A hand-checked 2x2 product, so a wholesale sign/transpose error can't hide
// behind "all three agree with each other".
TEST(Matmul, KnownProduct) {
  Matrix a(2);
  a.at(0, 0) = 1;
  a.at(0, 1) = 2;
  a.at(1, 0) = 3;
  a.at(1, 1) = 4;
  Matrix b(2);
  b.at(0, 0) = 5;
  b.at(0, 1) = 6;
  b.at(1, 0) = 7;
  b.at(1, 1) = 8;
  // [[1 2][3 4]] * [[5 6][7 8]] = [[19 22][43 50]]
  const Matrix p = mul_naive(a, b);
  EXPECT_DOUBLE_EQ(p.at(0, 0), 19);
  EXPECT_DOUBLE_EQ(p.at(0, 1), 22);
  EXPECT_DOUBLE_EQ(p.at(1, 0), 43);
  EXPECT_DOUBLE_EQ(p.at(1, 1), 50);
}

} // namespace
} // namespace memory_optimization::matrix_multiply
