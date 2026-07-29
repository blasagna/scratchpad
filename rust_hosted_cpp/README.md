# rust_hosted_cpp

What a C++ library looks like when Rust is the only thing that builds it, tests
it, and runs it — using [cxx] for the glue.

The demo library is **lrukit**: a fixed-capacity least-recently-used cache. It is
small on purpose — the interesting part is not the eviction policy, it's what the
project *doesn't* contain. There is no `BUILD` file, no `CMakeLists.txt`, no
GoogleTest suite, and no `main.cpp`. `cpp/` is two files:

```sh
$ cargo run -q -p lrukit -- --capacity 2 'put a 1' 'put b 2' 'get a' 'put c 3' keys
stored a
stored b
1
stored c
c
a
```

`b` is gone: `get a` made `a` the most recently used entry, so `b` was what left
to make room for `c`. That eviction rule is C++ code, the assertion that proves
it is Rust code in `rust/tests/cache.rs`, and no C++ compiler runs in this repo
except the one `build.rs` invokes.

This is the counterpart of [`cpp_rust_bindings`](../cpp_rust_bindings), where the
C++ library *is* a first-class Bazel target with its own tests and its own CLI,
and the Rust bindings are a second consumer. Same seam, opposite trade. That area
pays for a second build system and gets a second opinion; this one pays for
neither and gets neither.

## Layout

```
rust_hosted_cpp/
├── cpp/                      the C++ library — no cxx, no Rust, no build files
│   ├── lrukit.hpp              the public header
│   └── lrukit.cpp              the cache, ~140 lines of plain C++20
└── rust/                     crate lrukit — the build, the tests, the program
    ├── build.rs                the only build configuration in the area
    ├── src/shim.hpp/.cpp       the binding layer, the only bilingual code
    ├── src/lib.rs              #[cxx::bridge] + the safe Rust API + seam tests
    ├── src/main.rs             the CLI (clap) — the only executable
    └── tests/cache.rs          the cache's own behavior, in Rust
```

## How it works

Four layers, each with one job:

```
  lrukit::Cache::get("a")                rust/src/lib.rs
        │  safe, owned, Result<Option<String>, CacheError>
        ▼
  ffi::get(Pin<&mut Cache>, &str, …)     the #[cxx::bridge] module,
        │                                plus the C++ cxx-build generates
        ▼
  lrukit::bridge::get(Cache&, …)         rust/src/shim.cpp
        │  rust::Str → std::string_view, pointer-or-null → bool + out-param
        ▼
  lrukit::Cache::find(string_view)       cpp/lrukit.cpp — plain C++
```

`#[cxx::bridge]` is a procedural macro over a module of *declarations*. At build
time two things read it: the macro expands the Rust half into `extern "C"`
declarations, and `cxx-build` (from `build.rs`) parses the same file to emit the
matching C++ half. Because both halves come from one source of truth, a signature
mismatch is a compile error rather than a runtime corruption.

### One build system, and it is cargo's

`build.rs` names the C++ sources, the standard they need, and the warnings they
are held to. Nothing else knows they exist:

```rust
let mut build = cxx_build::bridge("src/lib.rs");
build
    .file("src/shim.cpp")
    .file("../cpp/lrukit.cpp")
    .include("src")
    .include("../cpp")
    .std("c++20")
    .warnings(true)
    .extra_warnings(true)
    .flag_if_supported("-pedantic");
build.warnings_into_errors(!permissive);
build.compile("lrukit_bridge");
```

Two of those lines exist only because there is no Bazel here. `//.bazelrc` sets
`-std=c++20` and `-Wall -Werror -Wextra -pedantic` for every other C++ target in
the repo; this C++ is invisible to it, so the same policy is restated —
`LRUKIT_PERMISSIVE=1` playing the part of `--config=permissive`. Leave those out
and the C++ silently compiles at the compiler's default standard with warnings
off, which is a worse place to be than the sibling area's "compiled twice".

## The techniques, one at a time

Excerpts below are trimmed; read `rust/src/lib.rs` and `rust/src/shim.hpp` for
the commented originals.

**Keep the C++ ignorant of Rust anyway.** `cpp/lrukit.hpp` includes no cxx
header and names no Rust type, even though cargo is the only thing that will ever
compile it. This costs the shim a few conversions and buys the ability to hand
the directory to a C++ build tomorrow. It is also the only reason the layering
can be checked at all: if the library could see `rust::Str`, "the logic is in
C++, the glue is in the shim" would be a habit rather than a fact.

**Bind the object as opaque, the counters as shared.** `Cache` is a `type` with
no fields — Rust only ever holds it behind `UniquePtr` — while `CacheStats` is a
cxx *shared struct*, defined once in the bridge and emitted into both languages:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct CacheStats { hits: u64, misses: u64, evictions: u64, size: usize, capacity: usize }

unsafe extern "C++" {
    include!("shim.hpp");
    #[namespace = "lrukit"]
    type Cache;
    fn stats(cache: &Cache) -> Result<CacheStats>;
}
```

Five plain integers do not need an accessor each. The library still has its own
`lrukit::Stats` of the same shape and the shim copies field by field, because
sharing the struct all the way down would mean `cpp/lrukit.hpp` including a
generated cxx header — the one thing that directory must not do. Naming every
field in that copy is also what makes adding a counter a compile error rather
than a number that silently never gets reported.

**Turn a nullable pointer into an `Option`.** `Cache::find` returns
`const std::string *`, null on a miss. That pointer is valid only until the next
mutation, so it must not reach Rust. The bridge splits the answer in two and
`lib.rs` puts it back together:

```rust
fn get(cache: Pin<&mut Cache>, key: &str, out: Pin<&mut CxxString>) -> Result<bool>;

