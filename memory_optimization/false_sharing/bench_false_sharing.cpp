#include "memory_optimization/false_sharing/false_sharing.hpp"

#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>

// Reproduces Figure 6.10: N threads each incrementing their own counter, with
// the counters packed onto shared cache lines versus padded one per line. The
// packed run should slow down as thread count rises (RFO ping-pong); the padded
// run should stay roughly flat.
//
//   bazel run -c opt //memory_optimization/false_sharing:bench_false_sharing

namespace memory_optimization::false_sharing {
namespace {

constexpr std::uint64_t kIters =
    20'000'000; // paper uses 500M; 20M keeps it snappy

template <bool Padded> void run_bench(benchmark::State &state) {
  const auto threads = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    std::uint64_t total = run(threads, kIters, Padded);
    benchmark::DoNotOptimize(total);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(threads * kIters));
}

BENCHMARK(run_bench<false>) // packed: false sharing
    ->DenseRange(1, 8, 1)
    ->ArgName("threads")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK(run_bench<true>) // padded: one counter per line
    ->DenseRange(1, 8, 1)
    ->ArgName("threads")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

} // namespace
} // namespace memory_optimization::false_sharing

BENCHMARK_MAIN();
