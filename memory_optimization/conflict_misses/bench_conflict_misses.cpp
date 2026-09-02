#include "memory_optimization/conflict_misses/conflict_misses.hpp"

#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>

#include "memory_optimization/support/rdtsc.hpp"

// Reproduces Figure 6.5: cycles per element chasing a short list whose nodes
// sit a fixed stride apart. The list length is held above the L1d
// associativity, so when the stride is a large power of two (4096) all nodes
// collide in a few cache sets and cycles/element spikes; a nearby
// non-power-of-two stride spreads them across sets and stays fast.
//
//   bazel run -c opt
//   //memory_optimization/conflict_misses:bench_conflict_misses

namespace memory_optimization::conflict_misses {
namespace {

// Longer than any L1d's associativity (12 on this Zen5, 8 in the paper), so a
// single colliding set cannot hold the whole list.
constexpr std::size_t kCount = 32;
constexpr std::size_t kSteps = 20'000'000;

void run(benchmark::State &state) {
  const auto stride = static_cast<std::size_t>(state.range(0));
  ChaseList list(kCount, stride);

  std::uint64_t cycles = 0;
  for (auto _ : state) {
    const std::uint64_t start = support::rdtsc();
    std::uintptr_t sink = list.chase(kSteps);
    cycles += support::rdtsc() - start;
    benchmark::DoNotOptimize(sink);
  }
  state.counters["cycles_per_element"] =
      benchmark::Counter(static_cast<double>(cycles) /
                         (static_cast<double>(state.iterations()) * kSteps));
}

// Strides in bytes. 4096 and 8192 are the conflict-prone powers of two; the
// odd neighbours (3968, 4160) and small strides are the controls.
BENCHMARK(run)
    ->Arg(64)
    ->Arg(256)
    ->Arg(3968)
    ->Arg(4096)
    ->Arg(4160)
    ->Arg(8192)
    ->ArgName("stride")
    ->Unit(benchmark::kMillisecond);

} // namespace
} // namespace memory_optimization::conflict_misses

BENCHMARK_MAIN();
