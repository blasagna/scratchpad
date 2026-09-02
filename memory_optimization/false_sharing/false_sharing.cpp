#include "memory_optimization/false_sharing/false_sharing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

namespace memory_optimization::false_sharing {

void hammer(std::uint64_t *counter, std::uint64_t iters) {
  std::atomic_ref<std::uint64_t> ref(*counter);
  for (std::uint64_t k = 0; k < iters; ++k) {
    ref.fetch_add(1, std::memory_order_relaxed);
  }
}

namespace {

template <typename Counter>
std::uint64_t run_layout(std::size_t threads, std::uint64_t iters) {
  std::vector<Counter> counters(threads);
  std::latch start(static_cast<std::ptrdiff_t>(threads));

  {
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
      workers.emplace_back([&, t] {
        start.arrive_and_wait(); // release all workers together
        hammer(&counters[t].value, iters);
      });
    }
  } // jthreads join here

  std::uint64_t total = 0;
  for (const Counter &c : counters) {
    total += c.value;
  }
  return total;
}

} // namespace

std::uint64_t run(std::size_t threads, std::uint64_t iters, bool padded) {
  return padded ? run_layout<Padded>(threads, iters)
                : run_layout<Packed>(threads, iters);
}

} // namespace memory_optimization::false_sharing
