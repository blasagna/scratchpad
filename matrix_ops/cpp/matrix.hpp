#ifndef MATRIX_OPS_CPP_MATRIX_HPP
#define MATRIX_OPS_CPP_MATRIX_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace matrix_ops {

// What went wrong. Mirrors the C port's MatrixResult one-for-one so the two
// report the same failure for the same input; kRead and kWrite are the only
// ones backed by a system call, and leave errno in place for the caller to
// pair with a message.
enum class Error {
  kOk,
  kDimMismatch, // add/sub shapes differ, or mul inner dims differ
  kBadNumber,   // a token was not a finite number
  kRagged,      // input rows have differing column counts
  kEmpty,       // the input held no values
  kBadShape,    // requested dimensions do not fit the value count
  kRead,        // a read error occurred on the input stream
  kWrite,       // a write error occurred on the output stream
  kOverflow,    // rows * cols is larger than can be allocated
};

// Returns a short human-readable label for an Error.
std::string_view describe(Error error);

// A dense 2D matrix of doubles, stored row-major.
//
// There is no public constructor that can fail silently: create() rejects a
// zero dimension, so no Matrix a caller holds is ever empty, and nothing
// downstream has to reason about a degenerate shape. A default-constructed
// Matrix exists only as a placeholder inside ParseResult.
class Matrix {
public:
  Matrix() = default;

  // Allocates a rows x cols matrix with every element zero. Returns nullopt
  // for a zero dimension or a product too large to allocate.
  static std::optional<Matrix> create(std::size_t rows, std::size_t cols);

  std::size_t rows() const noexcept { return rows_; }
  std::size_t cols() const noexcept { return cols_; }
  bool empty() const noexcept { return data_.empty(); }

  // Element (row, col). The indices are a caller precondition -- row < rows()
  // and col < cols() -- and are not checked, since every caller here derives
  // them from the matrix's own dimensions.
  double at(std::size_t row, std::size_t col) const {
    return data_[row * cols_ + col];
  }
  double &at(std::size_t row, std::size_t col) {
    return data_[row * cols_ + col];
  }

  // The elements in row-major order. Used by the parser to fill a matrix in
  // one copy, and by the tests to compare one whole.
  std::span<const double> values() const noexcept { return data_; }
  std::span<double> values() noexcept { return data_; }

private:
  std::size_t rows_ = 0;
  std::size_t cols_ = 0;
  std::vector<double> data_;
};

// Element-wise sum and difference. Both operands must have identical
// dimensions; nullopt means they did not, which is the only way these fail.
std::optional<Matrix> add(const Matrix &a, const Matrix &b);
std::optional<Matrix> sub(const Matrix &a, const Matrix &b);

// Matrix product. `a` is m x n and `b` must be n x p -- only the shared inner
// dimension has to agree, and the product is m x p. Not commutative, so the
// argument order is the CLI's operand order. nullopt means a->cols != b->rows.
std::optional<Matrix> mul(const Matrix &a, const Matrix &b);

// Multiplies every element by scalar. Cannot fail, so unlike its C
// counterpart it returns a Matrix rather than a status.
Matrix scale(const Matrix &a, double scalar);

} // namespace matrix_ops

#endif // MATRIX_OPS_CPP_MATRIX_HPP
