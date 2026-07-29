//! The cache's own behavior: eviction order, promotion, and the counters.
//!
//! In `../../cpp_rust_bindings` this file would be a GoogleTest suite next to
//! the C++, and the Rust tests would deliberately not repeat it. There is no
//! C++ test binary here, so this is the suite -- the rule that replaces "test
//! the logic in C++, the seam in Rust" is **the logic lives here, the seam
//! lives in `src/lib.rs`**. Nothing else covers `../cpp/lrukit.cpp`, so a
//! behavior that is not asserted below is not asserted anywhere.
//!
//! These tests use only the public API, which is also the only way to reach the
//! library at all: there is no C++ caller that could poke at internals.

use lrukit::Cache;

/// Builds a cache and fills it, failing the test on any rejected key.
fn cache_of(capacity: usize, entries: &[(&str, &str)]) -> Cache {
    let mut cache = Cache::new(capacity).expect("capacity is at least 1");
    for (key, value) in entries {
        cache.put(key, value).expect("keys here are non-empty");
    }
    cache
}

#[test]
fn a_cache_of_zero_capacity_is_refused() {
    let error = Cache::new(0).expect_err("zero capacity is rejected");

    assert_eq!(error.message(), "capacity must be at least 1");
}

#[test]
fn an_empty_key_is_refused_by_every_operation() {
    let mut cache = cache_of(4, &[("k", "v")]);

    assert!(cache.put("", "v").is_err());
    assert!(cache.get("").is_err());
    assert!(cache.contains("").is_err());
    assert!(cache.remove("").is_err());
}

#[test]
fn a_new_cache_is_empty() {
    let cache = cache_of(4, &[]);

    assert!(cache.is_empty());
    assert_eq!(cache.len(), 0);
    assert_eq!(cache.capacity(), 4);
    assert_eq!(cache.keys(), Vec::<String>::new());
}

#[test]
fn keys_are_listed_most_recently_used_first() {
    let cache = cache_of(4, &[("a", "1"), ("b", "2"), ("c", "3")]);

    assert_eq!(cache.keys(), ["c", "b", "a"]);
}

#[test]
fn a_full_cache_evicts_the_least_recently_used_entry() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);

    cache.put("c", "3").unwrap();

    assert_eq!(cache.keys(), ["c", "b"]);
    assert_eq!(cache.get("a").unwrap(), None);
    assert_eq!(cache.stats().evictions, 1);
}

#[test]
fn a_get_promotes_the_entry_it_found() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);

    assert_eq!(cache.get("a").unwrap(), Some("1".to_string()));
    cache.put("c", "3").unwrap();

    // "a" was the oldest by insertion and is the newest by use, so "b" leaves.
    assert_eq!(cache.keys(), ["c", "a"]);
    assert_eq!(cache.get("b").unwrap(), None);
}

#[test]
fn a_contains_does_not_promote_the_entry_it_found() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);

    assert!(cache.contains("a").unwrap());
    cache.put("c", "3").unwrap();

    // This is the whole difference between contains and get: polling for a key
    // must not save it from eviction, or a monitoring loop would keep alive
    // exactly the entries it was only asking about.
    assert_eq!(cache.keys(), ["c", "b"]);
    assert_eq!(cache.get("a").unwrap(), None);
}

#[test]
fn a_missing_get_promotes_nothing_and_evicts_nothing() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);

    assert_eq!(cache.get("absent").unwrap(), None);

    assert_eq!(cache.keys(), ["b", "a"]);
    assert_eq!(cache.len(), 2);
}

#[test]
fn a_repeated_put_updates_in_place_and_promotes() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);

    assert!(!cache.put("a", "9").unwrap(), "a was already present");

    assert_eq!(cache.len(), 2, "an update does not grow the cache");
    assert_eq!(cache.stats().evictions, 0, "and so evicts nothing");
    assert_eq!(cache.keys(), ["a", "b"]);
    assert_eq!(cache.get("a").unwrap(), Some("9".to_string()));
}

#[test]
fn put_reports_whether_the_key_was_new() {
    let mut cache = cache_of(4, &[]);

    assert!(cache.put("k", "1").unwrap());
    assert!(!cache.put("k", "2").unwrap());
}

