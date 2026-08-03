#ifndef MATRIX_OPS_CPP_MATRIX_IO_HPP
#define MATRIX_OPS_CPP_MATRIX_IO_HPP

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

#include "matrix.hpp"

namespace matrix_ops {

// Decimal places used when the caller does not pick one. Part of the shared
// contract, so it lives in the header rather than in the CLI.
inline constexpr int kDefaultPrecision = 4;

// The most decimal places a caller may ask for. A double's smallest subnormal
// is about 5e-324, so past ~1074 places every digit printed is a zero the
// trimming then removes; 1100 is that with room to spare. The cap is not
// cosmetic: a rendering of a large value needs 309 digits before the point plus
// the precision after it, so an uncapped precision lets one cell demand
// gigabytes -- INT_MAX asked for 6.3 GB here, where the C port's snprintf
// instead overflowed the int it returns and reported it as an allocation
// failure. Neither is a useful answer, so both ports refuse the request.
inline constexpr int kMaxPrecision = 1100;

// "The caller did not specify this dimension". Zero is free for the job
// because Matrix::create rejects a zero dimension anyway.
inline constexpr std::size_t kDimUnspecified = 0;

// Outcome of a parse: a matrix, or the reason there is not one. Shaped like
// simple_logger's LogResult -- unlike add/sub/mul, parsing fails in seven
// distinct ways and the CLI has to name which.
struct ParseResult {
  Matrix matrix;
  Error error = Error::kOk;

  bool ok() const noexcept { return error == Error::kOk; }
  explicit operator bool() const noexcept { return ok(); }
};

// Builds a matrix from whitespace-separated numbers, read in row-major order.
// The shape comes from the text's own layout unless the caller overrides it:
//
//   1. A single non-blank line is a 1 x N row vector; several non-blank lines
//      are rows, one per line.
//   2. Ragged input -- lines with differing value counts -- is always kRagged,
//      even when want_rows and want_cols would make the layout irrelevant. A
//      ragged file is far more often a typo than an intent, so it is not
//      silently reshaped.
//   3. The requested dimensions are then applied:
//        - neither given: the layout's own shape
//        - both given: requires want_rows * want_cols == the value count, and
//          reshapes row-major, which is what turns a flat list of six values
//          into a 2 x 3
//        - only one given: the other is derived by division, and a count that
//          does not divide evenly is kBadShape
//
// A number is anything std::strtod accepts *except* the non-finite spellings
// (nan, inf, infinity) and anything that overflows to infinity. See the note
// in matrix_io.cpp on why this is strtod and not std::from_chars.
//
// Blank lines and surrounding whitespace are ignored.
ParseResult parse_text(std::string_view text, std::size_t want_rows,
                       std::size_t want_cols);

// Reads all of `in`, then parses it as parse_text does. The whole stream is
// buffered rather than streamed: unlike text_analyzer, where streaming was a
// requirement, the values have to be held in memory anyway. Reports kRead if
// the stream failed, and kBadNumber for an embedded NUL, which would otherwise
// truncate the text silently.
//
// Buffering the whole stream is also the one unbounded allocation on the input
// side, so like parse_text this reports kNoMem rather than letting a bad_alloc
// escape: the C port returns MATRIX_ERR_NOMEM here and the CLI contract calls
// running out of memory an operational failure (exit 1), not a crash.
ParseResult read_stream(std::istream &in, std::size_t want_rows,
                        std::size_t want_cols);

// How a matrix is rendered.
struct Format {
  // Maximum decimal places. Trailing zeros are trimmed afterwards, so this is
  // an upper bound on the fraction shown rather than a fixed width: with the
  // default of 4, 1.0 renders as "1" and 1/3 as "0.3333".
  int precision = kDefaultPrecision;
};

// Renders a matrix, one row per line, every line ending in a newline including
// the last.
//
// Every element is rendered first, then the widest rendering sets a common
// column width and all of them are right-justified into it and separated by
// two spaces, so decimal points line up down each column.
//
// Elements use std::format("{:.{}f}") followed by stripping trailing zeros and
// a bare trailing '.', so integral values print as integers. The shortest
// round-trip form ("{}") is deliberately not used: it drops into scientific
// notation for large values, which reads badly in a column and would not match
// the C port's "%.*f". A negative zero is normalized to "0".
//
// Because the width follows the data, a matrix holding a value like 1e300
// renders a column some 300 characters wide. That is left uncapped rather than
// truncating a value the user asked to see.
//
// Returning a string rather than writing to a stream keeps this pure, which is
// what lets most of the format tests be plain string comparisons -- the same
// improvement simple_logger's C++ port made over its C sibling.
// fmt.precision must be in [0, kMaxPrecision]; the CLI rejects anything else
// before it gets here.
std::string render(const Matrix &m, const Format &fmt);

// render() written to a stream. Returns kOk, kWrite with errno as the failing
// call left it, or kNoMem if the rendering could not be allocated.
Error write(std::ostream &out, const Matrix &m, const Format &fmt);

} // namespace matrix_ops

#endif // MATRIX_OPS_CPP_MATRIX_IO_HPP
