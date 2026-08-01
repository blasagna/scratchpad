#include <gtest/gtest.h>

#include <stdio.h>

#include <string>
#include <vector>

extern "C" {
#include "matrix_io.h"
}

/* Owns a Matrix for the duration of a test, as in test_matrix.c. */
class Owned {
public:
  Owned() : m_{0, 0, nullptr} {}
  ~Owned() { matrix_free(&m_); }
  Owned(const Owned &) = delete;
  Owned &operator = (const Owned &) = delete;

  Matrix *ptr() { return &m_; }
  Matrix &get() { return m_; }

private:
  Matrix m_;
};

static std::vector<double> elements(const Matrix *m) {
  return std::vector<double>(m->data, m->data + m->rows * m->cols);
}

/*
 * Parses text with no requested dimensions and asserts the shape, returning the
 * values in row-major order.
 */
static std::vector<double> parsed(const std::string &text, size_t want_rows,
                                  size_t want_cols, size_t expect_rows,
                                  size_t expect_cols) {
  Owned m;
  EXPECT_EQ(matrix_parse_text(text.c_str(), want_rows, want_cols, m.ptr()),
            MATRIX_OK)
      << "text: " << text;
  EXPECT_EQ(m.get().rows, expect_rows);
  EXPECT_EQ(m.get().cols, expect_cols);
  return m.get().data ? elements(m.ptr()) : std::vector<double>{};
}

/* Parses text expecting a failure, and returns the result. */
static MatrixResult parse_error(const std::string &text,
                                size_t want_rows = MATRIX_DIM_UNSPECIFIED,
                                size_t want_cols = MATRIX_DIM_UNSPECIFIED) {
  Matrix m = {0, 0, nullptr};
  MatrixResult rc = matrix_parse_text(text.c_str(), want_rows, want_cols, &m);
  EXPECT_EQ(m.data, nullptr) << "text: " << text;
  matrix_free(&m);
  return rc;
}

/* Renders a matrix through matrix_write into a string. */
static std::string written(const Matrix *m, int precision) {
  MatrixFormat fmt = matrix_format_default();
  fmt.precision = precision;

  char buf[8192];
  FILE *out = fmemopen(buf, sizeof(buf), "w");
  EXPECT_NE(out, nullptr);
  EXPECT_EQ(matrix_write(out, m, &fmt), MATRIX_OK);
  long written_len = ftell(out);
  EXPECT_EQ(fclose(out), 0);
  return std::string(buf,
                     static_cast<size_t>(written_len < 0 ? 0 : written_len));
}

/* --- shape inference from layout --- */

TEST(Shape, ASingleLineIsARowVector) {
  EXPECT_EQ(
      parsed("1 2 3", MATRIX_DIM_UNSPECIFIED, MATRIX_DIM_UNSPECIFIED, 1, 3),
      (std::vector<double>{1, 2, 3}));
}

TEST(Shape, ASingleValueIsOneByOne) {
  EXPECT_EQ(parsed("7", MATRIX_DIM_UNSPECIFIED, MATRIX_DIM_UNSPECIFIED, 1, 1),
            std::vector<double>{7});
}

