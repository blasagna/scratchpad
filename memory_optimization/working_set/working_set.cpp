#include "memory_optimization/working_set/working_set.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memory_optimization/support/cacheinfo.hpp"

namespace memory_optimization::working_set {

std::uint64_t sum_repeatedly(const std::vector<std::uint64_t> &buffer,
                             std::size_t repeats) {
  std::uint64_t acc = 0;
  for (std::size_t r = 0; r < repeats; ++r) {
    for (std::uint64_t x : buffer) {
      acc += x;
    }
  }
  return acc;
}

const char *level_for_size(std::size_t bytes) {
  static const support::CacheInfo info = support::query_cache_info();
  if (info.l1d_size != 0 && bytes <= info.l1d_size) {
    return "L1d";
  }
  if (info.l2_size != 0 && bytes <= info.l2_size) {
    return "L2";
  }
  if (info.l3_size != 0 && bytes <= info.l3_size) {
    return "L3";
  }
  return "RAM";
}

} // namespace memory_optimization::working_set
