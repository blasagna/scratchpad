#include "matrix_io.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <vector>

namespace matrix_ops {
namespace {

// Spaces printed between columns.
constexpr std::string_view kColumnGap = "  ";

// True for the space characters that separate values *within* a line. A
// newline is excluded because it delimits rows.
bool is_inline_space(char c) {
  return c != '\n' && std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Parses one whitespace-delimited token as a finite double.
//
// This is std::strtod and not std::from_chars, which is the natural C++ choice
// and is what simple_logger's C++ port uses for integers. For doubles it
// diverges from this area's contract in two places the C port's tests pin:
//
//   - from_chars rejects a leading '+', recognizing a sign only in the
//     exponent. The contract accepts "+3" (test_matrix_io.c,
//     Numbers.AcceptsSignsDecimalsAndExponents).
//   - from_chars reports result_out_of_range on underflow. The contract
//     accepts "1e-400" as zero and refuses only the overflow to infinity
//     (Numbers.AcceptsAValueThatUnderflowsToZero).
//
// Rather than reproduce both quirks around from_chars, the port uses the same
// function the C one does and applies the same hand-written guards. Note the
// result is tested with isfinite rather than errno being tested for ERANGE:
// strtod sets ERANGE for the underflow case too, and that one is fine.
std::optional<double> parse_number(std::string_view token) {
  // strtod needs a NUL-terminated string and the token points into a buffer
  // that must not be modified, so it is copied. A stack buffer covers every
  // realistic number; anything longer is not one.
  char buf[512];
  if (token.empty() || token.size() >= sizeof(buf))
    return std::nullopt;
  token.copy(buf, token.size());
  buf[token.size()] = '\0';

  char *end = nullptr;
  const double value = std::strtod(buf, &end);

  // end == buf means nothing was consumed; a non-NUL *end means trailing junk,
  // which catches "1.2.3" and "5x".
  if (end == buf || *end != '\0')
    return std::nullopt;
  if (!std::isfinite(value))
    return std::nullopt;
  return value;
}

// The shape a text's own layout implies.
struct Layout {
  std::size_t rows = 0;
  std::size_t cols = 0;
};

// Scans text into `values`, reporting the layout it implies. A ragged input is
// rejected here, before any requested shape is considered.
Error scan_values(std::string_view text, std::vector<double> &values,
                  Layout &layout) {
  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t i = 0;

  while (i < text.size()) {
    // Collect one line's worth of values.
    std::size_t line_cols = 0;
    while (i < text.size() && text[i] != '\n') {
      if (is_inline_space(text[i])) {
        i++;
        continue;
      }

      const std::size_t start = i;
      while (i < text.size() && text[i] != '\n' && !is_inline_space(text[i]))
        i++;

      const std::optional<double> value =
          parse_number(text.substr(start, i - start));
      if (!value)
        return Error::kBadNumber;
      values.push_back(*value);
      line_cols++;
    }

    if (i < text.size() && text[i] == '\n')
      i++;

    // A blank line carries no values and is not a row; that is what lets input
    // be padded with blank lines or end in a newline.
    if (line_cols == 0)
      continue;

    if (rows == 0)
      cols = line_cols;
    else if (line_cols != cols)
      return Error::kRagged;
    rows++;
  }

  layout = {rows, cols};
  return Error::kOk;
}

// Turns the layout's shape and the requested dimensions into the final one,
// per the rules documented on parse_text.
Error resolve_shape(std::size_t count, Layout layout, std::size_t want_rows,
                    std::size_t want_cols, std::size_t &rows_out,
                    std::size_t &cols_out) {
  if (want_rows == kDimUnspecified && want_cols == kDimUnspecified) {
    rows_out = layout.rows;
    cols_out = layout.cols;
    return Error::kOk;
  }

  if (want_rows != kDimUnspecified && want_cols != kDimUnspecified) {
    // Guard the product before comparing it, or a wrapped want_rows *
    // want_cols could equal count and produce a matrix of the wrong shape.
    if (want_rows > std::numeric_limits<std::size_t>::max() / want_cols)
      return Error::kOverflow;
    if (want_rows * want_cols != count)
      return Error::kBadShape;
    rows_out = want_rows;
    cols_out = want_cols;
    return Error::kOk;
  }

  // Exactly one dimension is given here, since the two cases above returned
  // for neither and for both. The branches are spelled out rather than sharing
  // a "given" variable so that each divisor is visibly the one the enclosing
  // condition just proved nonzero.
  if (want_rows != kDimUnspecified) {
    if (count % want_rows != 0)
      return Error::kBadShape;
    rows_out = want_rows;
    cols_out = count / want_rows;
    return Error::kOk;
  }

  if (count % want_cols != 0)
    return Error::kBadShape;
  rows_out = count / want_cols;
  cols_out = want_cols;
  return Error::kOk;
}

// Renders one element, trimming trailing zeros so an integral value prints as
// an integer.
std::string format_element(double value, int precision) {
  std::string text = std::format("{:.{}f}", value, precision);

  // Only a rendering that actually has a fraction can be trimmed; with
  // precision 0 there is no '.' and the digits are all significant.
  if (text.find('.') != std::string::npos) {
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.')
      text.pop_back();
  }

  // Normalize a negative zero. It arrives two ways: as the double -0.0, and as
  // a small negative value the precision rounded away (-0.0001 at precision
  // 2). Checking the rendering rather than the value catches both, and "-0" in
  // a result reads as a bug rather than as arithmetic.
  if (text == "-0")
    text = "0";
  return text;
}

} // namespace

ParseResult parse_text(std::string_view text, std::size_t want_rows,
                       std::size_t want_cols) {
  std::vector<double> values;
  Layout layout;

  if (const Error error = scan_values(text, values, layout);
      error != Error::kOk)
    return {{}, error};

  if (values.empty())
    return {{}, Error::kEmpty};

  std::size_t rows = 0;
  std::size_t cols = 0;
  if (const Error error = resolve_shape(values.size(), layout, want_rows,
                                        want_cols, rows, cols);
      error != Error::kOk)
    return {{}, error};

  std::optional<Matrix> m = Matrix::create(rows, cols);
  if (!m)
    return {{}, Error::kOverflow};

  // The values are already in row-major order, and resolve_shape has
  // established that there are exactly rows * cols of them.
  std::ranges::copy(values, m->values().begin());
  return {*std::move(m), Error::kOk};
}

ParseResult read_stream(std::istream &in, std::size_t want_rows,
                        std::size_t want_cols) {
  // errno is cleared first so the check below cannot pick up a stale value
  // from an unrelated earlier call.
  errno = 0;
  std::ostringstream buffer;
  buffer << in.rdbuf();

  // badbit alone is not enough. A failed read through rdbuf() -- reading a
  // directory is the easy way to see it -- surfaces as "no characters were
  // inserted", which sets the *destination's* failbit and leaves `in` looking
  // like a clean empty stream. Reporting that as kEmpty would turn an
  // operational I/O failure (exit 1) into a usage error (exit 2), which is
  // what the C port, checking ferror(), gets right.
  if (in.bad() || (buffer.fail() && errno != 0))
    return {{}, Error::kRead};

  const std::string text = buffer.str();

  // A NUL in the input would end the text early for a NUL-terminated parser.
  // string_view carries its own length so it would not here, but rejecting it
  // keeps this port's accepted set identical to the C one's.
  if (text.find('\0') != std::string::npos)
    return {{}, Error::kBadNumber};

  return parse_text(text, want_rows, want_cols);
}

std::string render(const Matrix &m, const Format &fmt) {
  // Every element is rendered before anything is emitted, because the widest
  // one sets the column width they are all justified into.
  std::vector<std::string> cells;
  cells.reserve(m.rows() * m.cols());
  std::size_t width = 0;
  for (const double value : m.values()) {
    cells.push_back(format_element(value, fmt.precision));
    width = std::max(width, cells.back().size());
  }

  std::string out;
  for (std::size_t r = 0; r < m.rows(); r++) {
    for (std::size_t c = 0; c < m.cols(); c++) {
      if (c > 0)
        out += kColumnGap;
      // Right-justified into the common width, so decimal points line up down
      // each column.
      out += std::format("{:>{}}", cells[r * m.cols() + c], width);
    }
    out += '\n';
  }
  return out;
}

Error write(std::ostream &out, const Matrix &m, const Format &fmt) {
  const std::string text = render(m, fmt);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out ? Error::kOk : Error::kWrite;
}

} // namespace matrix_ops
