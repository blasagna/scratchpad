#ifndef MEMORY_OPTIMIZATION_FALSE_SHARING_FALSE_SHARING_HPP
#define MEMORY_OPTIMIZATION_FALSE_SHARING_FALSE_SHARING_HPP

#include <cstddef>
#include <cstdint>

// Section 6.4.1, "Concurrency Optimizations" -- false sharing. When several
// threads each write their *own* variable, but those variables happen to sit on
// the same cache line, the coherence protocol still forces the line to bounce
// between cores: every write needs the line in the 'E'(xclusive) state, so each
// core keeps sending Request-For-Ownership messages to steal it back. The
// threads are logically independent yet serialize on the cache line.
//
// The paper's Figure 6.10 measures N threads each incrementing a counter 500M
// times, with the counters packed onto one cache line versus spread one per
// line. The packed version is several times slower and gets worse with more
// threads; the spread version scales cleanly. The fix (paper's rwstruct) is to
// pad each per-thread datum out to a full cache line so no two share one.

namespace memory_optimization::false_sharing {

// paper: the paper hardcodes CLSIZE and pads with __attribute__((aligned)). The
// modern spelling is alignas(N). The natural N is
// std::hardware_destructive_interference_size, but that constant trips GCC's
// -Winterference-size under the repo's -Werror (it warns the value is not ABI-
// stable across compiler versions), so we pin the well-known x86-64 line size
// and let the runtime check in the demo confirm it against sysconf.
inline constexpr std::size_t kCacheLine = 64;

// A counter packed with its neighbours -- adjacent Packed values share a line.
struct Packed {
  std::uint64_t value = 0;
};

// A counter padded to own a whole cache line, so no two share one.
struct alignas(kCacheLine) Padded {
  std::uint64_t value = 0;
};

// Increments `*counter` `iters` times. Uses std::atomic_ref (C++20) with
// relaxed ordering: the thread owns this counter so no cross-thread atomicity
// is required, but the atomic read-modify-write forces a real memory access
// each iteration (defeating the "fold the loop into += iters" optimization),
// which is what makes the cache-line traffic observable. This is not the
// atomicity demo -- here each address has a single writer.
void hammer(std::uint64_t *counter, std::uint64_t iters);

// Runs `threads` workers, each hammering its own counter `iters` times, and
// returns the summed total (== threads * iters). Counters are packed adjacently
// (false sharing) or padded one per line, per `padded`. Threads are released
// together via a latch so the contended window overlaps.
std::uint64_t run(std::size_t threads, std::uint64_t iters, bool padded);

} // namespace memory_optimization::false_sharing

#endif // MEMORY_OPTIMIZATION_FALSE_SHARING_FALSE_SHARING_HPP
