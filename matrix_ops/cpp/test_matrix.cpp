#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <vector>

#include "matrix.hpp"

namespace {

using matrix_ops::Error;
using matrix_ops::Matrix;

// Builds a matrix of the given shape from values in row-major order. The C
// port's tests need an RAII wrapper around every matrix; here Matrix owns its
// storage, so there is nothing to release.
Matrix make(std::size_t rows, std::size_t cols,
            std::initializer_list<double> values) {
  EXPECT_EQ(values.size(), rows * cols);
  std::optional<Matrix> m = Matrix::create(rows, cols);
  EXPECT_TRUE(m.has_value());
  std::size_t i = 0;
  for (const double v : values)
    m->values()[i++] = v;
  return *std::move(m);
}

// Flattens a matrix into row-major order for comparison.
std::vector<double> elements(const Matrix &m) {
  return std::vector<double>(m.values().begin(), m.values().end());
}

} // namespace

/* --- Matrix::create --- */

TEST(Create, ZeroFillsAndSetsShape) {
  const std::optional<Matrix> a = Matrix::create(2, 3);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->rows(), 2u);
  EXPECT_EQ(a->cols(), 3u);
  EXPECT_EQ(elements(*a), std::vector<double>(6, 0.0));
}

TEST(Create, RejectsAZeroDimension) {
  EXPECT_FALSE(Matrix::create(0, 3).has_value());
  EXPECT_FALSE(Matrix::create(3, 0).has_value());
}

TEST(Create, RejectsAnOverflowingProduct) {
  // Two dimensions whose product wraps. Caught before the multiply, so the
  // allocation is never asked for a plausible-looking small size.
  const std::size_t half = std::numeric_limits<std::size_t>::max() / 2 + 1;
  EXPECT_FALSE(Matrix::create(half, 4).has_value());
}

/* --- element access --- */

TEST(Access, IsRowMajor) {
  Matrix a = make(2, 3, {1, 2, 3, 4, 5, 6});
  EXPECT_EQ(a.at(0, 0), 1);
  EXPECT_EQ(a.at(0, 2), 3);
  EXPECT_EQ(a.at(1, 0), 4);
  EXPECT_EQ(a.at(1, 2), 6);

  a.at(1, 1) = 50;
  EXPECT_EQ(a.values()[4], 50);
}

/* --- add / sub --- */

TEST(Add, SumsElementWise) {
  const Matrix a = make(2, 3, {1, 2, 3, 4, 5, 6});
  const Matrix b = make(2, 3, {10, 20, 30, 40, 50, 60});
  const std::optional<Matrix> out = matrix_ops::add(a, b);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->rows(), 2u);
  EXPECT_EQ(out->cols(), 3u);
  EXPECT_EQ(elements(*out), (std::vector<double>{11, 22, 33, 44, 55, 66}));
}

TEST(Sub, SubtractsElementWiseAndIsNotCommutative) {
  const Matrix a = make(1, 3, {1, 2, 3});
  const Matrix b = make(1, 3, {10, 20, 30});
  const std::optional<Matrix> ab = matrix_ops::sub(a, b);
  const std::optional<Matrix> ba = matrix_ops::sub(b, a);
  ASSERT_TRUE(ab.has_value());
  ASSERT_TRUE(ba.has_value());
  EXPECT_EQ(elements(*ab), (std::vector<double>{-9, -18, -27}));
  EXPECT_EQ(elements(*ba), (std::vector<double>{9, 18, 27}));
}

TEST(AddSub, RoundTrip) {
  const Matrix a = make(2, 2, {1.5, -2.25, 0, 7});
  const Matrix b = make(2, 2, {0.5, 2.25, -3, 1});
  const std::optional<Matrix> sum = matrix_ops::add(a, b);
  ASSERT_TRUE(sum.has_value());
  const std::optional<Matrix> back = matrix_ops::sub(*sum, b);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(elements(*back), elements(a));
}

TEST(Add, RejectsMismatchedShapes) {
  const Matrix a = make(2, 3, {1, 2, 3, 4, 5, 6});
  const Matrix b = make(3, 2, {1, 2, 3, 4, 5, 6});
  // Same element count, different shape -- the count is not what is checked.
  EXPECT_FALSE(matrix_ops::add(a, b).has_value());
  EXPECT_FALSE(matrix_ops::sub(a, b).has_value());
}

/* --- mul --- */

TEST(Mul, NonSquareProduct) {
  const Matrix a = make(2, 3, {1, 2, 3, 4, 5, 6});
  const Matrix b = make(3, 2, {7, 8, 9, 10, 11, 12});
  const std::optional<Matrix> out = matrix_ops::mul(a, b);
  ASSERT_TRUE(out.has_value());
  // The outer dimensions become the result's shape.
  EXPECT_EQ(out->rows(), 2u);
  EXPECT_EQ(out->cols(), 2u);
  EXPECT_EQ(elements(*out), (std::vector<double>{58, 64, 139, 154}));
}

TEST(Mul, IdentityLeavesAMatrixUnchanged) {
  const Matrix a = make(2, 2, {1, 2, 3, 4});
  const Matrix id = make(2, 2, {1, 0, 0, 1});
  const std::optional<Matrix> out = matrix_ops::mul(a, id);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(elements(*out), elements(a));
}

