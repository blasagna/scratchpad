#include "memory_optimization/matrix_multiply/matmul.hpp"

#include <algorithm>
#include <cstddef>
#include <random>

#include "memory_optimization/support/cacheinfo.hpp"

namespace memory_optimization::matrix_multiply {

Matrix random_matrix(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  Matrix m(n);
  for (auto &x : m.data) {
    x = dist(rng);
  }
  return m;
}

Matrix mul_naive(const Matrix &a, const Matrix &b) {
  const std::size_t n = a.n;
  Matrix res(n);
  // paper: the exact loop nest from §6.2.1 -- the inner k loop reads b down a
  // column (b.at(k, j)), the access pattern the whole example sets out to fix.
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      double sum = 0.0;
      for (std::size_t k = 0; k < n; ++k) {
        sum += a.at(i, k) * b.at(k, j);
      }
      res.at(i, j) = sum;
    }
  }
  return res;
}

Matrix mul_transposed(const Matrix &a, const Matrix &b) {
  const std::size_t n = a.n;
  // paper: "rearrange (transpose) the second matrix mul2 before using it." The
  // temporary costs N*N, recovered because the inner loop now walks tmp
  // row-wise (tmp.at(j, k)) instead of b column-wise.
  Matrix tmp(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      tmp.at(j, i) = b.at(i, j);
    }
  }

  Matrix res(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      double sum = 0.0;
      for (std::size_t k = 0; k < n; ++k) {
        sum += a.at(i, k) * tmp.at(j, k);
      }
      res.at(i, j) = sum;
    }
  }
  return res;
}

Matrix mul_blocked(const Matrix &a, const Matrix &b) {
  const std::size_t n = a.n;
  Matrix res(n);

  // paper: #define SM (CLS / sizeof(double)). SM sub-blocks of each matrix fit
  // in one cache line's worth of doubles, so an SM x SM tile stays in L1d.
  const std::size_t sm = support::cache_line_size() / sizeof(double);

  const double *mul1 = a.data.data();
  const double *mul2 = b.data.data();
  double *rmat = res.data.data();

  // paper: the six-nested-loop scheme (p.50). We keep the raw row-pointer
  // arithmetic (rres/rmul1/rmul2 advancing by N) so the inner loops read as the
  // paper's do; the modern touch is that the buffers come from std::vector.
  //
  // Divergence from the listing: the paper assumes N % SM == 0 and lets the
  // inner loops run a full SM. We clamp to the block's real extent so any N
  // works, which the correctness test exercises with non-multiple sizes.
  for (std::size_t i = 0; i < n; i += sm) {
    const std::size_t i_end = std::min(i + sm, n);
    for (std::size_t j = 0; j < n; j += sm) {
      const std::size_t j_end = std::min(j + sm, n);
      for (std::size_t k = 0; k < n; k += sm) {
        const std::size_t k_end = std::min(k + sm, n);
        for (std::size_t i2 = i; i2 < i_end; ++i2) {
          double *rres = rmat + i2 * n;
          const double *rmul1 = mul1 + i2 * n;
          for (std::size_t k2 = k; k2 < k_end; ++k2) {
            const double *rmul2 = mul2 + k2 * n;
            const double a_ik = rmul1[k2];
            for (std::size_t j2 = j; j2 < j_end; ++j2) {
              rres[j2] += a_ik * rmul2[j2];
            }
          }
        }
      }
    }
  }
  return res;
}

} // namespace memory_optimization::matrix_multiply
