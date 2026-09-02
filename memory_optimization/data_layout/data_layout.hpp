#ifndef MEMORY_OPTIMIZATION_DATA_LAYOUT_DATA_LAYOUT_HPP
#define MEMORY_OPTIMIZATION_DATA_LAYOUT_DATA_LAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Section 6.2.1, "Optimizing Level 1 Data Cache Access" -- data structure
// layout. Two layout levers the paper calls out:
//
//   1. Hot/cold splitting. The paper's `struct order` bundles a hot field the
//      billing job reads (price/paid) with cold fields it does not (buyer[5],
//      buyer_id). Iterating over an array of these to sum outstanding payments
//      drags the cold fields through the cache too, so each useful value costs
//      a near-whole cache line -- "up to 5 times worse than it could be."
//      Storing the hot fields in their own compact array packs several per
//      line.
//
//   2. Alignment. An access that straddles a cache-line boundary touches two
//      lines instead of one (the paper's Figure 6.4, "Overhead of Unaligned
//      Accesses"). Keeping data naturally aligned keeps each access on one
//      line.
//
// The layout of the structs themselves is best inspected with the `pahole`
// tool (paper §6.2.1 / §7); see this project's README. `Order` below is laid
// out exactly as the paper's example so its pahole output matches.

namespace memory_optimization::data_layout {

// paper: `struct order { double price; bool paid; const char *buyer[5]; long
// buyer_id; };`. On x86-64 this is 64 bytes (8 + 1 + 7 pad + 40 + 8). A job
// that only needs price+paid still pulls the whole line per record.
struct Order {
  double price;
  bool paid;
  const char *buyer[5];
  long buyer_id;
};

// The hot half, split out: 16 bytes, so ~4 records share a 64-byte line.
struct HotOrder {
  double price;
  bool paid;
};

// Sums the price of every unpaid order. `total_due_fat` reads from the full
// records (cold fields evicting useful data); `total_due_split` reads from the
// packed hot array. Same result, very different cache footprint.
double total_due_fat(std::span<const Order> orders);
double total_due_split(std::span<const HotOrder> orders);

// --- Alignment --------------------------------------------------------------

// A working buffer of `count` 64-bit slots laid out at `stride` bytes apart,
// starting at byte `offset` within a cache-line-aligned block. offset=0 with
// stride a multiple of 8 keeps every slot inside one line; an offset that
// pushes each slot across a 64-byte boundary makes every access touch two
// lines.
class SlotBuffer {
public:
  SlotBuffer(std::size_t count, std::size_t stride, std::size_t offset);

  // Adds 1 to every slot (a defined unaligned RMW via memcpy). Returns the
  // number of slots touched, so the caller can defeat dead-code elimination.
  std::uint64_t increment_all();

  std::size_t count() const noexcept { return count_; }

private:
  std::byte *slot(std::size_t i) noexcept;

  std::size_t count_;
  std::size_t stride_;
  std::size_t offset_;
  std::vector<std::byte> buffer_;
};

} // namespace memory_optimization::data_layout

#endif // MEMORY_OPTIMIZATION_DATA_LAYOUT_DATA_LAYOUT_HPP