TEST(Mul, IsNotCommutative) {
  const Matrix a = make(2, 2, {1, 2, 3, 4});
  const Matrix b = make(2, 2, {0, 1, 0, 0});
  const std::optional<Matrix> ab = matrix_ops::mul(a, b);
  const std::optional<Matrix> ba = matrix_ops::mul(b, a);
  ASSERT_TRUE(ab.has_value());
  ASSERT_TRUE(ba.has_value());
  EXPECT_EQ(elements(*ab), (std::vector<double>{0, 1, 0, 3}));
  EXPECT_EQ(elements(*ba), (std::vector<double>{3, 4, 0, 0}));
}

TEST(Mul, RowVectorTimesColumnVectorIsOneByOne) {
  const Matrix a = make(1, 3, {1, 2, 3});
  const Matrix b = make(3, 1, {4, 5, 6});
  const std::optional<Matrix> out = matrix_ops::mul(a, b);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->rows(), 1u);
  EXPECT_EQ(out->cols(), 1u);
  EXPECT_EQ(elements(*out), std::vector<double>{32});
}

TEST(Mul, ColumnVectorTimesRowVectorIsTheOuterProduct) {
  const Matrix a = make(3, 1, {1, 2, 3});
  const Matrix b = make(1, 3, {4, 5, 6});
  const std::optional<Matrix> out = matrix_ops::mul(a, b);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->rows(), 3u);
  EXPECT_EQ(out->cols(), 3u);
  EXPECT_EQ(elements(*out),
            (std::vector<double>{4, 5, 6, 8, 10, 12, 12, 15, 18}));
}

TEST(Mul, RejectsAMismatchedInnerDimension) {
  const Matrix a = make(2, 3, {1, 2, 3, 4, 5, 6});
  const Matrix b = make(2, 3, {1, 2, 3, 4, 5, 6});
  // Identical shapes, which add accepts and mul does not: 3 != 2.
  EXPECT_FALSE(matrix_ops::mul(a, b).has_value());
}

TEST(Mul, IsAssociative) {
  const Matrix a = make(2, 3, {1, 2, 3, 4, 5, 6});
  const Matrix b = make(3, 2, {7, 8, 9, 10, 11, 12});
  const Matrix c = make(2, 2, {1, 2, 3, 4});
  const std::optional<Matrix> ab = matrix_ops::mul(a, b);
  ASSERT_TRUE(ab.has_value());
  const std::optional<Matrix> ab_c = matrix_ops::mul(*ab, c);
  const std::optional<Matrix> bc = matrix_ops::mul(b, c);
  ASSERT_TRUE(bc.has_value());
  const std::optional<Matrix> a_bc = matrix_ops::mul(a, *bc);
  ASSERT_TRUE(ab_c.has_value());
  ASSERT_TRUE(a_bc.has_value());
  EXPECT_EQ(elements(*ab_c), elements(*a_bc));
}

/* --- scale --- */

TEST(Scale, MultipliesEveryElement) {
  const Matrix a = make(2, 2, {1, 2, 3, 4});
  EXPECT_EQ(elements(matrix_ops::scale(a, 2.5)),
            (std::vector<double>{2.5, 5, 7.5, 10}));
}

TEST(Scale, ByZeroAndByNegativeOne) {
  const Matrix a = make(1, 3, {1, -2, 3});
  // -0.0 == 0.0 compares equal, which is the property the caller cares about;
  // the printed form is normalized separately, in render().
  EXPECT_EQ(elements(matrix_ops::scale(a, 0)), (std::vector<double>{0, 0, 0}));
  EXPECT_EQ(elements(matrix_ops::scale(a, -1)),
            (std::vector<double>{-1, 2, -3}));
}

TEST(Scale, LeavesTheShapeAlone) {
  const Matrix a = make(3, 1, {1, 2, 3});
  const Matrix out = matrix_ops::scale(a, 7);
  EXPECT_EQ(out.rows(), 3u);
  EXPECT_EQ(out.cols(), 1u);
}

TEST(Scale, OneByOne) {
  const Matrix a = make(1, 1, {6});
  EXPECT_EQ(elements(matrix_ops::scale(a, 7)), std::vector<double>{42});
}

/* --- describe --- */

TEST(Describe, NamesEveryError) {
  const std::array<Error, 9> all{
      Error::kOk,     Error::kDimMismatch, Error::kBadNumber,
      Error::kRagged, Error::kEmpty,       Error::kBadShape,
      Error::kRead,   Error::kWrite,       Error::kOverflow,
  };
  for (const Error error : all) {
    EXPECT_NE(matrix_ops::describe(error), "unknown error")
        << "error " << static_cast<int>(error);
  }
}

TEST(Scale, OnADefaultConstructedMatrixIsEmpty) {
  // Matrix::create refuses a 0x0, so scale() has no result matrix to build.
  // Unreachable from the CLI -- every matrix there comes from the parser --
  // but the type permits it, and it used to dereference an empty optional.
  const Matrix out = matrix_ops::scale(Matrix{}, 2.0);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(out.rows(), 0u);
  EXPECT_EQ(out.cols(), 0u);
}
