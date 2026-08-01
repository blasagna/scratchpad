#include "matrix.hpp"

#include <utility>

namespace matrix_ops {
namespace {

// Shared body of add and sub: both walk the elements in step and differ only
// in the sign applied to b. Passing the sign rather than a callable keeps the
// inner loop a plain multiply-add, as in the C port.
std::optional<Matrix> add_scaled(const Matrix &a, const Matrix &b,
                                 double b_sign) {
  if (a.rows() != b.rows() || a.cols() != b.cols())
    return std::nullopt;

  std::optional<Matrix> result = Matrix::create(a.rows(), a.cols());
  if (!result)
    return std::nullopt;

  const std::span<const double> lhs = a.values();
  const std::span<const double> rhs = b.values();
  const std::span<double> out = result->values();
  for (std::size_t i = 0; i < out.size(); i++)
    out[i] = lhs[i] + b_sign * rhs[i];

  return result;
}

} // namespace

std::string_view describe(Error error) {
  switch (error) {
  case Error::kOk:
    return "success";
  case Error::kDimMismatch:
    return "incompatible matrix dimensions";
  case Error::kBadNumber:
    return "value is not a finite number";
  case Error::kRagged:
    return "rows do not all have the same number of values";
  case Error::kEmpty:
    return "no values given";
  case Error::kBadShape:
    return "requested dimensions do not fit the values";
  case Error::kRead:
    return "error reading input";
  case Error::kWrite:
    return "error writing output";
  case Error::kOverflow:
    return "matrix is too large";
  }
  return "unknown error";
}

std::optional<Matrix> Matrix::create(std::size_t rows, std::size_t cols) {
  // Every operation here is defined in terms of elements, so a matrix with no
  // elements has no useful answer to give.
  if (rows == 0 || cols == 0)
    return std::nullopt;

  // Checked before multiplying, not after: the wrapped product would be a
  // plausible-looking small number and the vector would happily hold it. The C
  // port guards SIZE_MAX; here max_size() is the tighter and more honest bound.
  std::vector<double> data;
  if (rows > data.max_size() / cols)
    return std::nullopt;

  Matrix m;
  m.rows_ = rows;
  m.cols_ = cols;
  m.data_.assign(rows * cols, 0.0);
  return m;
}

std::optional<Matrix> add(const Matrix &a, const Matrix &b) {
  return add_scaled(a, b, 1.0);
}

std::optional<Matrix> sub(const Matrix &a, const Matrix &b) {
  return add_scaled(a, b, -1.0);
}

std::optional<Matrix> mul(const Matrix &a, const Matrix &b) {
  // Only the inner dimension has to agree; the outer two become the result's
  // shape.
  if (a.cols() != b.rows())
    return std::nullopt;

  std::optional<Matrix> result = Matrix::create(a.rows(), b.cols());
  if (!result)
    return std::nullopt;

  for (std::size_t i = 0; i < a.rows(); i++) {
    for (std::size_t j = 0; j < b.cols(); j++) {
      double sum = 0.0;
      for (std::size_t k = 0; k < a.cols(); k++)
        sum += a.at(i, k) * b.at(k, j);
      result->at(i, j) = sum;
    }
  }

  return result;
}

Matrix scale(const Matrix &a, double scalar) {
  // A default-constructed Matrix is 0x0, which create() refuses. That cannot
  // arrive from the CLI -- every matrix there comes from create() via the
  // parser -- but the type permits it, so scaling an empty matrix yields an
  // empty matrix rather than dereferencing an empty optional.
  std::optional<Matrix> result = Matrix::create(a.rows(), a.cols());
  if (!result)
    return Matrix{};

  const std::span<const double> in = a.values();
  const std::span<double> out = result->values();
  for (std::size_t i = 0; i < out.size(); i++)
    out[i] = in[i] * scalar;
  return *std::move(result);
}

} // namespace matrix_ops
