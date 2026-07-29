# rust_hosted_cpp

A C++ library with no build system, no test binary, and no CLI of its own,
built and driven entirely from Rust through [cxx](https://cxx.rs). The demo
library is `lrukit` (a fixed-capacity LRU cache). Two pieces: `cpp/` (two files,
plain C++20, no build config at all) and `rust/` (crate `lrukit` — the
`#[cxx::bridge]`, a shim, the safe API, every test, and the only executable).
The full walkthrough is in [`README.md`](README.md).

This is the counterpart of [`../cpp_rust_bindings`](../cpp_rust_bindings), which
binds a C++ library that is *also* a first-class Bazel target with its own
GoogleTest suite and its own CLI. Same seam, opposite trade: there is nothing to
keep in sync here, and equally nothing to fall back on — cargo is the only thing
that compiles this C++, and Rust is the only thing that tests it.

## Commands

```sh
cargo test -p lrukit        # the whole suite: 50 tests (13 seam, 12 CLI, 20 logic, 5 doctests)
cargo run -q -p lrukit -- --capacity 2 'put a 1' 'put b 2' 'get a' 'put c 3' keys stats
printf 'put x 1\nget x\nstats\n' | cargo run -q -p lrukit -- --capacity 4

LRUKIT_PERMISSIVE=1 cargo build -p lrukit   # C++ warnings stay warnings
```

There is deliberately no `bazel` line here. `bazel test //...` does not build a
single file in this directory, and adding a `BUILD` would undo the point of the
area.

## Invariants

- **`cpp/` must never depend on cxx.** The library is built only by cargo, which
  makes it *tempting* to let a generated cxx header leak in — `rust::Str` would
  save the shim a conversion, and the shared `CacheStats` would save a copy.
  Both would make an ordinary C++ library permanently unbuildable by anything
  but this crate. The one-way dependency is what keeps "cargo hosts the C++" a
  choice rather than a trap.
- **The Rust tests own the C++ logic, because nothing else does.** `tests/cache.rs`
  covers eviction order, promotion, and the counters — the suite that would be
  GoogleTest in the sibling area. `src/lib.rs`'s `mod tests` covers the seam. Do
  not carry over that area's "don't re-test the logic in Rust" rule; here that
  would leave `cpp/lrukit.cpp` untested. A behavior not asserted in `rust/` is
  not asserted anywhere.
- **Every bridge function is declared `-> Result<T>`, without exception.** That
  declaration is what makes cxx generate the try/catch. Without it a `throw`
  unwinds into Rust frames that cannot handle it and the process aborts. This
  includes functions with no error of their own to report — `keys` builds a
  vector, and any call can meet `std::bad_alloc`, which is a `throw` like any
  other. Where the public signature stays infallible, the wrapper funnels
  through `expect_no_throw`, converting an abort into a panic. There is no
  partial credit here; a new binding that is not `Result` is a latent crash.
  `every_throw_site_arrives_as_an_err` in `src/lib.rs` is the tripwire, and a
  new `throw` in the C++ needs a new line in it.
- **`build.rs` is the whole build configuration**, so anything the C++ needs is
  stated there or does not happen: `.std("c++20")` (the library uses
  `std::format`), the include paths, and the warning policy. Nothing else in the
  repo compiles this code, so there is no `//.bazelrc` supplying defaults.
- **C++ warnings are errors by default**, via
  `.warnings(true).extra_warnings(true).flag_if_supported("-pedantic")` plus
  `warnings_into_errors`, matching the repo's `--config=strict` Bazel default.
  `LRUKIT_PERMISSIVE=1` is the `--config=permissive` equivalent. Deleting this
  would make this the only C++ in the repo compiled with warnings off.
- **`build.rs` must list every C++ input** in its `rerun-if-changed` loop. The
  sources in `../cpp` are outside the crate, and printing any `rerun-if-changed`
  disables cargo's default directory watching. Add a `.cpp` and forget this, and
  edits silently do not rebuild.
- **`Pin<&mut Cache>`, never `&mut Cache`.** Safe Rust can `mem::swap` through a
  `&mut`, which is unsound for a C++ object whose address matters — and `Cache`
  is such an object twice over, since its `std::map` holds iterators into its own
  `std::list`.
- **No error message may contain a key or a value.** `what()` crosses as a C
  string, so an embedded NUL would truncate the message and non-UTF-8 bytes
  would arrive lossily. Arguments cross as `rust::Str`, a pointer/length pair,
  and carry both fine. Keeping caller data out of messages means that asymmetry
  never becomes a bug; `keys_and_values_may_contain_an_interior_nul` pins the
  half that must keep working.
- **`contains` takes `&Cache`, `get` takes `Pin<&mut Cache>`.** The difference is
  the API's central promise — polling for a key must not save it from eviction —
  and the signatures are what enforce it. Making `contains` promote would be a
  one-line change in the C++ that no type checker would object to;
  `a_contains_does_not_promote_the_entry_it_found` is the guard.

## Gotcha: shim.hpp and the generated header include each other

`shim.hpp` includes `lrukit/src/lib.rs.h` to see the shared `CacheStats`, and
that generated header includes `shim.hpp` right back, because the bridge module
declares `include!("shim.hpp")`. The cycle is real and harmless: both files are
`#include`-guarded, whichever is reached first wins, and `CacheStats` is defined
before the declarations that need it either way. If you ever drop the guard from
`shim.hpp`, this is what breaks, and the error will point somewhere else.

The include path for the generated header is `<crate name>/<path to the bridge
module>.h` — `lrukit/src/lib.rs.h` — and cxx-build puts its directory on the
include path automatically, so `build.rs` needs no entry for it.

## Gotcha: an absent value crosses as two things, not one

C++'s `Cache::find` returns `const std::string *`, null on a miss. A raw pointer
into the cache is exactly what must not reach Rust — the next `put` may free it —
so the bridge splits the answer: `fn get(..., out: Pin<&mut CxxString>) -> Result<bool>`,
where the `bool` says whether it was found and the shim copies into `out` only on
a hit. `Cache::get` in `lib.rs` reassembles the two into `Option<String>`.

Two consequences. The out-parameter is allocated fresh per call
(`cxx::let_cxx_string!`) rather than cached in the struct, because a reusable
buffer would have to be owned by `&mut self` and any `&str` handed out of it
would pin the cache for the caller's lifetime. And the shim leaves `out`
untouched on a miss, which is invisible through the safe API and therefore
tested directly against `ffi` in `a_miss_leaves_the_out_parameter_untouched`.