TEST(Shape, NewlinesDelimitRows) {
  EXPECT_EQ(parsed("1 2 3\n4 5 6\n", MATRIX_DIM_UNSPECIFIED,
                   MATRIX_DIM_UNSPECIFIED, 2, 3),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, AFinalNewlineIsOptional) {
  EXPECT_EQ(
      parsed("1 2\n3 4", MATRIX_DIM_UNSPECIFIED, MATRIX_DIM_UNSPECIFIED, 2, 2),
      (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, BlankLinesAndSurroundingWhitespaceAreIgnored) {
  EXPECT_EQ(parsed("\n\n  1   2  \n\n  3   4  \n\n", MATRIX_DIM_UNSPECIFIED,
                   MATRIX_DIM_UNSPECIFIED, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, TabsSeparateValuesAndDoNotStartARow) {
  EXPECT_EQ(parsed("1\t2\n3\t4\n", MATRIX_DIM_UNSPECIFIED,
                   MATRIX_DIM_UNSPECIFIED, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, CarriageReturnsAreJustWhitespace) {
  /* A CRLF file parses the same as an LF one. */
  EXPECT_EQ(parsed("1 2\r\n3 4\r\n", MATRIX_DIM_UNSPECIFIED,
                   MATRIX_DIM_UNSPECIFIED, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, RaggedRowsAreAlwaysAnError) {
  EXPECT_EQ(parse_error("1 2 3\n4 5\n"), MATRIX_ERR_RAGGED);
  /* Even when the requested dimensions would make the layout irrelevant: a
   * ragged file is more often a typo than an intent. */
  EXPECT_EQ(parse_error("1 2 3\n4 5\n", 1, 5), MATRIX_ERR_RAGGED);
}

TEST(Shape, EmptyInputIsAnError) {
  EXPECT_EQ(parse_error(""), MATRIX_ERR_EMPTY);
  EXPECT_EQ(parse_error("   \n\n  \n"), MATRIX_ERR_EMPTY);
}

/* --- requested dimensions --- */

TEST(Shape, BothDimensionsReshapeAFlatList) {
  EXPECT_EQ(parsed("1 2 3 4 5 6", 2, 3, 2, 3),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(parsed("1 2 3 4 5 6", 3, 2, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(parsed("1 2 3 4 5 6", 6, 1, 6, 1),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, BothDimensionsOverrideTheLayout) {
  /* A 2x3 file read as 3x2. The values keep their row-major order. */
  EXPECT_EQ(parsed("1 2 3\n4 5 6\n", 3, 2, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, BothDimensionsMustMatchTheValueCount) {
  EXPECT_EQ(parse_error("1 2 3 4 5 6", 2, 4), MATRIX_ERR_BAD_SHAPE);
  EXPECT_EQ(parse_error("1 2 3 4 5 6", 2, 2), MATRIX_ERR_BAD_SHAPE);
}

TEST(Shape, RowsAloneDerivesTheColumns) {
  EXPECT_EQ(parsed("1 2 3 4 5 6", 2, MATRIX_DIM_UNSPECIFIED, 2, 3),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(parsed("1 2 3 4 5 6", 3, MATRIX_DIM_UNSPECIFIED, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, ColsAloneDerivesTheRows) {
  EXPECT_EQ(parsed("1 2 3 4 5 6", MATRIX_DIM_UNSPECIFIED, 2, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(parsed("1 2 3 4 5 6", MATRIX_DIM_UNSPECIFIED, 6, 1, 6),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, ASingleDimensionMustDivideTheValueCount) {
  EXPECT_EQ(parse_error("1 2 3 4 5", 2, MATRIX_DIM_UNSPECIFIED),
            MATRIX_ERR_BAD_SHAPE);
  EXPECT_EQ(parse_error("1 2 3 4 5", MATRIX_DIM_UNSPECIFIED, 2),
            MATRIX_ERR_BAD_SHAPE);
}

/* --- number parsing --- */

TEST(Numbers, AcceptsSignsDecimalsAndExponents) {
  EXPECT_EQ(parsed("-2.5 +3 1e3 -1.5E-2 .5 4.", MATRIX_DIM_UNSPECIFIED,
                   MATRIX_DIM_UNSPECIFIED, 1, 6),
            (std::vector<double>{-2.5, 3, 1000, -0.015, 0.5, 4}));
}

TEST(Numbers, RejectsNonNumericTokens) {
  EXPECT_EQ(parse_error("1 abc 3"), MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(parse_error("1 2 3x"), MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(parse_error("1.2.3"), MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(parse_error("--5"), MATRIX_ERR_BAD_NUMBER);
}

TEST(Numbers, RejectsTheNonFiniteSpellingsStrtodWouldAccept) {
  /* strtod parses all of these; the accepted set is pinned here instead so the
   * later ports have one rule to match rather than a platform's. */
  EXPECT_EQ(parse_error("nan"), MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(parse_error("1 inf 3"), MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(parse_error("infinity"), MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(parse_error("-INF"), MATRIX_ERR_BAD_NUMBER);
}

TEST(Numbers, RejectsAValueTooLargeToRepresent) {
  EXPECT_EQ(parse_error("1e400"), MATRIX_ERR_BAD_NUMBER);
}

TEST(Numbers, AcceptsAValueThatUnderflowsToZero) {
  /* strtod sets ERANGE here too, but a flush to zero is a usable answer where
   * an overflow to infinity is not. */
  Owned m;
  ASSERT_EQ(matrix_parse_text("1e-400", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  EXPECT_NEAR(m.get().data[0], 0.0, 1e-300);
}

/* --- matrix_read_stream --- */

TEST(ReadStream, ParsesAStream) {
  std::string text = "1 2 3\n4 5 6\n";
  FILE *in = fmemopen(const_cast<char *>(text.data()), text.size(), "r");
  ASSERT_NE(in, nullptr);

  Owned m;
  MatrixResult rc = matrix_read_stream(in, MATRIX_DIM_UNSPECIFIED,
                                       MATRIX_DIM_UNSPECIFIED, m.ptr());
  fclose(in);

  ASSERT_EQ(rc, MATRIX_OK);
  EXPECT_EQ(m.get().rows, 2u);
  EXPECT_EQ(m.get().cols, 3u);
  EXPECT_EQ(elements(m.ptr()), (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(ReadStream, HonorsRequestedDimensions) {
  std::string text = "1 2 3 4 5 6";
  FILE *in = fmemopen(const_cast<char *>(text.data()), text.size(), "r");
  ASSERT_NE(in, nullptr);

  Owned m;
  MatrixResult rc = matrix_read_stream(in, 3, 2, m.ptr());
  fclose(in);

  ASSERT_EQ(rc, MATRIX_OK);
  EXPECT_EQ(m.get().rows, 3u);
  EXPECT_EQ(m.get().cols, 2u);
}

TEST(ReadStream, ReadsPastOneChunk) {
  /* More than READ_CHUNK bytes, to exercise the buffer growth. */
  std::string text;
  const size_t kCount = 5000;
  for (size_t i = 0; i < kCount; i++)
    text += "1 ";

  FILE *in = fmemopen(const_cast<char *>(text.data()), text.size(), "r");
  ASSERT_NE(in, nullptr);

  Owned m;
  MatrixResult rc = matrix_read_stream(in, MATRIX_DIM_UNSPECIFIED,
                                       MATRIX_DIM_UNSPECIFIED, m.ptr());
  fclose(in);

  ASSERT_EQ(rc, MATRIX_OK);
  EXPECT_EQ(m.get().rows, 1u);
  EXPECT_EQ(m.get().cols, kCount);
}

TEST(ReadStream, EmptyStreamIsEmptyNotAReadError) {
  FILE *in = fmemopen(const_cast<char *>(""), 0, "r");
  ASSERT_NE(in, nullptr);

  Matrix m = {0, 0, nullptr};
  MatrixResult rc = matrix_read_stream(in, MATRIX_DIM_UNSPECIFIED,
                                       MATRIX_DIM_UNSPECIFIED, &m);
  fclose(in);

  EXPECT_EQ(rc, MATRIX_ERR_EMPTY);
  EXPECT_EQ(m.data, nullptr);
}

TEST(ReadStream, RejectsAnEmbeddedNul) {
  /* The parser is NUL-terminated, so a NUL would silently truncate the input.
   * Refusing it is more honest than parsing the prefix. */
  std::string text("1 2\0 3 4", 8);
  FILE *in = fmemopen(const_cast<char *>(text.data()), text.size(), "r");
  ASSERT_NE(in, nullptr);

  Matrix m = {0, 0, nullptr};
  MatrixResult rc = matrix_read_stream(in, MATRIX_DIM_UNSPECIFIED,
                                       MATRIX_DIM_UNSPECIFIED, &m);
  fclose(in);

  EXPECT_EQ(rc, MATRIX_ERR_BAD_NUMBER);
  EXPECT_EQ(m.data, nullptr);
}

/* --- matrix_write --- */

TEST(Write, TrimsTrailingZerosSoIntegersPrintAsIntegers) {
  Owned m;
  ASSERT_EQ(matrix_parse_text("1 2\n3 4\n", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  EXPECT_EQ(written(m.ptr(), MATRIX_DEFAULT_PRECISION), "1  2\n3  4\n");
}

TEST(Write, RightJustifiesIntoACommonWidth) {
  Owned m;
  ASSERT_EQ(matrix_parse_text("1 22\n333 4444\n", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  EXPECT_EQ(written(m.ptr(), MATRIX_DEFAULT_PRECISION), "   1    22\n"
                                                        " 333  4444\n");
}

TEST(Write, KeepsFractionsUpToThePrecision) {
  Owned m;
  ASSERT_EQ(matrix_parse_text("0.5 0.25", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  /* "0.5" is padded to the width of the wider "0.25" beside it. */
  EXPECT_EQ(written(m.ptr(), 4), " 0.5  0.25\n");
  EXPECT_EQ(written(m.ptr(), 0), "0  0\n");
}

TEST(Write, BreaksARoundingTieTowardsTheEvenDigit) {
  /* 0.25 and 0.5 are both exactly representable, so rounding them to one and
   * zero decimals is a genuine tie, and "%.*f" resolves it to the even digit
   * rather than away from zero: 0.2, not 0.3, and 0, not 1. Rust's formatting
   * does the same, so this is worth pinning before the other ports exist. */
  Owned m;
  ASSERT_EQ(matrix_parse_text("0.25 0.5", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  EXPECT_EQ(written(m.ptr(), 1), "0.2  0.5\n");
  EXPECT_EQ(written(m.ptr(), 0), "0  0\n");
}

TEST(Write, RoundsAThirdToThePrecision) {
  Owned m;
  ASSERT_EQ(matrix_create(1, 1, m.ptr()), MATRIX_OK);
  m.get().data[0] = 1.0 / 3.0;
  EXPECT_EQ(written(m.ptr(), 4), "0.3333\n");
  EXPECT_EQ(written(m.ptr(), 2), "0.33\n");
}

TEST(Write, NeverPrintsANegativeZero) {
  Owned m;
  ASSERT_EQ(matrix_create(1, 3, m.ptr()), MATRIX_OK);
  m.get().data[0] = -0.0;    /* the double itself */
  m.get().data[1] = -0.0001; /* rounded away by the precision */
  m.get().data[2] = -1.0;
  EXPECT_EQ(written(m.ptr(), 2), " 0   0  -1\n");
}

TEST(Write, EveryLineEndsInANewlineIncludingTheLast) {
  Owned m;
  ASSERT_EQ(matrix_parse_text("1\n2\n3\n", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  EXPECT_EQ(written(m.ptr(), MATRIX_DEFAULT_PRECISION), "1\n2\n3\n");
}

TEST(Write, NegativeValuesWidenTheColumn) {
  Owned m;
  ASSERT_EQ(matrix_parse_text("-1 2\n3 -4\n", MATRIX_DIM_UNSPECIFIED,
                              MATRIX_DIM_UNSPECIFIED, m.ptr()),
            MATRIX_OK);
  EXPECT_EQ(written(m.ptr(), MATRIX_DEFAULT_PRECISION), "-1   2\n"
                                                        " 3  -4\n");
}

TEST(FormatDefault, UsesTheDocumentedPrecision) {
  EXPECT_EQ(matrix_format_default().precision, MATRIX_DEFAULT_PRECISION);
}

TEST(Write, HandlesAWidthNoFixedBufferWouldHold) {
  /* "%.*f" of a value near DBL_MAX needs 309 digits before the point plus the
   * precision after it. A 512-byte buffer used to fail here and report it as
   * MATRIX_ERR_NOMEM, printing "out of memory: Success". */
  Owned m;
  ASSERT_EQ(matrix_create(1, 1, m.ptr()), MATRIX_OK);
  m.get().data[0] = 1e300;
  const std::string text = written(m.ptr(), 300);
  EXPECT_EQ(text.size(), 302u);
  EXPECT_EQ(text.front(), '1');
  EXPECT_EQ(text.back(), '\n');
  EXPECT_EQ(text.find('.'), std::string::npos);
}
