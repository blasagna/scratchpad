#include "memory_optimization/tlb_usage/tlb_usage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <numeric>
#include <random>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace memory_optimization::tlb_usage {

namespace {
constexpr std::size_t kPage = 4096;     // logical stride: one node per 4 KiB
constexpr std::size_t kHuge = 1u << 21; // 2 MiB transparent huge page

std::size_t round_up(std::size_t n, std::size_t to) {
  return (n + to - 1) / to * to;
}
} // namespace

PageWalker::Node *PageWalker::page_node(std::size_t index) const {
  return reinterpret_cast<Node *>(static_cast<std::byte *>(region_) +
                                  index * kPage);
}

PageWalker::PageWalker(std::size_t num_pages, bool huge_pages, unsigned seed)
    : num_pages_(num_pages), huge_pages_(huge_pages) {
  // An empty walk has no head; leave region_ null (munmap in the destructor is
  // guarded). walk() is only meaningful for num_pages > 0, which callers pass.
  if (num_pages_ == 0) {
    return;
  }

  // Round the region to a 2 MiB multiple either way so the huge-page case has
  // whole 2 MiB spans to promote, and the two cases allocate identically.
  region_bytes_ = round_up(num_pages_ * kPage, kHuge);
  region_ = ::mmap(nullptr, region_bytes_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, /*fd=*/-1, /*offset=*/0);
  if (region_ == MAP_FAILED) {
    // The region can be hundreds of MiB (the bench sweeps up to 512 MiB); under
    // memory pressure or a tight ulimit mmap fails, and using MAP_FAILED below
    // would segfault. Fail cleanly instead.
    region_ = nullptr;
    throw std::bad_alloc();
  }

  if (huge_pages_) {
    // paper: use larger pages so each TLB entry covers more. MADV_HUGEPAGE asks
    // the kernel to back this region with 2 MiB transparent huge pages.
    ::madvise(region_, region_bytes_, MADV_HUGEPAGE);
  } else {
    // Ask explicitly for no huge pages, so the baseline is honestly 4 KiB even
    // if the system default is THP=always.
    ::madvise(region_, region_bytes_, MADV_NOHUGEPAGE);
  }

  // Fault every page in now (and, for the huge case, give khugepaged 2 MiB-
  // aligned dirty spans to collapse) so the measured walk pays no page faults.
  std::memset(region_, 0, region_bytes_);

  // Random single cycle over the page nodes.
  std::vector<std::size_t> order(num_pages_);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  for (std::size_t i = 0; i < num_pages_; ++i) {
    page_node(order[i])->next = page_node(order[(i + 1) % num_pages_]);
  }
  head_ = page_node(order[0]);
}

PageWalker::~PageWalker() {
  if (region_ != nullptr && region_ != MAP_FAILED) {
    ::munmap(region_, region_bytes_);
  }
}

std::uintptr_t PageWalker::walk(std::size_t steps) const {
  const Node *p = head_;
  std::uintptr_t acc = 0;
  for (std::size_t s = 0; s < steps; ++s) {
    acc ^= reinterpret_cast<std::uintptr_t>(p);
    p = p->next;
  }
  return acc ^ reinterpret_cast<std::uintptr_t>(p);
}

bool PageWalker::huge_backed() const {
  // Read the AnonHugePages line for this range from /proc/self/smaps is heavy;
  // instead we report the request. THP promotion is opportunistic, so callers
  // treat this as "asked for", and the benchmark labels it as such.
  return huge_pages_;
}

} // namespace memory_optimization::tlb_usage
