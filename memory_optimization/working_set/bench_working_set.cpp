#include "memory_optimization/working_set/working_set.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "memory_optimization/support/cacheinfo.hpp"

// Reproduces the §6.2.3 message: bandwidth drops each time the working set
// outgrows a cache level. Sweep a reused buffer from 4 KiB to 256 MiB and
// report the achieved bytes/s; each row is labelled with the smallest cache
// that holds it. Watch the bytes/s fall as the size crosses this machine's L1d
// / L2 / L3.
//
//   bazel run -c opt //memory_optimization/working_set:bench_working_set

namespace memory_optimization::working_set {
namespace {

void run(benchmark::State &state) {
  const auto bytes = static_cast<std::size_t>(state.range(0));
  const std::size_t count = bytes / sizeof(std::uint64_t);
  std::vector<std::uint64_t> buffer(count, 1);

  // Keep total work roughly constant across sizes: touch ~256 MiB each time.
  const std::size_t repeats =
      std::max<std::size_t>(1, (256u << 20) / std::max<std::size_t>(bytes, 1));

  for (auto _ : state) {
    std::uint64_t s = sum_repeatedly(buffer, repeats);
    benchmark::DoNotOptimize(s);
  }
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(bytes * repeats));
  state.SetLabel(level_for_size(bytes));
}

void register_sweep() {
  const support::CacheInfo info = support::query_cache_info();
  benchmark::AddCustomContext("L1d_bytes", std::to_string(info.l1d_size));
  benchmark::AddCustomContext("L2_bytes", std::to_string(info.l2_size));
  benchmark::AddCustomContext("L3_bytes", std::to_string(info.l3_size));

  benchmark::RegisterBenchmark("working_set", run)
      ->RangeMultiplier(2)
      ->Range(1 << 12, 1 << 28) // 4 KiB .. 256 MiB
      ->ArgName("bytes")
      ->Unit(benchmark::kMicrosecond);
}

} // namespace
} // namespace memory_optimization::working_set

int main(int argc, char **argv) {
  memory_optimization::working_set::register_sweep();
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
