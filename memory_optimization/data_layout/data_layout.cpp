#include "memory_optimization/data_layout/data_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace memory_optimization::data_layout {

double total_due_fat(std::span<const Order> orders) {
  double due = 0.0;
  for (const Order &o : orders) {
    if (!o.paid) {
      due += o.price;
    }
  }
  return due;
}

double total_due_split(std::span<const HotOrder> orders) {
  double due = 0.0;
  for (const HotOrder &o : orders) {
    if (!o.paid) {
      due += o.price;
    }
  }
  return due;
}

// --- Alignment --------------------------------------------------------------

namespace {
constexpr std::size_t kLine = 64;
} // namespace

std::byte *SlotBuffer::slot(std::size_t i) noexcept {
  return buffer_.data() + offset_ + i * stride_;
}

SlotBuffer::SlotBuffer(std::size_t count, std::size_t stride,
                       std::size_t offset)
    : count_(count), stride_(stride), offset_(offset) {
  // Round the base up to a line so `offset` measures from a known boundary, and
  // add a line of slack so the last slot's 8 bytes never run off the end.
  const std::size_t span = offset_ + count_ * stride_ + kLine;
  buffer_.assign(span + kLine, std::byte{0});
  // Shift so buffer_.data()+offset_ sits at the requested offset past a line.
  const auto base = reinterpret_cast<std::uintptr_t>(buffer_.data());
  const std::size_t to_line = (kLine - (base % kLine)) % kLine;
  offset_ += to_line;
}

std::uint64_t SlotBuffer::increment_all() {
  for (std::size_t i = 0; i < count_; ++i) {
    std::byte *p = slot(i);
    // memcpy keeps the (possibly unaligned) read-modify-write defined in C++
    // while lowering to a single unaligned mov on x86 -- the paper does the RMW
    // through a raw misaligned pointer.
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    v += 1;
    std::memcpy(p, &v, sizeof(v));
  }
  return count_;
}

} // namespace memory_optimization::data_layout
