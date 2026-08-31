#include "memory_optimization/instruction_cache/instruction_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

// Two §6.2.2 measurements:
//   - inlining: summing a transform over an array with the per-element function
//     inlined vs forced out of line. Inlining removes the call and lets the
//     loop vectorize, so it should be clearly faster.
//   - branch hints: scanning with a rare expensive path, hinted [[unlikely]]
//     (correct) vs [[likely]] (wrong). The effect is smaller and hardware-
//     dependent on modern predictors -- see the README.
//
//   bazel run -c opt
//   //memory_optimization/instruction_cache:bench_instruction_cache

namespace memory_optimization::instruction_cache {
namespace {

constexpr std::size_t kN = 2'000'000;

std::vector<std::uint64_t> make_data() {
  std::vector<std::uint64_t> v(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    v[i] = i * 7 + 1;
  }
  return v;
}

template <std::uint64_t (*Sum)(std::span<const std::uint64_t>)>
void bench_sum(benchmark::State &state) {
  const std::vector<std::uint64_t> data = make_data();
  for (auto _ : state) {
    std::uint64_t s = Sum(data);
    benchmark::DoNotOptimize(s);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kN));
}

template <std::uint64_t (*Scan)(std::span<const std::uint64_t>, std::uint64_t)>
void bench_scan(benchmark::State &state) {
  std::vector<std::uint64_t> data = make_data();
  // Sentinel hits ~0.1% of elements: rare, as the [[unlikely]] hint promises.
  const std::uint64_t sentinel = 12345;
  for (std::size_t i = 0; i < kN; i += 1000) {
    data[i] = sentinel;
  }
  for (auto _ : state) {
    std::uint64_t s = Scan(data, sentinel);
    benchmark::DoNotOptimize(s);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kN));
}

BENCHMARK(bench_sum<sum_not_inlined>)->Unit(benchmark::kMillisecond);
BENCHMARK(bench_sum<sum_inlined>)->Unit(benchmark::kMillisecond);
BENCHMARK(bench_scan<scan_mis_hinted>)->Unit(benchmark::kMillisecond);
BENCHMARK(bench_scan<scan_well_hinted>)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace memory_optimization::instruction_cache

BENCHMARK_MAIN();
