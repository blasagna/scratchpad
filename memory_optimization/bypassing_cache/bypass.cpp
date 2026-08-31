#include "memory_optimization/bypassing_cache/bypass.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

#include <emmintrin.h>
#include <xmmintrin.h>

namespace memory_optimization::bypassing_cache {

namespace {

// Index of element (i, j) walked so the inner loop is either sequential
// (row-major: j varies fastest, contiguous) or strided (column-major: i varies
// fastest, N doubles apart).
template <typename Store> void fill(Matrix &m, Store store, Order order) {
  const std::size_t n = m.n;
  double *base = m.data.data();
  if (order == Order::kRowMajor) {
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = 0; j < n; ++j) {
        store(base + i * n + j);
      }
    }
  } else {
    for (std::size_t j = 0; j < n; ++j) {
      for (std::size_t i = 0; i < n; ++i) {
        store(base + i * n + j);
      }
    }
  }
}

} // namespace

void fill_normal(Matrix &m, double value, Order order) {
  fill(m, [value](double *p) { *p = value; }, order);
}

void fill_nontemporal(Matrix &m, double value, Order order) {
  // A double is 8 bytes; _mm_stream_si64 writes 8 bytes bypassing the cache.
  const long long bits = std::bit_cast<long long>(value);
  fill(
      m,
      [bits](double *p) {
        _mm_stream_si64(reinterpret_cast<long long *>(p), bits);
      },
      order);
  // paper: "the program needs to explicitly insert memory barriers (sfence) for
  // x86" -- non-temporal stores are weakly ordered, so fence before anyone
  // reads the result back.
  _mm_sfence();
}

} // namespace memory_optimization::bypassing_cache