pub fn get(&mut self, key: &str) -> Result<Option<String>> {
    cxx::let_cxx_string!(out = "");
    if !ffi::get(self.inner.pin_mut(), key, out.as_mut())? {
        return Ok(None);
    }
    ...
}
```

The `Option` shape exists only on the Rust side; the C++ never hears about it.

**Let the signature carry the semantics.** `get` takes `Pin<&mut Cache>` and
`contains` takes `&Cache`, because reading through `get` promotes an entry and
asking through `contains` deliberately does not. A `const` method in C++ becomes
a `&self` method in Rust, and the API's central promise — polling for a key must
not save it from eviction — is visible in the type rather than only in a comment.

**Declare every function `Result`, including the infallible ones.** cxx generates
the try/catch from that declaration; without it a `throw` unwinds into Rust
frames that cannot handle it and the process aborts. `keys` has no error of its
own to report, but it builds a vector of strings, and `std::bad_alloc` is a
`throw` like any other. Where the public signature stays infallible, an
`expect_no_throw` wrapper converts what would have been an abort into a panic.

**Keep caller data out of error messages.** Arguments cross as `rust::Str`, a
pointer/length pair, so a key may contain an interior NUL and arrives intact.
Exception messages cross as `what()`, a C string, so they truncate at the first
NUL and arrive lossy if they are not UTF-8. `lrukit` never puts a key or a value
in a message, which is why that asymmetry is a piece of trivia here rather than
a bug — `cpp_rust_bindings` echoes its input and has to pin the behavior with
tests instead.

## Where the tests go when there is only one language

The sibling area's rule is *test the logic in C++, the seam in Rust*. Delete the
C++ test binary and that rule leaves the library untested, so it is replaced by:

| | covers | why it lives there |
|---|---|---|
| `rust/tests/cache.rs` | eviction order, promotion, counters | the suite that would have been GoogleTest — 20 tests against the public API |
| `rust/src/lib.rs` `mod tests` | the seam: out-parameters, NULs, throw sites, ownership | needs `ffi` internals, so it cannot be an integration test |
| `rust/src/main.rs` `mod tests` | command parsing and output | the CLI is a program, not a library |

`cargo test -p lrukit` runs all 50 (including 5 doctests). Nothing else runs, and
that is the point to internalize: **a behavior not asserted in `rust/` is not
asserted anywhere.**

Two of those tests are worth calling out because they only make sense in this
arrangement. `a_miss_leaves_the_out_parameter_untouched` reaches past the safe
wrapper to `ffi` directly, because the safe wrapper allocates a fresh string every
call and so hides the contract it depends on. And
`many_caches_are_created_and_dropped_without_leaking` runs 5,000 create/drop
rounds — not a leak detector by itself, but a smoke test that `~Cache` really
runs through the `UniquePtr` deleter, and a ready-made target for `valgrind`
against the test binary.

## Best practices this demonstrates

- **The dependency points one way even when nothing forces it to.** `cpp/` could
  cheat here and no build would break. It doesn't, and that is what keeps it a
  library rather than an implementation detail of a Rust crate.
- **Restate what you removed.** Dropping Bazel dropped the C++ standard and the
  warning policy with it. Both are back in `build.rs`, escape hatch included.
- **Absent values and errors get different shapes.** A miss is `Ok(None)`; a
  rejected key is `Err`. A cache that returned `Err` for a miss would make its
  own ordinary business look like a failure.
- **Test the thing nobody else will.** With no second harness, the Rust suite
  inherits the whole library, not just the boundary.

## Build, test, run

```sh
cargo build -p lrukit
cargo test -p lrukit
cargo run -q -p lrukit -- --capacity 4 'put k hello there' 'get k' stats

printf 'put x 1\n# comments and blank lines are skipped\n\nget x\n' | cargo run -q -p lrukit --
LRUKIT_PERMISSIVE=1 cargo build -p lrukit    # C++ warnings stay warnings
```

Exit codes follow the rest of the repo: `0` success, `1` a failure the program
was asked to attempt (a zero capacity), `2` a request that never made sense (an
unknown command). A cache miss is neither — it prints `(miss)` and the run
continues.

`bazel test //...` does not build anything in this directory. That is not an
oversight.

## Gotchas

- **`shim.hpp` and the generated header include each other.** `shim.hpp` needs
  `lrukit/src/lib.rs.h` for the shared `CacheStats`; that header includes
  `shim.hpp` back, because the bridge declares `include!("shim.hpp")`. Both are
  guarded, so it resolves — but drop the guard from `shim.hpp` and the error will
  point somewhere else entirely.
- **`build.rs` must list every C++ input** in its `rerun-if-changed` loop.
  Printing any `rerun-if-changed` disables cargo's default directory watching,
  and `../cpp` is outside the crate regardless. Add a `.cpp`, forget this, and
  edits silently do not rebuild.
- **`-Werror` applies to cxx's generated code too**, since one `cc::Build`
  compiles everything. It is clean today; a cxx upgrade could change that, which
  is what `LRUKIT_PERMISSIVE=1` is for.
- **The C++ has no editor tooling here.** There is no `compile_commands.json`,
  so clangd will flag `std::format` and the shim's includes. `cargo build` is the
  only authority on whether the C++ compiles.
- **`Pin<&mut Cache>`, never `&mut Cache`.** Safe Rust can `mem::swap` through a
  `&mut`, and `Cache`'s map holds iterators into its own list — moving the object
  behind them would be quietly catastrophic.

[cxx]: https://cxx.rs
