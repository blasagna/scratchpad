#ifndef MEMORY_OPTIMIZATION_SUPPORT_CACHEINFO_HPP
#define MEMORY_OPTIMIZATION_SUPPORT_CACHEINFO_HPP

#include <cstddef>

// The paper (§6.2.3, "Optimizing Level 2 and Higher Cache Access") stresses
// that cache-conscious code must "dynamically adjust itself to the cache line
// size" and to the cache sizes, rather than hardcoding them. It points at
// sysconf(_SC_LEVEL1_DCACHE_LINESIZE) and the getconf utility as the runtime
// way to learn them. This little helper wraps those sysconf() queries so every
// demo can size its working sets from the real machine.
//
// The values come from sysconf() on Linux; a query the platform cannot answer
// returns 0 here, and callers fall back to the documented defaults (a 64-byte
// line is universal on x86-64).

namespace memory_optimization::support {

struct CacheInfo {
  std::size_t l1d_size;  // bytes, or 0 if unknown
  std::size_t l2_size;   // bytes, or 0 if unknown
  std::size_t l3_size;   // bytes, or 0 if unknown
  std::size_t line_size; // bytes, or 0 if unknown
  std::size_t l1d_assoc; // ways, or 0 if unknown
};

// Queries the cache hierarchy via sysconf(). Cheap; callers may call it once.
CacheInfo query_cache_info();

// The L1d cache-line size in bytes, or kDefaultLineSize if sysconf cannot say.
// This is the CLS the paper's blocking factor SM = CLS / sizeof(double) is
// built from.
std::size_t cache_line_size();

// x86-64 has used a 64-byte line across every microarchitecture to date, so it
// is a safe compile-time default and the value we pin alignas() to (see the
// note on std::hardware_destructive_interference_size in the false_sharing
// demo -- the standard constant exists but trips -Winterference-size under
// -Werror, so we use a checked literal instead).
inline constexpr std::size_t kDefaultLineSize = 64;

} // namespace memory_optimization::support

#endif // MEMORY_OPTIMIZATION_SUPPORT_CACHEINFO_HPP
