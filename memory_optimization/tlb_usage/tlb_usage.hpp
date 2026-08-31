#ifndef MEMORY_OPTIMIZATION_TLB_USAGE_TLB_USAGE_HPP
#define MEMORY_OPTIMIZATION_TLB_USAGE_TLB_USAGE_HPP

#include <cstddef>
#include <cstdint>

// Section 6.2.4, "Optimizing TLB Usage". Every memory access needs the virtual
// address translated to physical, and the TLB caches those translations. It is
// small (a few thousand entries) and each entry covers one page (4 KiB), so a
// working set spread over many pages thrashes the TLB even when the data itself
// would fit in cache: each access pays a page-table walk. Two levers reduce the
// cost -- touch fewer pages, or use larger pages so each TLB entry covers more.
//
// This demo touches one cache line per 4 KiB page across many pages in random
// order (so neither the cache prefetcher nor the TLB prefetcher helps), and
// compares default 4 KiB pages against transparent huge pages (2 MiB) requested
// with madvise(MADV_HUGEPAGE). The cache-miss count is identical either way, so
// the difference isolates the TLB / page-walk cost. A 2 MiB page covers 512x
// more address space per TLB entry.

namespace memory_optimization::tlb_usage {

// Walks one cache line per page across `num_pages` pages, linked in a random
// cycle, held in an mmap'd region that is either backed by ordinary 4 KiB pages
// or advised for 2 MiB transparent huge pages.
class PageWalker {
public:
  PageWalker(std::size_t num_pages, bool huge_pages, unsigned seed = 1);
  ~PageWalker();

  PageWalker(const PageWalker &) = delete;
  PageWalker &operator=(const PageWalker &) = delete;

  // Follows the cycle `steps` times; returns an opaque address-derived value.
  std::uintptr_t walk(std::size_t steps) const;

  std::size_t num_pages() const noexcept { return num_pages_; }
  bool huge_pages() const noexcept { return huge_pages_; }
  // True if the region actually reports huge-page backing (best-effort; THP is
  // opportunistic, so a false here just means the OS did not promote it).
  bool huge_backed() const;

private:
  struct Node {
    Node *next;
  };
  Node *page_node(std::size_t index) const;

  std::size_t num_pages_;
  bool huge_pages_;
  std::size_t region_bytes_;
  void *region_ = nullptr;
  Node *head_ = nullptr;
};

} // namespace memory_optimization::tlb_usage

#endif // MEMORY_OPTIMIZATION_TLB_USAGE_TLB_USAGE_HPP
