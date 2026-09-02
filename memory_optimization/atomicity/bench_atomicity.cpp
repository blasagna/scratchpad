#include "memory_optimization/atomicity/atomicity.hpp"

#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>

// Measures the cost of a contended atomic increment: many threads hammering one
// shared counter, fetch_add versus a CAS retry loop. Both slow down as threads
// contend for the single cache line; the CAS loop pays extra when it has to
// retry after losing a race, so it should trail fetch_add as contention rises.
//
//   bazel run -c opt //memory_optimization/atomicity:bench_atomicity

namespace memory_optimization::atomicity {
namespace {

constexpr std::uint64_t kIters = 5'000'000;

template <Mode M> void run_bench(benchmark::State &state) {
  const auto threads = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    std::uint64_t total = run(threads, kIters, M);
    benchmark::DoNotOptimize(total);
  }
  // items = total increments attempted across all threads.
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(threads * kIters));
}

BENCHMARK(run_bench<Mode::kFetchAdd>)
    ->DenseRange(1, 8, 1)
    ->ArgName("threads")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK(run_bench<Mode::kCasLoop>)
    ->DenseRange(1, 8, 1)
    ->ArgName("threads")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

} // namespace
} // namespace memory_optimization::atomicity

BENCHMARK_MAIN();
