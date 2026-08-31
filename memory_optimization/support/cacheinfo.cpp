#include "memory_optimization/support/cacheinfo.hpp"

#include <unistd.h>

#include <cstddef>

namespace memory_optimization::support {

namespace {

// sysconf returns a long that is -1 (or 0) when the name is not supported. Fold
// both into 0 so callers have one "unknown" sentinel.
std::size_t query(int name) {
  const long value = ::sysconf(name);
  return value > 0 ? static_cast<std::size_t>(value) : 0;
}

} // namespace

CacheInfo query_cache_info() {
  return CacheInfo{
      .l1d_size = query(_SC_LEVEL1_DCACHE_SIZE),
      .l2_size = query(_SC_LEVEL2_CACHE_SIZE),
      .l3_size = query(_SC_LEVEL3_CACHE_SIZE),
      .line_size = query(_SC_LEVEL1_DCACHE_LINESIZE),
      .l1d_assoc = query(_SC_LEVEL1_DCACHE_ASSOC),
  };
}

std::size_t cache_line_size() {
  const std::size_t line = query(_SC_LEVEL1_DCACHE_LINESIZE);
  return line != 0 ? line : kDefaultLineSize;
}

} // namespace memory_optimization::support
