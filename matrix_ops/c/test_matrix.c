#include <gtest/gtest.h>

#include <stdint.h>

#include <initializer_list>
#include <vector>

extern "C" {
#include "matrix.h"
}

/*
 * Owns a Matrix for the duration of a test, so a failing EXPECT does not leak.
 * Every test builds its matrices through this rather than calling
 * matrix_create directly.
 */
class Owned {
public:
  Owned() : m_{0, 0, nullptr} {}
  ~Owned() { matrix_free(&m_); }
  Owned(const Owned &) = delete;
  Owned &operator = (const Owned &) = delete;

  Matrix *ptr() { return &m_; }
  const Matrix *ptr() const { return &m_; }
  Matrix &get() { return m_; }

private:
  Matrix m_;
};

/* Fills a matrix of the given shape with values in row-major order. */
static void fill(Owned &o, size_t rows, size_t cols,
                 std::initializer_list<double> values) {
  ASSERT_EQ(values.size(), rows * cols);
  ASSERT_EQ(matrix_create(rows, cols, o.ptr()), MATRIX_OK);
  size_t i = 0;
  for (double v : values)
    o.get().data[i++] = v;
}

/* Flattens a matrix into row-major order for comparison. */
static std::vector<double> elements(const Matrix *m) {
  return std::vector<double>(m->data, m->data + m->rows * m->cols);
}

/* --- matrix_create --- */

TEST(Create, ZeroFillsAndSetsShape) {
  Owned a;
  ASSERT_EQ(matrix_create(2, 3, a.ptr()), MATRIX_OK);
  EXPECT_EQ(a.get().rows, 2u);
  EXPECT_EQ(a.get().cols, 3u);
  EXPECT_EQ(elements(a.ptr()), std::vector<double>(6, 0.0));
}

TEST(Create, RejectsAZeroDimension) {
  Matrix m = {99, 99, nullptr};
  EXPECT_EQ(matrix_create(0, 3, &m), MATRIX_ERR_BAD_SHAPE);
  EXPECT_EQ(matrix_create(3, 0, &m), MATRIX_ERR_BAD_SHAPE);
  /* The failure left the caller's matrix alone. */
  EXPECT_EQ(m.rows, 99u);
}

TEST(Create, RejectsAnOverflowingProduct) {
  /* Two dimensions whose product wraps. Caught before the multiply, so calloc
   * is never asked for a plausible-looking small allocation. */
  Matrix m = {0, 0, nullptr};
  size_t half = SIZE_MAX / 2 + 1;
  EXPECT_EQ(matrix_create(half, 4, &m), MATRIX_ERR_OVERFLOW);
  EXPECT_EQ(m.data, nullptr);
}

TEST(Free, IsSafeOnAZeroInitializedMatrix) {
  Matrix m = {0, 0, nullptr};
  matrix_free(&m);
  matrix_free(
      &m); /* and idempotent, which is what the CLI's cleanup relies on */
  matrix_free(nullptr);
  SUCCEED();
}

/* --- matrix_at / matrix_set --- */

TEST(Access, IsRowMajor) {
  Owned a;
  fill(a, 2, 3, {1, 2, 3, 4, 5, 6});
  EXPECT_EQ(matrix_at(a.ptr(), 0, 0), 1);
  EXPECT_EQ(matrix_at(a.ptr(), 0, 2), 3);
  EXPECT_EQ(matrix_at(a.ptr(), 1, 0), 4);
  EXPECT_EQ(matrix_at(a.ptr(), 1, 2), 6);

  matrix_set(a.ptr(), 1, 1, 50);
  EXPECT_EQ(a.get().data[4], 50);
}

/* --- matrix_add / matrix_sub --- */

TEST(Add, SumsElementWise) {
  Owned a, b, out;
  fill(a, 2, 3, {1, 2, 3, 4, 5, 6});
  fill(b, 2, 3, {10, 20, 30, 40, 50, 60});
  ASSERT_EQ(matrix_add(a.ptr(), b.ptr(), out.ptr()), MATRIX_OK);
  EXPECT_EQ(out.get().rows, 2u);
  EXPECT_EQ(out.get().cols, 3u);
  EXPECT_EQ(elements(out.ptr()), (std::vector<double>{11, 22, 33, 44, 55, 66}));
}