#[test]
fn a_cache_of_capacity_one_holds_only_the_newest_entry() {
    let mut cache = cache_of(1, &[("a", "1")]);

    cache.put("b", "2").unwrap();

    assert_eq!(cache.keys(), ["b"]);
    assert_eq!(cache.len(), 1);
    assert_eq!(cache.stats().evictions, 1);
}

#[test]
fn removing_an_entry_frees_a_slot_without_counting_an_eviction() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);

    assert!(cache.remove("a").unwrap());
    assert!(!cache.remove("a").unwrap(), "already gone");

    cache.put("c", "3").unwrap();

    assert_eq!(cache.keys(), ["c", "b"]);
    assert_eq!(
        cache.stats().evictions,
        0,
        "the caller asked, so nothing was displaced"
    );
}

#[test]
fn clear_drops_the_entries_and_keeps_the_counters() {
    let mut cache = cache_of(2, &[("a", "1"), ("b", "2")]);
    cache.get("a").unwrap();
    cache.get("zz").unwrap();
    cache.put("c", "3").unwrap();

    cache.clear();

    assert!(cache.is_empty());
    assert_eq!(cache.keys(), Vec::<String>::new());
    assert_eq!(cache.capacity(), 2, "capacity is a property of the cache");

    let stats = cache.stats();
    assert_eq!(
        (stats.hits, stats.misses, stats.evictions),
        (1, 1, 1),
        "the counters describe traffic served, not current contents"
    );
    assert_eq!(stats.size, 0);
}

#[test]
fn the_cache_still_works_after_being_cleared() {
    let mut cache = cache_of(2, &[("a", "1")]);

    cache.clear();
    cache.put("b", "2").unwrap();

    assert_eq!(cache.get("b").unwrap(), Some("2".to_string()));
    assert_eq!(cache.keys(), ["b"]);
}

#[test]
fn hits_and_misses_are_counted_only_by_get() {
    let mut cache = cache_of(4, &[("a", "1")]);

    cache.contains("a").unwrap();
    cache.contains("zz").unwrap();
    cache.remove("zz").unwrap();
    cache.put("b", "2").unwrap();

    let quiet = cache.stats();
    assert_eq!((quiet.hits, quiet.misses), (0, 0));

    cache.get("a").unwrap();
    cache.get("zz").unwrap();

    let loud = cache.stats();
    assert_eq!((loud.hits, loud.misses), (1, 1));
}

#[test]
fn stats_reports_the_current_size_and_the_fixed_capacity() {
    let cache = cache_of(4, &[("a", "1"), ("b", "2")]);
    let stats = cache.stats();

    assert_eq!((stats.size, stats.capacity), (2, 4));
    assert_eq!(stats.size, cache.len());
}

#[test]
fn the_hit_rate_is_zero_before_anything_is_read() {
    let cache = cache_of(4, &[("a", "1")]);

    // A rate of 0/0 has no honest number; the library prints 0.000 rather than
    // a NaN so the line stays parseable.
    assert_eq!(
        cache.stats().to_string(),
        "hits=0 misses=0 evictions=0 size=1/4 hit_rate=0.000"
    );
}

#[test]
fn values_are_copies_and_do_not_change_under_the_caller() {
    let mut cache = cache_of(2, &[("a", "first")]);

    let held = cache.get("a").unwrap().unwrap();
    cache.put("a", "second").unwrap();
    cache.put("b", "other").unwrap();
    cache.put("c", "another").unwrap(); // evicts "a"

    // The C++ pointer this came from is long dead; the String is Rust's.
    assert_eq!(held, "first");
    assert_eq!(cache.get("a").unwrap(), None);
}

#[test]
fn many_caches_are_created_and_dropped_without_leaking() {
    // Not a leak detector -- it is a smoke test that ~Cache actually runs
    // through the UniquePtr deleter. Under `valgrind --leak-check=full` on the
    // test binary it becomes one.
    for round in 0..5_000 {
        let mut cache = cache_of(4, &[]);
        for entry in 0..8 {
            cache
                .put(&format!("key-{entry}"), &format!("value-{round}-{entry}"))
                .unwrap();
        }
        assert_eq!(cache.len(), 4);
        assert_eq!(cache.stats().evictions, 4);
    }
}

#[test]
fn a_large_value_survives_the_round_trip() {
    let mut cache = cache_of(2, &[]);
    let big = "x".repeat(1 << 20);

    cache.put("big", &big).unwrap();

    assert_eq!(cache.get("big").unwrap(), Some(big));
}
