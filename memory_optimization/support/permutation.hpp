#ifndef MEMORY_OPTIMIZATION_SUPPORT_PERMUTATION_HPP
#define MEMORY_OPTIMIZATION_SUPPORT_PERMUTATION_HPP

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <random>
#include <vector>

// Several demos build a random single-cycle linked list: shuffle the indices
// [0, count), then link consecutive entries head-to-tail so the traversal is a
// random walk the hardware prefetcher cannot follow. The *linking* differs per
// demo (a strided buffer, a std::vector, an mmap'd region; some also fill an
// `ahead` pointer or a payload), but generating the permutation is identical,
// so it lives here once. Callers keep their own linking loop over the returned
// order, e.g. `link(order[i]) -> order[(i + 1) % count]`.

namespace memory_optimization::support {

// A random permutation of [0, count) with a fixed seed (reproducible).
inline std::vector<std::size_t> random_permutation(std::size_t count,
                                                   unsigned seed) {
  std::vector<std::size_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

} // namespace memory_optimization::support

#endif // MEMORY_OPTIMIZATION_SUPPORT_PERMUTATION_HPP