TEST(Sub, SubtractsElementWiseAndIsNotCommutative) {
  Owned a, b, ab, ba;
  fill(a, 1, 3, {1, 2, 3});
  fill(b, 1, 3, {10, 20, 30});
  ASSERT_EQ(matrix_sub(a.ptr(), b.ptr(), ab.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_sub(b.ptr(), a.ptr(), ba.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(ab.ptr()), (std::vector<double>{-9, -18, -27}));
  EXPECT_EQ(elements(ba.ptr()), (std::vector<double>{9, 18, 27}));
}

TEST(AddSub, RoundTrip) {
  Owned a, b, sum, back;
  fill(a, 2, 2, {1.5, -2.25, 0, 7});
  fill(b, 2, 2, {0.5, 2.25, -3, 1});
  ASSERT_EQ(matrix_add(a.ptr(), b.ptr(), sum.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_sub(sum.ptr(), b.ptr(), back.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(back.ptr()), elements(a.ptr()));
}

TEST(Add, RejectsMismatchedShapes) {
  Owned a, b;
  Matrix out = {0, 0, nullptr};
  fill(a, 2, 3, {1, 2, 3, 4, 5, 6});
  fill(b, 3, 2, {1, 2, 3, 4, 5, 6});
  /* Same element count, different shape — the count is not what is checked. */
  EXPECT_EQ(matrix_add(a.ptr(), b.ptr(), &out), MATRIX_ERR_DIM_MISMATCH);
  EXPECT_EQ(matrix_sub(a.ptr(), b.ptr(), &out), MATRIX_ERR_DIM_MISMATCH);
  EXPECT_EQ(out.data, nullptr);
}

/* --- matrix_mul --- */

TEST(Mul, NonSquareProduct) {
  Owned a, b, out;
  fill(a, 2, 3, {1, 2, 3, 4, 5, 6});
  fill(b, 3, 2, {7, 8, 9, 10, 11, 12});
  ASSERT_EQ(matrix_mul(a.ptr(), b.ptr(), out.ptr()), MATRIX_OK);
  /* The outer dimensions become the result's shape. */
  EXPECT_EQ(out.get().rows, 2u);
  EXPECT_EQ(out.get().cols, 2u);
  EXPECT_EQ(elements(out.ptr()), (std::vector<double>{58, 64, 139, 154}));
}

TEST(Mul, IdentityLeavesAMatrixUnchanged) {
  Owned a, id, out;
  fill(a, 2, 2, {1, 2, 3, 4});
  fill(id, 2, 2, {1, 0, 0, 1});
  ASSERT_EQ(matrix_mul(a.ptr(), id.ptr(), out.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(out.ptr()), elements(a.ptr()));
}

TEST(Mul, IsNotCommutative) {
  Owned a, b, ab, ba;
  fill(a, 2, 2, {1, 2, 3, 4});
  fill(b, 2, 2, {0, 1, 0, 0});
  ASSERT_EQ(matrix_mul(a.ptr(), b.ptr(), ab.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_mul(b.ptr(), a.ptr(), ba.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(ab.ptr()), (std::vector<double>{0, 1, 0, 3}));
  EXPECT_EQ(elements(ba.ptr()), (std::vector<double>{3, 4, 0, 0}));
}

TEST(Mul, RowVectorTimesColumnVectorIsOneByOne) {
  Owned a, b, out;
  fill(a, 1, 3, {1, 2, 3});
  fill(b, 3, 1, {4, 5, 6});
  ASSERT_EQ(matrix_mul(a.ptr(), b.ptr(), out.ptr()), MATRIX_OK);
  EXPECT_EQ(out.get().rows, 1u);
  EXPECT_EQ(out.get().cols, 1u);
  EXPECT_EQ(elements(out.ptr()), std::vector<double>{32});
}

TEST(Mul, ColumnVectorTimesRowVectorIsTheOuterProduct) {
  Owned a, b, out;
  fill(a, 3, 1, {1, 2, 3});
  fill(b, 1, 3, {4, 5, 6});
  ASSERT_EQ(matrix_mul(a.ptr(), b.ptr(), out.ptr()), MATRIX_OK);
  EXPECT_EQ(out.get().rows, 3u);
  EXPECT_EQ(out.get().cols, 3u);
  EXPECT_EQ(elements(out.ptr()),
            (std::vector<double>{4, 5, 6, 8, 10, 12, 12, 15, 18}));
}

TEST(Mul, RejectsAMismatchedInnerDimension) {
  Owned a, b;
  Matrix out = {0, 0, nullptr};
  fill(a, 2, 3, {1, 2, 3, 4, 5, 6});
  fill(b, 2, 3, {1, 2, 3, 4, 5, 6});
  /* Identical shapes, which add accepts and mul does not: 3 != 2. */
  EXPECT_EQ(matrix_mul(a.ptr(), b.ptr(), &out), MATRIX_ERR_DIM_MISMATCH);
  EXPECT_EQ(out.data, nullptr);
}

TEST(Mul, IsAssociative) {
  Owned a, b, c, ab, ab_c, bc, a_bc;
  fill(a, 2, 3, {1, 2, 3, 4, 5, 6});
  fill(b, 3, 2, {7, 8, 9, 10, 11, 12});
  fill(c, 2, 2, {1, 2, 3, 4});
  ASSERT_EQ(matrix_mul(a.ptr(), b.ptr(), ab.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_mul(ab.ptr(), c.ptr(), ab_c.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_mul(b.ptr(), c.ptr(), bc.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_mul(a.ptr(), bc.ptr(), a_bc.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(ab_c.ptr()), elements(a_bc.ptr()));
}

/* --- matrix_scale --- */

TEST(Scale, MultipliesEveryElement) {
  Owned a, out;
  fill(a, 2, 2, {1, 2, 3, 4});
  ASSERT_EQ(matrix_scale(a.ptr(), 2.5, out.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(out.ptr()), (std::vector<double>{2.5, 5, 7.5, 10}));
}

TEST(Scale, ByZeroAndByNegativeOne) {
  Owned a, zero, negated;
  fill(a, 1, 3, {1, -2, 3});
  ASSERT_EQ(matrix_scale(a.ptr(), 0, zero.ptr()), MATRIX_OK);
  ASSERT_EQ(matrix_scale(a.ptr(), -1, negated.ptr()), MATRIX_OK);
  /* -0.0 == 0.0 compares equal, which is the property the caller cares about;
   * the printed form is normalized separately, in matrix_write. */
  EXPECT_EQ(elements(zero.ptr()), (std::vector<double>{0, 0, 0}));
  EXPECT_EQ(elements(negated.ptr()), (std::vector<double>{-1, 2, -3}));
}

TEST(Scale, LeavesTheShapeAlone) {
  Owned a, out;
  fill(a, 3, 1, {1, 2, 3});
  ASSERT_EQ(matrix_scale(a.ptr(), 7, out.ptr()), MATRIX_OK);
  EXPECT_EQ(out.get().rows, 3u);
  EXPECT_EQ(out.get().cols, 1u);
}

TEST(Scale, OneByOne) {
  Owned a, out;
  fill(a, 1, 1, {6});
  ASSERT_EQ(matrix_scale(a.ptr(), 7, out.ptr()), MATRIX_OK);
  EXPECT_EQ(elements(out.ptr()), std::vector<double>{42});
}

/* --- matrix_result_str --- */

TEST(ResultStr, NamesEveryResult) {
  const MatrixResult all[] = {
      MATRIX_OK,
      MATRIX_ERR_NOMEM,
      MATRIX_ERR_DIM_MISMATCH,
      MATRIX_ERR_BAD_NUMBER,
      MATRIX_ERR_RAGGED,
      MATRIX_ERR_EMPTY,
      MATRIX_ERR_BAD_SHAPE,
      MATRIX_ERR_READ,
      MATRIX_ERR_WRITE,
      MATRIX_ERR_OVERFLOW,
  };
  for (MatrixResult r : all) {
    const char *s = matrix_result_str(r);
    EXPECT_NE(s, nullptr);
    EXPECT_STRNE(s, "unknown error") << "result " << static_cast<int>(r);
  }
}
