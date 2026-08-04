# text_analyzer

A streaming CLI that reports text statistics (line/word/char counts, most-common
words and characters with frequencies, word-length quantiles, optional `--json`).
The same program is implemented three times — `c/` and `cpp/` (Bazel), `rust/`
(cargo workspace member `text_analyzer`) — with matching semantics. The full
narrative, performance history, and open TODO are in [`README.md`](README.md).

## Core invariant: three-port byte-identical parity

**All three ports must produce byte-for-byte identical output for the same input,
in both text and JSON.** This is the property the golden tests and `bench/run.sh
--check` guard. Any behavior change has to land in all three ports together.

## Must-knows for staying in parity

- **Input is ASCII bytes.** `Characters:` counts bytes, not Unicode codepoints. A
  word is a maximal run of ASCII letters `[A-Za-z]`; every non-ASCII byte counts as
  a character, separates words, and is neither a digit nor punctuation. So `café` is
  one word (`caf`) and five characters. No port calls `setlocale`, so C/C++
  `isalpha`/`isdigit`/`ispunct` run in the `"C"` locale and match Rust's `is_ascii_*`
  — which is *why* they agree. **Full Unicode was considered and deliberately
  rejected**; do not reach for `char::is_alphabetic` or locale-aware ctype.
- **Words are lowercased** for counting; digits and punctuation are separators.
- **Word lengths** are true lengths, unaffected by `--max-word-len` truncation of the
  stored spelling. Quantiles use nearest rank (`ceil(p/100 * n)`, 1-based) on a length
  histogram — integers, no interpolation.
- **Blank lines** have no non-whitespace character. A final line without a trailing
  newline is not counted as a line at all (so never as blank).
- **Ranking tie-break** is count descending, then character/word ascending.
- **JSON must match byte-for-byte too**: floats render as `%.4f` / `{:.4}` with
  round-half-to-even (Rust uses a `FixedFloatFormatter`; do not pre-round). C and C++
  follow serde's pretty layout. Multiple files are one concatenated stream; stdin is
  read when no file is given or the file arg is `-`.

## Argument parsing

Each port uses its ecosystem's parser — `getopt_long` in C, CLI11 in C++, clap
in Rust — so `--help` text and argument diagnostics differ between them and
nothing compares them. What is shared is the flag surface and the accepted
values: the three integer options take `[0-9]+` above zero in every port, which
is why the C++ port validates them with `parse_positive` behind a
`CLI::Validator` rather than binding an `unsigned int` with
`CLI::PositiveNumber` — CLI11's own conversion would also accept `--top-n " 5"`.
A read failure still exits 1; a bad argument exits with whatever the parser
chose.

## Commands

```sh
bazel test //text_analyzer/...          # C and C++ unit (test_analyzer) + golden (test_golden) tests
cd text_analyzer/rust && cargo test     # Rust unit + golden + property tests
./text_analyzer/bench/run.sh --check    # direct three-way byte-parity diff, text and JSON
```

## Testing layout (folded in here)

- **Golden tests.** `testdata/` holds 13 hand-reviewed edge cases, each with expected
  text and JSON under two configs (defaults and a fixed alternate `top_n=3,
  max_word_len=5`). Bazel can't build the Cargo binary, so rather than shelling out,
  **each port diffs its own rendering against the same committed goldens**
  (`//text_analyzer/{c,cpp}:test_golden`, `rust/tests/golden.rs`), enforcing parity
  transitively. The alternate config is duplicated as `kAltConfig`/`ALT_CONFIG` in the
  three golden tests and `ALT_FLAGS` in `regenerate.sh` — keep them in sync.
- **Property tests** live in `rust/tests/property.rs` (proptest over the public API:
  chunk invariance, stat consistency, bounded/sorted rankings, JSON always parses, …).
- **Regenerating goldens / benchmarking** are step-by-step workflows — use the
  **text-analyzer-goldens** and **text-analyzer-bench** skills. Timing needs an
  optimized build (`--config=opt`); the default Bazel build is `-O0`.
