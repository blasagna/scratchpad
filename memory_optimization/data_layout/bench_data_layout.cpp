#include "memory_optimization/data_layout/data_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

// Two measurements from §6.2.1:
//   - hot/cold split: summing unpaid orders over the fat struct vs the packed
//     hot array. The fat version drags cold fields through the cache, so it
//     should be several times slower once the data exceeds the caches.
//   - alignment: incrementing 64-bit slots that sit inside a cache line vs
//     slots deliberately straddling a line boundary (Figure 6.4).
//
//   bazel run -c opt //memory_optimization/data_layout:bench_data_layout

namespace memory_optimization::data_layout {
namespace {

// 4M orders: fat = 256 MiB, hot = 64 MiB, both past L3, differing only in how
// many lines the billing job must touch.
constexpr std::size_t kOrders = 4'000'000;

std::vector<Order> make_fat() {
  std::vector<Order> v(kOrders);
  for (std::size_t i = 0; i < kOrders; ++i) {
    v[i].price = static_cast<double>(i);
    v[i].paid = (i % 4) == 0;
    v[i].buyer_id = static_cast<long>(i);
  }
  return v;
}

std::vector<HotOrder> make_hot() {
  std::vector<HotOrder> v(kOrders);
  for (std::size_t i = 0; i < kOrders; ++i) {
    v[i].price = static_cast<double>(i);
    v[i].paid = (i % 4) == 0;
  }
  return v;
}

void bench_fat(benchmark::State &state) {
  const std::vector<Order> orders = make_fat();
  for (auto _ : state) {
    double due = total_due_fat(orders);
    benchmark::DoNotOptimize(due);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(kOrders));
}

void bench_split(benchmark::State &state) {
  const std::vector<HotOrder> orders = make_hot();
  for (auto _ : state) {
    double due = total_due_split(orders);
    benchmark::DoNotOptimize(due);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(kOrders));
}

// offset 0 => aligned inside the line; offset 60 => each 8-byte slot crosses
// the 64-byte boundary, touching two lines.
void bench_alignment(benchmark::State &state) {
  constexpr std::size_t kSlots = 2'000'000; // ~128 MiB span at stride 64
  const auto offset = static_cast<std::size_t>(state.range(0));
  SlotBuffer buf(kSlots, 64, offset);
  for (auto _ : state) {
    std::uint64_t touched = buf.increment_all();
    benchmark::DoNotOptimize(touched);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(kSlots));
}

BENCHMARK(bench_fat)->Unit(benchmark::kMillisecond);
BENCHMARK(bench_split)->Unit(benchmark::kMillisecond);
BENCHMARK(bench_alignment)
    ->Arg(0)
    ->Arg(60)
    ->ArgName("offset")
    ->Unit(benchmark::kMillisecond);

} // namespace
} // namespace memory_optimization::data_layout

BENCHMARK_MAIN();
