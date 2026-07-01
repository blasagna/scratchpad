# text_analyzer (Rust)

Reads a text file and reports line, word, and character counts plus the most
frequent words and characters. A Rust port of the C text analyzer.

## Build & run

```sh
cargo build
cargo run -- path/to/file.txt
cargo run -- --json --top-n 10 path/to/file.txt
cargo test
```

## Code review

Reviewed with `cargo test` (12 tests pass) and `cargo clippy --all-targets -- -D warnings`
(clean). All five source files read closely.

### Overall

Well-crafted, idiomatic Rust. The module split (`analyzer` for logic, `report` for
rendering, thin `main`) is right; error handling propagates `io::Result` properly; the
CLI validates ranges so the core can stay assertion-free; and the doc comments are
thorough and honest about edge cases (byte==char, zero-values degenerate-but-defined,
etc.). Tests cover the meaningful branches including trailing-word-no-newline and
truncation. **No correctness bugs found.** The notes below are polish, not defects.

### Substantive-ish

1. **Byte-at-a-time iteration (`analyzer.rs:84`)** — `reader.bytes()` yields a `Result`
   per byte, and each is `?`-propagated. On a `BufReader` this is correct and fine for an
   exercise, but it's the one thing that would matter at scale: every byte pays iterator +
   branch overhead. The faster idiom is to loop over `reader.fill_buf()` / `consume()` and
   process each returned slice with a plain `for &b in chunk`. Not worth changing unless
   throughput is a goal.

2. **Prefix collision after truncation is silent (`analyzer.rs:93-98`)** — once a word
   hits `max_word_len`, extra alphabetic bytes are dropped but `in_word` stays true, so
   `"internationalization"` and `"internationalized"` merge into the same truncated key
   and their counts combine. This matches the C buffer semantics and is arguably intended,
   but it's the one behavioral surprise a reader might not expect. The `analyze` doc
   mentions truncation but not that truncation can *merge distinct words* — one extra
   sentence there would make it airtight.

### Nits

- **`report.rs:19` and `:25`** — `write!(out, "\nTop words:\n")` /
  `"\nTop characters:\n"` read cleaner as `writeln!(out, "\nTop words:")`. Trivial
  consistency with the surrounding `writeln!` calls.
- **`main.rs:26-52`** — args are parsed as `u64` then cast to `usize`. You could hand
  clap `clap::value_parser!(usize).range(1..)` and drop the three `as usize` casts. The
  current approach avoids platform-dependent parsing, so it's a defensible choice either
  way.
- **Char percentage denominator (`report.rs:27`)** — top-char percentages are over
  `char_count`, which includes spaces/newlines that can never appear in the
  printable-ASCII ranking. That's a reasonable definition ("% of all characters"), just
  worth being aware the percentages intentionally won't sum toward 100% across the shown
  rows.
