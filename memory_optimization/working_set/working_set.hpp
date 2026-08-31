#ifndef MEMORY_OPTIMIZATION_WORKING_SET_WORKING_SET_HPP
#define MEMORY_OPTIMIZATION_WORKING_SET_WORKING_SET_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// Section 6.2.3, "Optimizing Level 2 and Higher Cache Access". The recurring
// advice: know your program's working-set size and keep it within a cache when
// data is reused. As the working set grows past L1d, then L2, then L3, the
// average access cost steps up each time. The paper stresses querying the real
// cache sizes at runtime (sysconf / getconf) rather than hardcoding them, and
// sizing the work to fit.
//
// This demo sweeps a buffer size from a few KiB to hundreds of MiB, repeatedly
// touching every element, and reports the achieved bandwidth. The plateaus and
// drops line up with this machine's L1d / L2 / L3 sizes (which the benchmark
// prints, from support::query_cache_info()).

namespace memory_optimization::working_set {

// Sums `buffer` `repeats` times (reuse is what makes cache residency matter).
// Returns the accumulated sum so the work cannot be optimized away.
std::uint64_t sum_repeatedly(const std::vector<std::uint64_t> &buffer,
                             std::size_t repeats);

// Names the smallest cache level that holds `bytes` given the machine's sizes,
// e.g. "L1d", "L2", "L3", or "RAM". Used to label each sweep point.
const char *level_for_size(std::size_t bytes);

} // namespace memory_optimization::working_set

#endif // MEMORY_OPTIMIZATION_WORKING_SET_WORKING_SET_HPP
