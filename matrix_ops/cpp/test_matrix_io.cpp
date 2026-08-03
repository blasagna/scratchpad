#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "matrix_io.hpp"

namespace {

using matrix_ops::Error;
using matrix_ops::kDimUnspecified;
using matrix_ops::Matrix;
using matrix_ops::ParseResult;

std::vector<double> elements(const Matrix &m) {
  return std::vector<double>(m.values().begin(), m.values().end());
}

// Parses text, asserts the resulting shape, and returns the values in
// row-major order.
std::vector<double> parsed(std::string_view text, std::size_t want_rows,
                           std::size_t want_cols, std::size_t expect_rows,
                           std::size_t expect_cols) {
  const ParseResult result = matrix_ops::parse_text(text, want_rows, want_cols);
  EXPECT_TRUE(result.ok()) << "text: " << text;
  if (!result)
    return {};
  EXPECT_EQ(result.matrix.rows(), expect_rows);
  EXPECT_EQ(result.matrix.cols(), expect_cols);
  return elements(result.matrix);
}

// Parses text expecting a failure, and returns the error.
Error parse_error(std::string_view text,
                  std::size_t want_rows = kDimUnspecified,
                  std::size_t want_cols = kDimUnspecified) {
  const ParseResult result = matrix_ops::parse_text(text, want_rows, want_cols);
  EXPECT_FALSE(result.ok()) << "text: " << text;
  EXPECT_TRUE(result.matrix.empty()) << "text: " << text;
  return result.error;
}

// Renders a matrix at the given precision.
std::string written(const Matrix &m, int precision) {
  matrix_ops::Format fmt;
  fmt.precision = precision;
  return matrix_ops::render(m, fmt);
}

// Parses text that is expected to succeed, for the render tests.
Matrix must_parse(std::string_view text) {
  ParseResult result =
      matrix_ops::parse_text(text, kDimUnspecified, kDimUnspecified);
  EXPECT_TRUE(result.ok()) << "text: " << text;
  return std::move(result.matrix);
}

} // namespace

/* --- shape inference from layout --- */

TEST(Shape, ASingleLineIsARowVector) {
  EXPECT_EQ(parsed("1 2 3", kDimUnspecified, kDimUnspecified, 1, 3),
            (std::vector<double>{1, 2, 3}));
}

TEST(Shape, ASingleValueIsOneByOne) {
  EXPECT_EQ(parsed("7", kDimUnspecified, kDimUnspecified, 1, 1),
            std::vector<double>{7});
}

