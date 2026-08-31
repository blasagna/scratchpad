#ifndef MEMORY_OPTIMIZATION_CONFLICT_MISSES_CONFLICT_MISSES_HPP
#define MEMORY_OPTIMIZATION_CONFLICT_MISSES_CONFLICT_MISSES_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Section 6.2.1, "Optimizing Level 1 Data Cache Access" -- cache associativity
// and *conflict misses*. An L1d cache is set-associative: an address maps to
// one set by its middle bits, and only `associativity` lines can live in a set
// at once. If many hot addresses share the same set, they evict each other even
// though the rest of the cache sits idle. These are conflict misses, and the
// programmer can trigger them by laying data out at a power-of-two stride --
// which is exactly what arrays of power-of-two-sized structs, or fields at a
// fixed offset across many objects, do.
//
// The paper's Figure 6.5 chases a linked list whose elements sit a fixed
// distance apart. When that distance is a multiple of 4096 bytes and the list
// is longer than the associativity, all elements land in the same few sets and
// cycles-per-element jumps sharply. This demo reproduces that: place `count`
// nodes `stride` bytes apart, link them in a random cycle (so the hardware
// prefetcher cannot hide the latency), and time a traversal.

namespace memory_optimization::conflict_misses {

// A list node. Only the `next` pointer matters; the traversal is pure pointer
// chasing, so each step depends on the previous load (no instruction-level
// parallelism to hide a miss).
struct Node {
  Node *next = nullptr;
};

// A cyclic linked list whose nodes are placed at a chosen stride inside one
// aligned buffer, so the low address bits (hence the cache set) are controlled.
class ChaseList {
public:
  // Builds `count` nodes `stride` bytes apart (stride must be >= sizeof(Node)),
  // linked into a single random cycle. `seed` fixes the permutation.
  ChaseList(std::size_t count, std::size_t stride, unsigned seed = 1);

  // Follows `next` `steps` times starting from the head; returns an opaque
  // value derived from the visited addresses so the loop cannot be optimized
  // away.
  std::uintptr_t chase(std::size_t steps) const;

  // Number of distinct nodes on the cycle through the head. For a correctly
  // built list this equals count(); the test uses it to verify the structure.
  std::size_t cycle_length() const;

  std::size_t count() const noexcept { return count_; }

private:
  Node *node_at(std::size_t index);

  std::size_t count_;
  std::size_t stride_;
  Node *head_ = nullptr;
  // Over-aligned storage so node offsets (index*stride) alone decide the set.
  std::unique_ptr<std::byte[]> buffer_;
};

} // namespace memory_optimization::conflict_misses

#endif // MEMORY_OPTIMIZATION_CONFLICT_MISSES_CONFLICT_MISSES_HPP
