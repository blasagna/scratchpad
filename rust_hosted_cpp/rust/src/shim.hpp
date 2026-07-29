#ifndef RUST_HOSTED_CPP_RUST_SRC_SHIM_HPP
#define RUST_HOSTED_CPP_RUST_SRC_SHIM_HPP

#include <memory>
#include <string>

#include "lrukit.hpp"
#include "rust/cxx.h"

// CacheStats is the cxx *shared* struct declared in lib.rs. Its definition is
// generated into "lrukit/src/lib.rs.h", and this file deliberately does not
// include that header.
//
// The reason is that the generated header includes *this* one, because the
// bridge module declares include!("shim.hpp"). Including it back would make the
// two mutually dependent, and header guards do not rescue a cycle -- they only
// decide which half loses. A translation unit that reached the generated header
// first would enter it, come here, find the include of the generated header
// already suppressed, and hit the declarations below with CacheStats still
// undefined. That is a confusing error a long way from its cause, and it waits
// for the second .cpp file rather than failing now.
//
// A forward declaration breaks the cycle instead. Declaring a function that
// returns an incomplete type by value is legal; only the definitions need the
// full struct, and shim.cpp includes the generated header to get it. So this
// header stays includable from anywhere, in any order.
namespace lrukit::bridge {
struct CacheStats;
}

// The binding layer: the only code in this project that knows both languages
// exist. It is the C++ counterpart of the `#[cxx::bridge]` module in lib.rs.
//
// ../../cpp/lrukit.hpp is deliberately cxx-free, which leaves three mismatches
// for this file to absorb:
//
//   * Strings. cxx passes borrowed strings as rust::Str and owned ones as
//     rust::String; the library speaks std::string_view and std::string. There
//     is no implicit conversion in either direction, so every signature that
//     carries a string needs an adapter.
//
//   * Member functions. cxx can bind a C++ method directly, but only when its
//     signature already matches the Rust one it is declared with.
//     Cache::put(std::string_view, std::string_view) does not, so the methods
//     are re-exported here as free functions taking the object by reference --
//     the shape cxx maps to `Pin<&mut Cache>` and `&Cache`.
//
//   * Absent values. Cache::find returns a pointer that is null on a miss.
//     Raw pointers are exactly what a safe binding must not hand to Rust, so
//     `get` below splits the answer in two: a bool for "was it there" and an
//     out-parameter for the value. lib.rs reassembles them into Option<String>,
//     which is the shape a Rust caller wanted in the first place.
//
// Errors need no adapter at all. cxx wraps every function declared to return
// `Result` in a try/catch for std::exception and hands what() to Rust, and
// CacheError derives from std::runtime_error. Note that this holds for *any*
// std::exception, so a std::bad_alloc from deep inside the library also arrives
// as an Err rather than crossing the boundary and aborting -- but only for the
// functions Rust declares `Result`. That is every one of them; see the bridge
// module in lib.rs for why the uniformity is deliberate.
namespace lrukit::bridge {

std::unique_ptr<Cache> new_cache(std::size_t capacity);

bool put(Cache &cache, rust::Str key, rust::Str value);
bool get(Cache &cache, rust::Str key, std::string &out);
bool contains(const Cache &cache, rust::Str key);
bool erase(Cache &cache, rust::Str key);
void clear(Cache &cache);
rust::Vec<rust::String> keys(const Cache &cache);
std::size_t len(const Cache &cache);
std::size_t capacity(const Cache &cache);

CacheStats stats(const Cache &cache);
rust::String format_stats(const CacheStats &stats);

} // namespace lrukit::bridge

#endif // RUST_HOSTED_CPP_RUST_SRC_SHIM_HPP
