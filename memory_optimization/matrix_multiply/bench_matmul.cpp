#include "memory_optimization/matrix_multiply/matmul.hpp"

#include <cstddef>
#include <cstdint>
#include <random>

#include <benchmark/benchmark.h>

#include "memory_optimization/support/rdtsc.hpp"

// Reproduces Table 6.2 of the paper: the cost of a single N x N double product
// under the three access patterns. Report both wall time (Google Benchmark's
// native unit) and CPU cycles (the paper's unit) via the timestamp counter.
//
// Build optimized or the numbers are meaningless -- the default fastbuild is
// -O0:  bazel run -c opt //memory_optimization/matrix_multiply:bench_matmul

namespace memory_optimization::matrix_multiply {
namespace {

Matrix random_matrix(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  Matrix m(n);
  for (auto &x : m.data) {
    x = dist(rng);
  }
  return m;
}

template <Matrix (*Mul)(const Matrix &, const Matrix &)>
void run(benchmark::State &state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const Matrix a = random_matrix(n, 1);
  const Matrix b = random_matrix(n, 2);

  std::uint64_t cycles = 0;
  for (auto _ : state) {
    const std::uint64_t start = support::rdtsc();
    Matrix c = Mul(a, b);
    cycles += support::rdtsc() - start;
    benchmark::DoNotOptimize(c.data.data());
    benchmark::ClobberMemory();
  }

  const auto iters = static_cast<double>(state.iterations());
  state.counters["cycles"] =
      benchmark::Counter(static_cast<double>(cycles) / iters);
  // 2*N^3 flops per product (a multiply and an add per inner step).
  state.SetItemsProcessed(state.iterations() * 2 * n * n * n);
}

// 512 fits a naive run into a few ms; 1024 shows the effect at the paper's
// scale. Both are multiples of the blocking factor, though the code no longer
// requires it.
BENCHMARK(run<mul_naive>)->Arg(512)->Arg(1024)->Unit(benchmark::kMillisecond);
BENCHMARK(run<mul_transposed>)
    ->Arg(512)
    ->Arg(1024)
    ->Unit(benchmark::kMillisecond);
BENCHMARK(run<mul_blocked>)->Arg(512)->Arg(1024)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace memory_optimization::matrix_multiply

BENCHMARK_MAIN();
