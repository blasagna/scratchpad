#include "memory_optimization/conflict_misses/conflict_misses.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <numeric>
#include <random>
#include <vector>

namespace memory_optimization::conflict_misses {

Node *ChaseList::node_at(std::size_t index) {
  return reinterpret_cast<Node *>(buffer_.get() + index * stride_);
}

ChaseList::ChaseList(std::size_t count, std::size_t stride, unsigned seed)
    : count_(count), stride_(std::max(stride, sizeof(Node))) {
  // An empty list has no head to chase; leave everything null. chase() and
  // cycle_length() are only meaningful for count > 0, which every caller
  // passes.
  if (count_ == 0) {
    return;
  }

  const std::size_t bytes = count_ * stride_;
  // Over-aligned new (C++17): replaces the paper's posix_memalign. Paired with
  // AlignedBufferDelete so the deallocation matches (see the header).
  buffer_.reset(new (std::align_val_t{kBufferAlign}) std::byte[bytes]());

  // A random permutation of [0, count) whose successive entries form one cycle
  // covering every node -- a random single cycle, so the traversal is a random
  // walk the prefetcher cannot follow.
  std::vector<std::size_t> order(count_);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);

  for (std::size_t i = 0; i < count_; ++i) {
    Node *cur = node_at(order[i]);
    Node *nxt = node_at(order[(i + 1) % count_]);
    cur->next = nxt;
  }
  head_ = node_at(order[0]);
}

std::size_t ChaseList::cycle_length() const {
  std::size_t len = 0;
  const Node *p = head_;
  do {
    p = p->next;
    ++len;
  } while (p != head_);
  return len;
}

std::uintptr_t ChaseList::chase(std::size_t steps) const {
  const Node *p = head_;
  std::uintptr_t acc = 0;
  for (std::size_t s = 0; s < steps; ++s) {
    // Mixing the address in makes the result depend on the whole walk, so the
    // compiler must actually perform every dependent load.
    acc ^= reinterpret_cast<std::uintptr_t>(p);
    p = p->next;
  }
  return acc ^ reinterpret_cast<std::uintptr_t>(p);
}

} // namespace memory_optimization::conflict_misses
