#include "memory_optimization/bypassing_cache/bypass.hpp"

#include <cstddef>

#include <benchmark/benchmark.h>

// Reproduces Table 6.1: the cost of initializing a large matrix under the four
// combinations of {normal, non-temporal} x {row-order, column-order}. The
// matrix is 3000x3000 doubles = ~68 MiB, comfortably larger than L3, so the
// caches cannot cover the writes -- exactly the paper's setup.
//
//   bazel run -c opt //memory_optimization/bypassing_cache:bench_bypass

namespace memory_optimization::bypassing_cache {
namespace {

constexpr std::size_t kN = 3000;

template <void (*Fill)(Matrix &, double, Order)>
void run(benchmark::State &state) {
  const Order order = state.range(0) == 0 ? Order::kRowMajor : Order::kColMajor;
  Matrix m(kN);
  for (auto _ : state) {
    Fill(m, 1.0, order);
    benchmark::DoNotOptimize(m.data.data());
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(kN * kN * sizeof(double)));
}

// range(0): 0 = row-major (sequential), 1 = column-major (strided).
BENCHMARK(run<fill_normal>)
    ->Arg(0)
    ->Arg(1)
    ->Unit(benchmark::kMillisecond)
    ->ArgNames({"order"});
BENCHMARK(run<fill_nontemporal>)
    ->Arg(0)
    ->Arg(1)
    ->Unit(benchmark::kMillisecond)
    ->ArgNames({"order"});

} // namespace
} // namespace memory_optimization::bypassing_cache

BENCHMARK_MAIN();
