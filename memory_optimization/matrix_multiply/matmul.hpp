#ifndef MEMORY_OPTIMIZATION_MATRIX_MULTIPLY_MATMUL_HPP
#define MEMORY_OPTIMIZATION_MATRIX_MULTIPLY_MATMUL_HPP

#include <cstddef>
#include <vector>

// Section 6.2.1, "Optimizing Level 1 Data Cache Access" -- the paper's central
// worked example. Multiplying two N x N double matrices the textbook way,
//
//     for (i) for (j) for (k) res[i][j] += mul1[i][k] * mul2[k][j];
//
// walks mul2 down a column in the inner loop (k indexes the row of mul2), so
// each inner step jumps N doubles ahead and touches a fresh cache line. Every
// element of the result re-streams all of mul2 through the cache. The paper
// shows two fixes that leave the math identical but change the access pattern:
//
//   1. Transpose mul2 first, so the inner loop walks it sequentially
//   (Table 6.2:
//      ~76% faster despite the extra copy).
//   2. Block/tile the loops so each SM x SM sub-block stays resident in L1d
//      (SM = cache-line size / sizeof(double)); faster still.
//
// All variants below compute the same product; the correctness test pins that.

namespace memory_optimization::matrix_multiply {

// A dense N x N matrix of doubles, stored row-major -- the layout the paper
// assumes ("the compiler lays out the matrix in memory so that ... the leftmost
// index addresses the row").
struct Matrix {
  std::size_t n = 0;
  std::vector<double> data;

  Matrix() = default;
  explicit Matrix(std::size_t size) : n(size), data(size * size, 0.0) {}

  double &at(std::size_t i, std::size_t j) { return data[i * n + j]; }
  double at(std::size_t i, std::size_t j) const { return data[i * n + j]; }
};

// The naive ijk product. This is the version with the cache problem.
Matrix mul_naive(const Matrix &a, const Matrix &b);

// Transposes b once, then multiplies with a sequential inner loop over both
// operands. The transpose costs N*N writes but removes N per-element column
// walks (§6.2.1, "traditionally indicated by a superscript 'T'").
Matrix mul_transposed(const Matrix &a, const Matrix &b);

// Blocked/tiled product. Uses the paper's six-nested-loop scheme with blocking
// factor SM = cache_line_size / sizeof(double) so each sub-block of all three
// matrices fits in L1d at once. No transpose and no extra memory.
//
// The paper's listing assumes N is a multiple of SM; this version clamps the
// inner block bounds so it works for any N (the clamp is the one deliberate
// divergence, noted at the call site).
Matrix mul_blocked(const Matrix &a, const Matrix &b);

// An n x n matrix filled with reproducible pseudo-random values in [-1, 1].
// Used by both the correctness test and the benchmark so they exercise the same
// inputs; `seed` selects the sequence.
Matrix random_matrix(std::size_t n, unsigned seed);

} // namespace memory_optimization::matrix_multiply

#endif // MEMORY_OPTIMIZATION_MATRIX_MULTIPLY_MATMUL_HPP
