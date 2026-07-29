#ifndef RUST_HOSTED_CPP_CPP_LRUKIT_HPP
#define RUST_HOSTED_CPP_CPP_LRUKIT_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A fixed-capacity least-recently-used cache.
//
// This is the *core* library: plain C++20, with no idea that Rust exists. The
// binding layer lives in ../rust/src/shim.{hpp,cpp} and depends on this header,
// never the other way round.
//
// Unlike ../../cpp_rust_bindings/cpp, this directory has no build file, no test
// binary, and no CLI. Cargo compiles these sources through ../rust/build.rs and
// that is the only way they are ever built; the tests that cover the behavior
// described below live in ../rust/tests/cache.rs. Keeping the header cxx-free
// anyway is what makes that arrangement a choice rather than a dependency --
// the library could be dropped into a C++ build tomorrow without touching a
// line.
namespace lrukit {

// Every failure this library reports.
//
// Deriving from std::runtime_error is what lets the Rust bindings turn it into
// an Err without naming the type: cxx wraps each fallible call in a try/catch
// for std::exception and forwards what().
class CacheError : public std::runtime_error {
public:
  explicit CacheError(const std::string &message)
      : std::runtime_error(message) {}
};

// A snapshot of a cache's counters.
//
// This is an ordinary C++ struct, deliberately *not* the cxx shared struct of
// the same shape declared in ../rust/src/lib.rs. A shared struct is generated
// into a cxx header, and including that header here would make this library
// depend on the binding it is supposed to know nothing about. The shim copies
// the five fields across; see ../rust/src/shim.cpp.
struct Stats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::size_t size = 0;
  std::size_t capacity = 0;
};

// format_stats - renders a Stats the way the CLI prints it.
//
// Lives in the library rather than in the CLI because the wording of a stat
// line is library behavior: the numbers only mean anything alongside the rules
// for when they move. There is exactly one definition of that text.
std::string format_stats(const Stats &stats);

// A cache holding at most `capacity` string entries, evicting the least
// recently used one to make room.
//
// "Used" means inserted, updated, or fetched with find(). It deliberately does
// not include contains(), which answers a question about the cache without
// disturbing it -- a predicate that reorders its own subject is a poor
// predicate, and the distinction is what lets a caller poll without starving
// whatever it was about to evict.
class Cache {
public:
  // Throws CacheError if capacity is 0. A zero-capacity cache would evict every
  // entry on the way in, so every store would succeed and every load would
  // miss; refusing to build one turns a silently useless object into an error
  // at the point the mistake was made.
  explicit Cache(std::size_t capacity);

  // Not copyable. index_ holds iterators into entries_, and a member-wise copy
  // would give the new object a fresh list while its map still pointed into the
  // *original* one -- so the copy would read freed memory the moment the source
  // died, and erase() on it would be undefined even while both were alive.
  // std::list's node stability is what makes splice() cheap; it is not a
  // property that survives copying the container.
  //
  // A correct copy would have to rebuild index_ from the new list. Nothing here
  // needs one, so the operation is deleted rather than written: the bindings
  // hold the object behind a unique_ptr and never copy it, and a C++ caller who
  // tries gets a compile error instead of the segfault this used to be.
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;

  // Moving is sound and stays available, because moving a std::list transfers
  // the nodes themselves: every iterator the map holds stays valid and simply
  // belongs to the destination afterwards. Declaring the copy operations above
  // would otherwise suppress these entirely.
  Cache(Cache &&) = default;
  Cache &operator=(Cache &&) = default;

  std::size_t capacity() const;
  std::size_t size() const;

  // put - stores value under key, making it the most recently used entry.
  //
  // Returns true if key was not already present. Inserting into a full cache
  // evicts the least recently used entry and counts one eviction; updating an
  // existing key never evicts, whatever the cache's fullness.
  //
  // Throws CacheError if key is empty.
  bool put(std::string_view key, std::string_view value);

  // find - returns a pointer to the value bound to key, or nullptr if absent.
  //
  // Counts one hit or one miss, and on a hit promotes the entry to most
  // recently used. The pointee lives in the cache and stays valid until the
  // next call that could remove that entry (another put, erase, or clear);
  // copy it if it needs to outlive that.
  //
  // Throws CacheError if key is empty.
  const std::string *find(std::string_view key);

  // contains - reports whether key is present, without promoting it and
  // without counting a hit or a miss.
  //
  // Throws CacheError if key is empty.
  bool contains(std::string_view key) const;

  // erase - removes key, returning true if it was present. Not a hit, a miss,
  // or an eviction: the caller asked, so nothing was displaced.
  //
  // Throws CacheError if key is empty.
  bool erase(std::string_view key);

  // clear - drops every entry but keeps the counters.
  //
  // The counters describe the traffic this cache has served, not its current
  // contents, and clear() is a use of the cache rather than a new cache. Rebind
  // the object if a fresh set of numbers is wanted.
  void clear();

  // keys - returns every key, most recently used first.
  //
  // This is what makes eviction order observable from outside, and so testable
  // without reaching into the implementation.
  std::vector<std::string> keys() const;

  Stats stats() const;

private:
  // Entries live in a list ordered most- to least-recently-used; the map holds
  // iterators into it. std::list is the container of choice here because
  // splice() reorders in constant time and leaves every iterator valid, so a
  // promotion never has to touch the map.
  using Entry = std::pair<std::string, std::string>;
  using Entries = std::list<Entry>;

  // std::less<> makes the map heterogeneously comparable, so a lookup keyed by
  // string_view does not have to allocate a std::string first.
  using Index = std::map<std::string, Entries::iterator, std::less<>>;

  Entries entries_;
  Index index_;
  std::size_t capacity_;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  std::uint64_t evictions_ = 0;
};

} // namespace lrukit

#endif // RUST_HOSTED_CPP_CPP_LRUKIT_HPP
