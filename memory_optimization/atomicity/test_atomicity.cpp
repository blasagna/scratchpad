#include "memory_optimization/atomicity/atomicity.hpp"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace memory_optimization::atomicity {
namespace {

constexpr std::size_t kThreads = 8;
constexpr std::uint64_t kIters = 200'000;

// The atomic modes never lose an update, however contended.
TEST(Atomicity, FetchAddIsExact) {
  EXPECT_EQ(run(kThreads, kIters, Mode::kFetchAdd), kThreads * kIters);
}

TEST(Atomicity, CasLoopIsExact) {
  EXPECT_EQ(run(kThreads, kIters, Mode::kCasLoop), kThreads * kIters);
}

// One thread has no one to race with, so even the unsafe version is exact --
// this pins that the loss under contention is the race, not a counting bug.
TEST(Atomicity, PlainUnsafeExactWhenUncontended) {
  EXPECT_EQ(run(1, kIters, Mode::kPlainUnsafe), kIters);
}

// Under contention the unsafe version must never *over*count, and in practice
// loses updates. We assert the hard invariant (<= expected) unconditionally,
// and only note the lost update when it actually happens to avoid a flaky test.
TEST(Atomicity, PlainUnsafeLosesUpdatesUnderContention) {
  const std::uint64_t expected = kThreads * kIters;
  const std::uint64_t got = run(kThreads, kIters, Mode::kPlainUnsafe);
  EXPECT_LE(got, expected);
  EXPECT_GT(got, 0u);
  if (got == expected) {
    GTEST_LOG_(INFO) << "no updates lost this run (race did not manifest)";
  }
}

} // namespace
} // namespace memory_optimization::atomicity
