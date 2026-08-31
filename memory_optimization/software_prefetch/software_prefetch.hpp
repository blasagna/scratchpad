#ifndef MEMORY_OPTIMIZATION_SOFTWARE_PREFETCH_SOFTWARE_PREFETCH_HPP
#define MEMORY_OPTIMIZATION_SOFTWARE_PREFETCH_SOFTWARE_PREFETCH_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// Section 6.3.2, "Software Prefetching". Hardware prefetchers hide latency for
// regular strides, but a randomized pointer chase defeats them: each `next` is
// unpredictable and the load is on the critical path, so the CPU stalls the
// full memory latency at every hop once the working set spills out of cache.
// The programmer can help with the _mm_prefetch intrinsic, issuing a load hint
// for a node several hops ahead so its line is in cache by the time the walk
// arrives (the paper's Figure 6.7: NPAD=31, prefetching ~5 elements ahead).
//
// The catch in a pointer chase is knowing the address N hops ahead without
// following N pointers. This demo precomputes it: every node stores an `ahead`
// pointer to the node `distance` hops later in traversal order, filled in once
// at build time. In the hot loop the prefetch target is therefore already in a
// register (loaded with the node itself), so prefetching costs no extra chase.

namespace memory_optimization::software_prefetch {

// Padded so each node fills much of a cache line, matching the paper's NPAD-
// padded element (larger nodes make each hop a guaranteed line miss).
struct Node {
  Node *next = nullptr;
  Node *ahead = nullptr; // node `distance` hops further along the walk
  std::uint64_t payload = 0;
  std::uint64_t pad[5] = {}; // fill out to 64 bytes
};

class ChaseList {
public:
  // Builds `count` nodes in a single random cycle. `distance` is how many hops
  // ahead each node's `ahead` pointer targets (the prefetch look-ahead).
  ChaseList(std::size_t count, std::size_t distance, unsigned seed = 1);

  // Walk `steps` hops summing payloads, WITHOUT software prefetch.
  std::uint64_t chase_plain(std::size_t steps) const;

  // Same walk, issuing _mm_prefetch(node->ahead) each hop.
  std::uint64_t chase_prefetch(std::size_t steps) const;

  std::size_t count() const noexcept { return count_; }
  std::size_t working_set_bytes() const noexcept {
    return count_ * sizeof(Node);
  }

private:
  std::size_t count_;
  Node *head_ = nullptr;
  std::vector<Node> nodes_;
};

} // namespace memory_optimization::software_prefetch

#endif // MEMORY_OPTIMIZATION_SOFTWARE_PREFETCH_SOFTWARE_PREFETCH_HPP
