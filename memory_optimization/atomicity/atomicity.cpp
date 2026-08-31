#include "memory_optimization/atomicity/atomicity.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

namespace memory_optimization::atomicity {

namespace {

void worker(std::atomic<std::uint64_t> &counter, std::uint64_t iters,
            Mode mode) {
  switch (mode) {
  case Mode::kFetchAdd:
    // paper: "Add and Return Old Value" -- a single locked instruction on
    // x86-64. std::atomic::fetch_add compiles to `lock xadd`.
    for (std::uint64_t k = 0; k < iters; ++k) {
      counter.fetch_add(1, std::memory_order_relaxed);
    }
    break;
  case Mode::kCasLoop:
    // paper: "Atomic Replace with New Value" -- the CAS retry loop, the form
    // that works on any architecture (compare_exchange_weak updates `expected`
    // on failure, so no reload is needed inside the loop).
    for (std::uint64_t k = 0; k < iters; ++k) {
      std::uint64_t expected = counter.load(std::memory_order_relaxed);
      while (!counter.compare_exchange_weak(expected, expected + 1,
                                            std::memory_order_relaxed)) {
        // expected has been refreshed with the current value; retry.
      }
    }
    break;
  case Mode::kPlainUnsafe:
    // A non-atomic read-modify-write: the load and store are individually
    // atomic (so this is defined, not UB) but another thread can slip between
    // them, so increments are lost. This is the bug atomics exist to prevent.
    for (std::uint64_t k = 0; k < iters; ++k) {
      const std::uint64_t v = counter.load(std::memory_order_relaxed);
      counter.store(v + 1, std::memory_order_relaxed);
    }
    break;
  }
}

} // namespace

std::uint64_t run(std::size_t threads, std::uint64_t iters, Mode mode) {
  std::atomic<std::uint64_t> counter{0};
  std::latch start(static_cast<std::ptrdiff_t>(threads));

  {
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
      workers.emplace_back([&] {
        start.arrive_and_wait(); // maximize the contended window
        worker(counter, iters, mode);
      });
    }
  } // join

  return counter.load();
}

} // namespace memory_optimization::atomicity
