#include "memory_optimization/software_prefetch/software_prefetch.hpp"

#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>

#include "memory_optimization/support/rdtsc.hpp"

// Reproduces Figure 6.7: cycles per list element for a randomized pointer
// chase, with and without software prefetch, as the working set grows. The two
// curves track until the list exceeds the last-level cache; past that the plain
// walk stalls on the full memory latency each hop while the prefetching walk
// hides part of it.
//
//   bazel run -c opt
//   //memory_optimization/software_prefetch:bench_software_prefetch

namespace memory_optimization::software_prefetch {
namespace {

constexpr std::size_t kDistance = 8; // prefetch look-ahead, ~paper's 5
constexpr std::size_t kSteps = 8'000'000;

template <std::uint64_t (ChaseList::*Chase)(std::size_t) const>
void run(benchmark::State &state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  ChaseList list(count, kDistance);

  std::uint64_t cycles = 0;
  for (auto _ : state) {
    const std::uint64_t start = support::rdtsc();
    std::uint64_t sum = (list.*Chase)(kSteps);
    cycles += support::rdtsc() - start;
    benchmark::DoNotOptimize(sum);
  }
  state.counters["cycles_per_element"] =
      benchmark::Counter(static_cast<double>(cycles) /
                         (static_cast<double>(state.iterations()) * kSteps));
  state.counters["working_set_KiB"] = benchmark::Counter(
      static_cast<double>(list.working_set_bytes()) / 1024.0);
}

// Node counts spanning L2 (1 MiB => 16Ki nodes) through well past L3 (32 MiB
// => 512Ki nodes). Each node is 64 bytes.
BENCHMARK(run<&ChaseList::chase_plain>)
    ->RangeMultiplier(4)
    ->Range(1 << 10, 1 << 21)
    ->ArgName("nodes")
    ->Unit(benchmark::kMillisecond);
BENCHMARK(run<&ChaseList::chase_prefetch>)
    ->RangeMultiplier(4)
    ->Range(1 << 10, 1 << 21)
    ->ArgName("nodes")
    ->Unit(benchmark::kMillisecond);

} // namespace
} // namespace memory_optimization::software_prefetch

BENCHMARK_MAIN();
