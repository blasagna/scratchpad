#ifndef MEMORY_OPTIMIZATION_SUPPORT_RDTSC_HPP
#define MEMORY_OPTIMIZATION_SUPPORT_RDTSC_HPP

#include <cstdint>

#include <x86intrin.h>

// Section 6 of the paper reports most results in CPU cycles (e.g. Table 6.2's
// matrix-multiply timings), because cache and memory latencies are naturally
// expressed that way. Google Benchmark measures wall time, so where a demo
// wants the paper's unit it reads the timestamp counter directly and attaches
// the delta as a benchmark::Counter.
//
// This uses the compiler intrinsic __rdtsc() from <x86intrin.h> rather than the
// raw `rdtsc` inline asm the paper would have used in 2007 -- the intrinsic is
// warning-clean under the repo's -pedantic and lets the compiler schedule
// around it. Note the usual caveats: the TSC counts reference cycles at a fixed
// rate on modern x86 (it does not track the core's turbo frequency), and
// out-of- order execution means a bare rdtsc is not a precise instruction
// boundary. For the coarse per-benchmark-iteration accounting we do here that
// is fine; we are after ratios between variants, not absolute cycle-exact
// numbers.

namespace memory_optimization::support {

// Reads the timestamp counter.
inline std::uint64_t rdtsc() noexcept { return __rdtsc(); }

} // namespace memory_optimization::support

#endif // MEMORY_OPTIMIZATION_SUPPORT_RDTSC_HPP
