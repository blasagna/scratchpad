#include "lrukit.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lrukit {
namespace {

// check_key - rejects the one key every operation refuses.
//
// An empty key is almost always a bug at the call site -- an unset variable, a
// field that failed to parse -- and a cache is the worst place for it to land,
// because storing under "" succeeds and the mistake surfaces much later as a
// wrong hit. Every entry point that takes a key runs this first, so the rule is
// uniform rather than something each caller has to remember.
void check_key(std::string_view key) {
  if (key.empty()) {
    throw CacheError("key must not be empty");
  }
}

} // namespace

std::string format_stats(const Stats &stats) {
  const std::uint64_t lookups = stats.hits + stats.misses;
  // A cache nobody has read has no hit rate; reporting 0.000 rather than a NaN
  // keeps the line parseable and says the true thing, which is that there is
  // nothing to report yet.
  const double hit_rate = lookups == 0 ? 0.0
                                       : static_cast<double>(stats.hits) /
                                             static_cast<double>(lookups);
  return std::format(
      "hits={} misses={} evictions={} size={}/{} hit_rate={:.3f}", stats.hits,
      stats.misses, stats.evictions, stats.size, stats.capacity, hit_rate);
}

Cache::Cache(std::size_t capacity) : capacity_(capacity) {
  if (capacity_ == 0) {
    throw CacheError("capacity must be at least 1");
  }
}

std::size_t Cache::capacity() const { return capacity_; }

std::size_t Cache::size() const { return entries_.size(); }

bool Cache::put(std::string_view key, std::string_view value) {
  check_key(key);

  if (const auto found = index_.find(key); found != index_.end()) {
    // An update, not an insert: the size does not change, so a full cache
    // evicts nothing. Assigning into the existing string reuses its buffer.
    found->second->second.assign(value);
    entries_.splice(entries_.begin(), entries_, found->second);
    return false;
  }

  // Insert first and evict afterwards, even though it means briefly holding
  // capacity_ + 1 entries. The other order -- evict, then insert -- discards a
  // live entry and then has to allocate, so a std::bad_alloc there would leave
  // the cache having lost data it was never asked to drop.
  entries_.emplace_front(std::string(key), std::string(value));
  try {
    index_.emplace(entries_.front().first, entries_.begin());
  } catch (...) {
    entries_.pop_front();
    throw;
  }

  if (entries_.size() > capacity_) {
    index_.erase(entries_.back().first);
    entries_.pop_back();
    ++evictions_;
  }
  return true;
}

const std::string *Cache::find(std::string_view key) {
  check_key(key);

  const auto found = index_.find(key);
  if (found == index_.end()) {
    ++misses_;
    return nullptr;
  }

  ++hits_;
  // splice moves the node itself, so the iterator stored in the map stays
  // valid and the index needs no update.
  entries_.splice(entries_.begin(), entries_, found->second);
  return &found->second->second;
}

bool Cache::contains(std::string_view key) const {
  check_key(key);
  return index_.find(key) != index_.end();
}

bool Cache::erase(std::string_view key) {
  check_key(key);

  const auto found = index_.find(key);
  if (found == index_.end()) {
    return false;
  }
  entries_.erase(found->second);
  index_.erase(found);
  return true;
}

void Cache::clear() {
  // The map points into the list, so it has to go first; the counters stay.
  index_.clear();
  entries_.clear();
}

std::vector<std::string> Cache::keys() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const Entry &entry : entries_) {
    out.push_back(entry.first);
  }
  return out;
}

Stats Cache::stats() const {
  return Stats{
      .hits = hits_,
      .misses = misses_,
      .evictions = evictions_,
      .size = entries_.size(),
      .capacity = capacity_,
  };
}

} // namespace lrukit
