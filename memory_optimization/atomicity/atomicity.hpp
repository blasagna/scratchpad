#ifndef MEMORY_OPTIMIZATION_ATOMICITY_ATOMICITY_HPP
#define MEMORY_OPTIMIZATION_ATOMICITY_ATOMICITY_HPP

#include <cstddef>
#include <cstdint>

// Section 6.4.2, "Atomicity Optimizations". When several threads modify the
// same location, the processor guarantees nothing unless atomic operations are
// used: a plain read-modify-write can read a stale value between another
// thread's read and write, and one update is lost. The paper (Figure 6.12)
// shows the three shapes of atomic increment -- an add-and-fetch, a
// fetch-and-add, and a compare-and-swap (CAS) retry loop -- and notes x86-64
// can do the add with a single locked instruction while a CAS loop is the
// general, any-architecture form.
//
// This demo increments ONE shared counter from many threads (maximum
// contention) three ways, and shows that the plain non-atomic version silently
// loses updates while the atomic ones stay exact.

namespace memory_optimization::atomicity {

enum class Mode {
  kFetchAdd,    // std::atomic::fetch_add -- one locked xadd on x86-64
  kCasLoop,     // compare_exchange_weak retry loop -- the portable primitive
  kPlainUnsafe, // separate atomic load + store: a non-atomic RMW that races
};

// Runs `threads` workers, each adding 1 to a single shared counter `iters`
// times using `mode`, and returns the counter's final value.
//
// For kFetchAdd and kCasLoop the result is always threads * iters. For
// kPlainUnsafe the result is <= threads * iters: updates are lost to the race.
// (kPlainUnsafe is written with separate relaxed atomic load/store rather than
// a bare `++` so the program stays data-race-free and defined -- it
// demonstrates the lost-update problem without invoking undefined behavior.)
std::uint64_t run(std::size_t threads, std::uint64_t iters, Mode mode);

} // namespace memory_optimization::atomicity

#endif // MEMORY_OPTIMIZATION_ATOMICITY_ATOMICITY_HPP
