#ifndef MEMORY_OPTIMIZATION_BYPASSING_CACHE_BYPASS_HPP
#define MEMORY_OPTIMIZATION_BYPASSING_CACHE_BYPASS_HPP

#include <cstddef>
#include <vector>

// Section 6.1, "Bypassing the Cache". When data is produced but not consumed
// again soon -- filling a large matrix that is used much later -- pulling each
// cache line in to modify it is wasteful: it evicts data that will be needed
// and caches data that will not. x86 provides *non-temporal* store intrinsics
// (_mm_stream_*) that write straight to memory through the write-combining
// buffer, skipping the cache entirely. The C runtime's memset for large blocks
// uses exactly this.
//
// The paper's Table 6.1 measures initializing a large matrix two ways (normal
// vs non-temporal stores) and with two inner-loop orders (row = sequential,
// column = strided). The findings this demo reproduces:
//   - Sequential writes are just as fast bypassing the cache as through it
//     (write-combining fills whole lines either way), while *saving* the cache.
//   - Column-order writes are much slower: each touches a new line, so no
//     write-combining is possible and non-temporal is if anything worse.

namespace memory_optimization::bypassing_cache {

// A dense N x N matrix of doubles, row-major.
struct Matrix {
  std::size_t n = 0;
  std::vector<double> data;

  Matrix() = default;
  explicit Matrix(std::size_t size) : n(size), data(size * size, 0.0) {}
};

enum class Order { kRowMajor, kColMajor };

// Fills every element with `value` using ordinary stores (the cache is used).
void fill_normal(Matrix &m, double value, Order order);

// Fills every element with `value` using non-temporal stores (_mm_stream_si64,
// SSE2), which bypass the cache, followed by an _mm_sfence() to publish them.
//
// paper: the paper streams 16 bytes at a time with _mm_stream_si128. We stream
// one 8-byte double at a time with _mm_stream_si64 so the *same* per-element
// loop serves both row and column order -- streaming 128-bit vectors only makes
// sense for the contiguous row case. C++ has no portable non-temporal store, so
// the intrinsic stays.
void fill_nontemporal(Matrix &m, double value, Order order);

} // namespace memory_optimization::bypassing_cache

#endif // MEMORY_OPTIMIZATION_BYPASSING_CACHE_BYPASS_HPP
