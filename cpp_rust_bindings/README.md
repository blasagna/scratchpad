# cpp_rust_bindings

How to give a C++ library a Rust API, end to end, using [cxx] for the glue.

The demo library is **exprkit**: an arithmetic expression evaluator with
variables (`x = 2`, `pi * r ^ 2`, `sqrt(16)`). It is small on purpose — the
interesting part is not the parser, it's the seam. The same library is reachable
two ways:

```sh
$ bazel run -- //cpp_rust_bindings/cpp:exprkit_cli '2 ^ 10'   # C++ CLI
$ cargo run -q -p exprkit -- '2 ^ 10'                         # Rust CLI
```

Both print `1024`. Both print `exprkit: division by zero` and exit `1` on
`1/0`. That equivalence is the whole claim being demonstrated — and unlike the
sibling [`rust_python_bindings`](../rust_python_bindings), it is *structural*
rather than maintained by hand: see [One formatter, two
CLIs](#one-formatter-two-clis).

This is the mirror image of `rust_python_bindings`, which exports Rust *to* a
higher-level language. Here the foreign library is the C++ one and Rust is the
consumer, so the hazards are different: exceptions instead of error returns,
object lifetime instead of reference counting, two build systems instead of one.

## Layout

```
cpp_rust_bindings/
├── cpp/                      the C++ library — no cxx, no Rust, Bazel only
│   ├── exprkit.hpp             the public header (grammar documented here)
│   ├── exprkit.cpp             recursive-descent parser + Evaluator
│   ├── test_exprkit.cpp        GoogleTest unit tests — these own the logic
│   ├── main.cpp                the C++ CLI
│   └── BUILD                   cc_library + cc_binary + cc_test
└── rust/                     crate exprkit — the bindings and their CLI
    ├── build.rs                compiles the C++ for cargo
    ├── src/shim.hpp/.cpp       the binding layer, the only bilingual code
    ├── src/lib.rs              #[cxx::bridge] + the safe Rust API + unit tests
    ├── src/main.rs             the Rust CLI (clap)
    └── tests/evaluate.rs       integration tests against the public API
```

## How it works

Four layers, each with one job:

```
  exprkit::Evaluator::eval("x = 2")      rust/src/lib.rs
        │  safe, owned, Result<f64, ExprError>
        ▼
  ffi::eval(Pin<&mut Evaluator>, &str)   the #[cxx::bridge] module,
        │                                plus the C++ cxx-build generates
        ▼
  exprkit::bridge::eval(Evaluator&, …)   rust/src/shim.cpp
        │  rust::Str → std::string_view
        ▼
  exprkit::Evaluator::eval(string_view)  cpp/exprkit.cpp — plain C++
```

`#[cxx::bridge]` is a procedural macro over a module of *declarations*. At build
time two things read it: the macro expands the Rust half into `extern "C"`
declarations, and `cxx-build` (from `build.rs`) parses the same file to emit the
matching C++ half. Because both halves come from one source of truth, a
signature mismatch is a compile error rather than a runtime corruption — which
is the entire reason to prefer this over hand-written `extern "C"` blocks.

### Two build systems, one set of sources

The repo builds C++ with Bazel and Rust with cargo, and neither can drive the
other. Rather than pick a winner, `cpp/exprkit.cpp` is compiled twice:

- `bazel build //cpp_rust_bindings/cpp:exprkit` — for the C++ tests and CLI.
- `cargo build -p exprkit` — `build.rs` hands the same `.cpp` to `cxx_build`,
  along with the shim and the generated bridge, producing a static archive that
  the Rust crate links.

Sources are shared; object files are not. This is a real trade — the library is
compiled twice, and the two builds could in principle disagree on flags (Bazel
gets `-std=c++20` from `//.bazelrc`, cargo from `.std("c++20")` in `build.rs`).
The alternative, teaching cargo to consume a Bazel artifact, costs far more than
recompiling a few hundred lines.

## The techniques, one at a time

Excerpts below are trimmed; read `rust/src/lib.rs` and `rust/src/shim.hpp` for
the commented originals.

**Declare the bridge.** The module is private, and everything in it is an
implementation detail:

```rust
#[cxx::bridge(namespace = "exprkit::bridge")]
mod ffi {
    unsafe extern "C++" {
        include!("shim.hpp");

        #[namespace = "exprkit"]
        type Evaluator;

        fn evaluate(text: &str) -> Result<f64>;
        fn eval(evaluator: Pin<&mut Evaluator>, text: &str) -> Result<f64>;
        fn names(evaluator: &Evaluator) -> Vec<String>;
    }
}
```

`unsafe extern "C++"` is required: the keyword is the author asserting that
these signatures really describe the C++, which no compiler can check across the
language boundary. The per-item `#[namespace]` exists because `Evaluator` lives
in `exprkit` while the shim functions live in `exprkit::bridge`.

**An opaque C++ type.** `type Evaluator;` gives Rust a name and nothing else —
no size, no layout — so it can only be held behind a pointer. `UniquePtr<T>` is
`std::unique_ptr<T>`, so ownership moves to Rust with no wrapper object, and the
C++ destructor runs on drop:

```rust
pub struct Evaluator {
    inner: UniquePtr<ffi::Evaluator>,
}
```

Mutating methods take `Pin<&mut Evaluator>`, not `&mut Evaluator`. The reason is
that safe Rust can `mem::swap` anything behind a `&mut`, and swapping two C++
objects byte-for-byte is unsound for any type whose address matters (a
self-referential node, anything that registered a `this`). `Pin` is how cxx
takes that capability away.

**An exception becomes a `Result`.** Declaring the return type as `Result<T>` is
what makes cxx wrap the call in a try/catch:

```rust
fn evaluate(text: &str) -> Result<f64>;
```

It catches `std::exception` and hands `what()` to Rust as a `cxx::Exception`.
This is the single most important line in the project: **without it, a C++
`throw` unwinds into Rust frames that are not prepared for it and the process
aborts.** Note that it catches *any* `std::exception`, so a `std::bad_alloc`
from deep inside the library also arrives as an `Err` instead of crashing.

`cxx::Exception` is then converted to a crate-local error type, so callers never
have to name a cxx type:

```rust
impl From<cxx::Exception> for ExprError {
    fn from(err: cxx::Exception) -> Self {
        ExprError { message: err.what().to_string() }
    }
}
```

**The shim absorbs the signature mismatch.** `cpp/exprkit.hpp` is cxx-free by
design, which leaves two things it cannot express:

- *Strings.* cxx passes borrowed strings as `rust::Str` and owned ones as
  `rust::String`; the library speaks `std::string_view` and `std::string`. There
  is no implicit conversion either way.
- *Member functions.* cxx binds a C++ method directly only when its signature
  already matches the Rust declaration. `Evaluator::eval(std::string_view)` does
  not, so the methods are re-exported as free functions taking the object by
  reference — the shape cxx maps to `Pin<&mut Evaluator>`.

Both are absorbed by `rust/src/shim.cpp`, which is the only translation unit in
the project that knows both languages exist:

```cpp
std::string_view view(rust::Str text) {
  return std::string_view(text.data(), text.size());
}

double eval(Evaluator &evaluator, rust::Str text) {
  return evaluator.eval(view(text));
}
```

`rust::Str` is a pointer/length pair over UTF-8, exactly what `string_view`
wraps, so this borrows rather than copies.

**Owned collections cross without a second copy.** `rust::Vec<rust::String>` is
a real Rust `Vec`, allocated through a generated hook, so building one in C++
and returning it by value hands Rust an owned `Vec<String>`:

```cpp
rust::Vec<rust::String> names(const Evaluator &evaluator) {
  rust::Vec<rust::String> out;
  for (const std::string &name : evaluator.names()) {
    out.push_back(rust::String(name));
  }
  return out;
}
```

## One formatter, two CLIs

`rust_python_bindings` keeps its two CLIs identical by convention: two
formatting functions that a comment says to change together. That works until
someone forgets.

Here the C++ library owns the formatter and exports it:

```cpp
std::string format_value(double value);   // cpp/exprkit.hpp
```

The Rust CLI calls it through the bindings rather than using Rust's own float
formatting. Rust's `{}` and C++'s `std::format("{}", …)` both produce the
shortest round-tripping representation and agree on every value tested — but
"agree today" is not a guarantee across two standard libraries, and routing both
CLIs through one implementation removes the question. The invariant is enforced,
not documented.

Two things still differ, and should:

- **`--help` output.** The C++ CLI hand-rolls its usage text; the Rust one is
  generated by clap. Each argument parser words its own help, exactly as
  `statkit` lets each runtime word its own OS errors.
- **Nothing else.** Results, error messages, and exit codes (`0` ok, `1` runtime
  error, `2` usage error) match byte for byte.

## Best practices this demonstrates

- **Keep the C++ library binding-free.** `cpp/` has no idea Rust exists, so it
  stays buildable and testable with Bazel alone, usable from other C++, and
  reviewable by someone who does not know Rust. The shim is under 70 lines,
  most of them comments and none of them arithmetic.
- **Never let an exception cross the boundary.** Every fallible C++ function is
  declared `-> Result<T>`. A `throw` reaching a Rust frame is an abort, not an
  error.
- **Make the bridge module private and wrap it.** `ffi` traffics in
  `Pin<&mut _>` and `cxx::Exception`; `lib.rs` is the API. That indirection is
  where Rust-native conveniences live — `Result`, `Default`, `Debug`, doctests —
  none of which need to be written in C++.
- **Test the logic in C++, the seam in Rust.** `//cpp_rust_bindings/cpp:test_exprkit`
  owns precedence, parsing, and the error taxonomy. The Rust tests deliberately
  do not re-check any arithmetic; they check that floats arrive bit-for-bit,
  that every throw site becomes an `Err`, that a C++ object survives being moved
  around in a `Vec`, and that a failed call leaves the environment usable.
- **Own C++ objects with `UniquePtr`, not raw pointers.** Drop order and
  destructor calls then follow Rust's rules for free.

## Build, test, run

C++, from anywhere in the repo:

```sh
bazel test //cpp_rust_bindings/...                       # 26 GoogleTest cases
bazel run  -- //cpp_rust_bindings/cpp:exprkit_cli --help
bazel run  -- //cpp_rust_bindings/cpp:exprkit_cli 'r = 3' 'pi * r ^ 2' --names
printf '1 + 1\n' | bazel run -- //cpp_rust_bindings/cpp:exprkit_cli
```

Rust, from anywhere in the repo:

```sh
cargo test -p exprkit                                    # 24 tests, incl. doctests
cargo run  -q -p exprkit -- --help
cargo run  -q -p exprkit -- 'r = 3' 'pi * r ^ 2' --names
printf '1 + 1\n' | cargo run -q -p exprkit
```

To check the two CLIs against each other:

```sh
cargo build -q -p exprkit && bazel build //cpp_rust_bindings/cpp:exprkit_cli
diff <(printf '0.1 + 0.2\n2 ^ 3 ^ 2\n' | bazel-bin/cpp_rust_bindings/cpp/exprkit_cli) \
     <(printf '0.1 + 0.2\n2 ^ 3 ^ 2\n' | target/debug/exprkit)
```

## Gotchas

- **C++ edits need no special step, but do need the right one.** `build.rs`
  prints `cargo:rerun-if-changed` for `../cpp/*`, which is required: those files
  live outside the crate, and printing *any* `rerun-if-changed` disables cargo's
  default "watch the crate directory" behavior. Add a C++ source and you must
  add it to that list too, or edits will silently not rebuild.
- **A C++ exception message is transferred as a C string.** cxx builds the `Err`
  from `what()`, so the message stops at the first NUL byte — while arguments,
  which travel as `rust::Str`, carry a length and pass NULs through intact. The
  asymmetry is pinned by a test in `rust/src/lib.rs`.
- **A message that is not valid UTF-8 is converted lossily.** The C++ tokenizer
  works in bytes, so `evaluate("1 + −")` (U+2212) quotes a lone continuation
  byte; it arrives in Rust as U+FFFD rather than failing. Do not put arbitrary
  bytes in an exception message and expect them back.
- **`include!("shim.hpp")` is resolved by `build.rs`, not by cargo.** The two
  `.include(…)` calls there are what let the generated bridge find the shim and
  the shim find `exprkit.hpp`. A missing include path shows up as a C++
  preprocessor error inside a generated file with an unhelpful path.
- **`bazel run` needs `--` before flags.** `bazel run //…:exprkit_cli --help`
  prints *bazel's* help; `bazel run -- //…:exprkit_cli --help` prints the
  program's. The Rust CLI needs the same separator for `cargo run`.
- **The C++ is compiled twice**, once per build system. `bazel test` passing
  does not prove `cargo test` will build, and vice versa — a change to
  `cpp/exprkit.hpp` should be checked with both.

[cxx]: https://cxx.rs