TEST(Shape, NewlinesDelimitRows) {
  EXPECT_EQ(parsed("1 2 3\n4 5 6\n", kDimUnspecified, kDimUnspecified, 2, 3),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, AFinalNewlineIsOptional) {
  EXPECT_EQ(parsed("1 2\n3 4", kDimUnspecified, kDimUnspecified, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, BlankLinesAndSurroundingWhitespaceAreIgnored) {
  EXPECT_EQ(parsed("\n\n  1   2  \n\n  3   4  \n\n", kDimUnspecified,
                   kDimUnspecified, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, TabsSeparateValuesAndDoNotStartARow) {
  EXPECT_EQ(parsed("1\t2\n3\t4\n", kDimUnspecified, kDimUnspecified, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, CarriageReturnsAreJustWhitespace) {
  // A CRLF file parses the same as an LF one.
  EXPECT_EQ(parsed("1 2\r\n3 4\r\n", kDimUnspecified, kDimUnspecified, 2, 2),
            (std::vector<double>{1, 2, 3, 4}));
}

TEST(Shape, RaggedRowsAreAlwaysAnError) {
  EXPECT_EQ(parse_error("1 2 3\n4 5\n"), Error::kRagged);
  // Even when the requested dimensions would make the layout irrelevant: a
  // ragged file is more often a typo than an intent.
  EXPECT_EQ(parse_error("1 2 3\n4 5\n", 1, 5), Error::kRagged);
}

TEST(Shape, EmptyInputIsAnError) {
  EXPECT_EQ(parse_error(""), Error::kEmpty);
  EXPECT_EQ(parse_error("   \n\n  \n"), Error::kEmpty);
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
  // A 2x3 text read as 3x2. The values keep their row-major order.
  EXPECT_EQ(parsed("1 2 3\n4 5 6\n", 3, 2, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, BothDimensionsMustMatchTheValueCount) {
  EXPECT_EQ(parse_error("1 2 3 4 5 6", 2, 4), Error::kBadShape);
  EXPECT_EQ(parse_error("1 2 3 4 5 6", 2, 2), Error::kBadShape);
}

TEST(Shape, RowsAloneDerivesTheColumns) {
  EXPECT_EQ(parsed("1 2 3 4 5 6", 2, kDimUnspecified, 2, 3),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(parsed("1 2 3 4 5 6", 3, kDimUnspecified, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, ColsAloneDerivesTheRows) {
  EXPECT_EQ(parsed("1 2 3 4 5 6", kDimUnspecified, 2, 3, 2),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(parsed("1 2 3 4 5 6", kDimUnspecified, 6, 1, 6),
            (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(Shape, ASingleDimensionMustDivideTheValueCount) {
  EXPECT_EQ(parse_error("1 2 3 4 5", 2, kDimUnspecified), Error::kBadShape);
  EXPECT_EQ(parse_error("1 2 3 4 5", kDimUnspecified, 2), Error::kBadShape);
}

/* --- number parsing --- */

TEST(Numbers, AcceptsSignsDecimalsAndExponents) {
  // "+3" is why this port uses strtod rather than std::from_chars, which
  // recognizes a sign only in the exponent. See matrix_io.cpp.
  EXPECT_EQ(parsed("-2.5 +3 1e3 -1.5E-2 .5 4.", kDimUnspecified,
                   kDimUnspecified, 1, 6),
            (std::vector<double>{-2.5, 3, 1000, -0.015, 0.5, 4}));
}

TEST(Numbers, RejectsNonNumericTokens) {
  EXPECT_EQ(parse_error("1 abc 3"), Error::kBadNumber);
  EXPECT_EQ(parse_error("1 2 3x"), Error::kBadNumber);
  EXPECT_EQ(parse_error("1.2.3"), Error::kBadNumber);
  EXPECT_EQ(parse_error("--5"), Error::kBadNumber);
}

TEST(Numbers, RejectsTheNonFiniteSpellingsStrtodWouldAccept) {
  // strtod parses all of these; the accepted set is pinned here instead so the
  // three ports match rather than each inheriting its platform's.
  EXPECT_EQ(parse_error("nan"), Error::kBadNumber);
  EXPECT_EQ(parse_error("1 inf 3"), Error::kBadNumber);
  EXPECT_EQ(parse_error("infinity"), Error::kBadNumber);
  EXPECT_EQ(parse_error("-INF"), Error::kBadNumber);
}

TEST(Numbers, RejectsAValueTooLargeToRepresent) {
  EXPECT_EQ(parse_error("1e400"), Error::kBadNumber);
}

TEST(Numbers, AcceptsAValueThatUnderflowsToZero) {
  // strtod sets ERANGE here too, but a flush to zero is a usable answer where
  // an overflow to infinity is not -- which is the second reason this port
  // does not use std::from_chars, whose result_out_of_range covers both.
  const ParseResult result =
      matrix_ops::parse_text("1e-400", kDimUnspecified, kDimUnspecified);
  ASSERT_TRUE(result.ok());
  EXPECT_NEAR(result.matrix.values()[0], 0.0, 1e-300);
}

/* --- read_stream --- */

TEST(ReadStream, ParsesAStream) {
  std::istringstream in{"1 2 3\n4 5 6\n"};
  const ParseResult result =
      matrix_ops::read_stream(in, kDimUnspecified, kDimUnspecified);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.matrix.rows(), 2u);
  EXPECT_EQ(result.matrix.cols(), 3u);
  EXPECT_EQ(elements(result.matrix), (std::vector<double>{1, 2, 3, 4, 5, 6}));
}

TEST(ReadStream, HonorsRequestedDimensions) {
  std::istringstream in{"1 2 3 4 5 6"};
  const ParseResult result = matrix_ops::read_stream(in, 3, 2);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.matrix.rows(), 3u);
  EXPECT_EQ(result.matrix.cols(), 2u);
}

TEST(ReadStream, ReadsALargeStream) {
  std::string text;
  constexpr std::size_t kCount = 5000;
  for (std::size_t i = 0; i < kCount; i++)
    text += "1 ";

  std::istringstream in{text};
  const ParseResult result =
      matrix_ops::read_stream(in, kDimUnspecified, kDimUnspecified);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.matrix.rows(), 1u);
  EXPECT_EQ(result.matrix.cols(), kCount);
}

TEST(ReadStream, EmptyStreamIsEmptyNotAReadError) {
  std::istringstream in{""};
  const ParseResult result =
      matrix_ops::read_stream(in, kDimUnspecified, kDimUnspecified);
  EXPECT_EQ(result.error, Error::kEmpty);
  EXPECT_TRUE(result.matrix.empty());
}

TEST(ReadStream, RejectsAnEmbeddedNul) {
  // string_view would survive a NUL, but the C port's NUL-terminated parser
  // would not, so it is refused in both to keep the accepted sets identical.
  std::istringstream in{std::string("1 2\0 3 4", 8)};
  const ParseResult result =
      matrix_ops::read_stream(in, kDimUnspecified, kDimUnspecified);
  EXPECT_EQ(result.error, Error::kBadNumber);
  EXPECT_TRUE(result.matrix.empty());
}

/* --- render --- */

TEST(Render, TrimsTrailingZerosSoIntegersPrintAsIntegers) {
  const Matrix m = must_parse("1 2\n3 4\n");
  EXPECT_EQ(written(m, matrix_ops::kDefaultPrecision), "1  2\n3  4\n");
}

TEST(Render, RightJustifiesIntoACommonWidth) {
  const Matrix m = must_parse("1 22\n333 4444\n");
  EXPECT_EQ(written(m, matrix_ops::kDefaultPrecision), "   1    22\n"
                                                       " 333  4444\n");
}

TEST(Render, KeepsFractionsUpToThePrecision) {
  const Matrix m = must_parse("0.5 0.25");
  // "0.5" is padded to the width of the wider "0.25" beside it.
  EXPECT_EQ(written(m, 4), " 0.5  0.25\n");
  EXPECT_EQ(written(m, 0), "0  0\n");
}

TEST(Render, BreaksARoundingTieTowardsTheEvenDigit) {
  // 0.25 and 0.5 are both exactly representable, so rounding them to one and
  // zero decimals is a genuine tie, and std::format resolves it to the even
  // digit rather than away from zero -- the same rule printf uses, which is
  // what keeps this port byte-identical to the C one.
  const Matrix m = must_parse("0.25 0.5");
  EXPECT_EQ(written(m, 1), "0.2  0.5\n");
  EXPECT_EQ(written(m, 0), "0  0\n");
}

TEST(Render, RoundsAThirdToThePrecision) {
  std::optional<Matrix> m = Matrix::create(1, 1);
  ASSERT_TRUE(m.has_value());
  m->values()[0] = 1.0 / 3.0;
  EXPECT_EQ(written(*m, 4), "0.3333\n");
  EXPECT_EQ(written(*m, 2), "0.33\n");
}

TEST(Render, NeverPrintsANegativeZero) {
  std::optional<Matrix> m = Matrix::create(1, 3);
  ASSERT_TRUE(m.has_value());
  m->values()[0] = -0.0;    // the double itself
  m->values()[1] = -0.0001; // rounded away by the precision
  m->values()[2] = -1.0;
  EXPECT_EQ(written(*m, 2), " 0   0  -1\n");
}

TEST(Render, EveryLineEndsInANewlineIncludingTheLast) {
  const Matrix m = must_parse("1\n2\n3\n");
  EXPECT_EQ(written(m, matrix_ops::kDefaultPrecision), "1\n2\n3\n");
}

TEST(Render, NegativeValuesWidenTheColumn) {
  const Matrix m = must_parse("-1 2\n3 -4\n");
  EXPECT_EQ(written(m, matrix_ops::kDefaultPrecision), "-1   2\n"
                                                       " 3  -4\n");
}

TEST(Write, WritesWhatRenderProduces) {
  const Matrix m = must_parse("1 2\n3 4\n");
  matrix_ops::Format fmt;
  std::ostringstream out;
  EXPECT_EQ(matrix_ops::write(out, m, fmt), Error::kOk);
  EXPECT_EQ(out.str(), matrix_ops::render(m, fmt));
}

TEST(FormatDefault, UsesTheDocumentedPrecision) {
  EXPECT_EQ(matrix_ops::Format{}.precision, matrix_ops::kDefaultPrecision);
}

TEST(Render, RendersAtTheMaximumPrecision) {
  // The ceiling the CLI enforces has to be one this can actually render: the
  // widest possible cell is 309 digits, a point, and kMaxPrecision more. Every
  // digit past ~1074 is a zero the trimming removes, which is what makes this
  // a fine place to stop.
  std::optional<Matrix> m = Matrix::create(1, 1);
  ASSERT_TRUE(m.has_value());
  m->values()[0] = 0.5;
  EXPECT_EQ(written(*m, matrix_ops::kMaxPrecision), "0.5\n");
}

TEST(Render, HandlesAWidthNoFixedBufferWouldHold) {
  // "%.*f" of a value near DBL_MAX needs 309 digits before the point plus the
  // precision after it. The C port's 512-byte buffer used to fail here and
  // report it as an allocation failure.
  std::optional<Matrix> m = Matrix::create(1, 1);
  ASSERT_TRUE(m.has_value());
  m->values()[0] = 1e300;
  const std::string text = written(*m, 300);

  // The rendering needs 602 bytes on the way through -- 301 digits, the point,
  // and 300 zeros -- which is what overran the C port's fixed buffer. What
  // comes out is shorter, because the trailing zeros are then trimmed: 301
  // digits and a newline, with no '.' left for an integral value.
  EXPECT_EQ(text.size(), 302u);
  EXPECT_EQ(text.front(), '1');
  EXPECT_EQ(text.back(), '\n');
  EXPECT_EQ(text.find('.'), std::string::npos);
}

TEST(ReadStream, ReportsAReadFailureRatherThanEmptiness) {
  // A stream whose read fails must be kRead (an operational error, exit 1),
  // not kEmpty (a usage error, exit 2).
  std::ifstream dir{"/tmp", std::ios::binary};
  if (!dir.is_open())
    GTEST_SKIP() << "cannot open a directory as a stream here";
  const ParseResult result =
      matrix_ops::read_stream(dir, kDimUnspecified, kDimUnspecified);
  EXPECT_EQ(result.error, Error::kRead);
}
