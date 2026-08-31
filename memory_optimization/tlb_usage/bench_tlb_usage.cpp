#include "memory_optimization/tlb_usage/tlb_usage.hpp"

#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>

#include "memory_optimization/support/rdtsc.hpp"

// Reproduces the §6.2.4 point: a random walk touching one line per page pays a
// page-table walk on nearly every access once the page count exceeds the TLB's
// reach. Compare 4 KiB pages against 2 MiB transparent huge pages over the same
// access pattern -- the huge-page run should need far fewer translations and so
// spend fewer cycles per access, especially at large page counts.
//
//   bazel run -c opt //memory_optimization/tlb_usage:bench_tlb_usage
//
// Needs transparent huge pages enabled (see the README); if the kernel does not
// promote the region, the two curves converge -- which is itself informative.

namespace memory_optimization::tlb_usage {
namespace {

constexpr std::size_t kSteps = 5'000'000;

template <bool Huge> void run(benchmark::State &state) {
  const auto pages = static_cast<std::size_t>(state.range(0));
  PageWalker walker(pages, Huge);

  std::uint64_t cycles = 0;
  for (auto _ : state) {
    const std::uint64_t start = support::rdtsc();
    std::uintptr_t sink = walker.walk(kSteps);
    cycles += support::rdtsc() - start;
    benchmark::DoNotOptimize(sink);
  }
  state.counters["cycles_per_access"] =
      benchmark::Counter(static_cast<double>(cycles) /
                         (static_cast<double>(state.iterations()) * kSteps));
  state.SetLabel(Huge ? "2MiB" : "4KiB");
}

// 512 pages (2 MiB) fits the L2 TLB; 128Ki pages (512 MiB of address space)
// blows well past it. The data touched is one line per page either way.
BENCHMARK(run<false>)
    ->RangeMultiplier(8)
    ->Range(512, 128 * 1024)
    ->ArgName("pages")
    ->Unit(benchmark::kMillisecond);
BENCHMARK(run<true>)
    ->RangeMultiplier(8)
    ->Range(512, 128 * 1024)
    ->ArgName("pages")
    ->Unit(benchmark::kMillisecond);

} // namespace
} // namespace memory_optimization::tlb_usage

BENCHMARK_MAIN();
