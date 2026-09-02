#include "memory_optimization/software_prefetch/software_prefetch.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <xmmintrin.h>

#include "memory_optimization/support/permutation.hpp"

namespace memory_optimization::software_prefetch {

ChaseList::ChaseList(std::size_t count, std::size_t distance, unsigned seed)
    : count_(count), nodes_(count) {
  // An empty list has no head; leave head_ null. chase_* are only meaningful
  // for count > 0, which every caller passes.
  if (count_ == 0) {
    return;
  }

  // Random single cycle over all nodes (see conflict_misses for the same idea):
  // a shuffled permutation whose consecutive entries link head-to-tail.
  const std::vector<std::size_t> order =
      support::random_permutation(count_, seed);

  for (std::size_t i = 0; i < count_; ++i) {
    Node &cur = nodes_[order[i]];
    cur.next = &nodes_[order[(i + 1) % count_]];
    // `ahead` is the node `distance` hops further along the traversal order,
    // precomputed here so the hot loop never chases to find it.
    cur.ahead = &nodes_[order[(i + distance) % count_]];
    cur.payload = order[i];
  }
  head_ = &nodes_[order[0]];
}

std::uint64_t ChaseList::chase_plain(std::size_t steps) const {
  const Node *p = head_;
  std::uint64_t sum = 0;
  for (std::size_t s = 0; s < steps; ++s) {
    sum += p->payload;
    p = p->next;
  }
  return sum;
}

std::uint64_t ChaseList::chase_prefetch(std::size_t steps) const {
  const Node *p = head_;
  std::uint64_t sum = 0;
  for (std::size_t s = 0; s < steps; ++s) {
    // paper: _mm_prefetch(p, _MM_HINT_T0) issues a load hint for a node several
    // hops ahead. The target (p->ahead) was loaded together with p, so the hint
    // adds no dependent load of its own -- it just warms a line ~`distance`
    // hops before the walk reaches it.
    _mm_prefetch(reinterpret_cast<const char *>(p->ahead), _MM_HINT_T0);
    sum += p->payload;
    p = p->next;
  }
  return sum;
}

} // namespace memory_optimization::software_prefetch
