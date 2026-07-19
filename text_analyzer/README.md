# text analyzer

A mini project from the little book of C.

Build a simple tool that reads a text file and reports basic statistics, including number of lines, words, and characters it contains. 

Requirements:
1. take a file name as a required command line argument.
1. open the file safely, reporting errors if it does not exist or cannot be read.
1. read the file by streaming its contents (don't load the whole file into memory at once)
1. count total characters, total words, and total lines.
1. compute the most commonly used words and characters, both as counts and frequencies.
1. print a summary at the end to stdout
1. include a CLI option to print the output in json format
1. avoid magic numbers or repeated constants throughout the code. Reuse constant values, making them settable by command line options with sane defaults, rejecting invalid values (e.g. non-positive numbers) with a clear error.
1. structure the project as a main CLI executable using one or more libraries. Write unit tests of the libaries. 
1. Add a help menu to the CLI program

## TODO

### Done

1. ~~report word length statistics: mean, min, max, quantiles 25, 50, 75~~
1. ~~count blank lines, digits, and punctuation marks~~
1. ~~support reading from stdin~~
1. ~~support multiple files~~

### Performance

Reproduce with `./bench/run.sh`, which builds all three ports optimized,
generates the corpora, checks output parity, and prints this table. Both corpora
are the same size and word count; only vocabulary size differs, which isolates
word-lookup cost from I/O cost.

| corpus (2.7 MB, 400k words) | C | C++ | Rust |
|---|---|---|---|
| 40,000 distinct | 0.021s | 0.044s | 0.026s |
| 50 distinct | 0.009s | 0.027s | 0.016s |

Before this work, C took **11.25s** on the 40,000-distinct corpus and 0.030s on
the 50-distinct one — same bytes, same word count.

1. ~~**Replace C's word table with a real hash table.**~~ (done) `word_table_add`
   did a linear `strcmp` scan over every distinct word seen so far, making the
   cost quadratic in vocabulary. It is now an open-addressed table (FNV-1a,
   linear probing, power-of-two capacity, doubling at 70% load). 11.25s → 0.021s,
   a 536x improvement, and C is now the fastest of the three ports.
1. ~~**Shrink `WordFreq` in C.**~~ (done) Table entries now hold a `const char *`
   into a bump-allocated arena instead of an inline `char[256]`: 16 bytes per
   entry rather than 264, and rehashing moves pointers rather than copying
   strings. The public `WordFreq` in `TextStats` deliberately keeps its inline
   buffer — it holds only `top_n` entries, so it was never the memory problem,
   and leaving it alone kept the API and all existing tests unchanged.
1. ~~**Bulk reads instead of byte-at-a-time.**~~ (done for C; premise corrected)
   C's `fgetc` loop became a `fread` loop over a 64 KB buffer, worth 3x on the
   I/O-bound path (0.030s → 0.009s at 50 distinct words).

   The original justification for this item — "C++ is 5x slower than Rust" — was
   a measurement error: it compared a Bazel `fastbuild` (`-O0`) binary against
   `cargo --release`. Optimized, the gap is 1.6x, which does not justify
   restructuring the C++ or Rust read loops. **The default Bazel build is `-O0`;
   use `bazel build --config=opt` for any timing.**

   A related discovery: the project could not build optimized at all until now.
   `-Werror=stringop-truncation` fired on a `strncpy` in `word_table_add` at
   `-O2`, so every build had been unoptimized. Merely enabling optimization was
   worth 3.4x for C++ — more than this item proposed.

### Correctness and implementation

1. ~~**Decide what to do about UTF-8.**~~ (decided: ASCII) Everything is
   byte-oriented, so `Characters:` is really a byte count and `café` tokenizes as
   `caf` plus two stray bytes. **Resolved by committing to ASCII** and saying so
   in the READMEs, `--help`, and the header docs — no behavior change.

   Full Unicode was considered and rejected. It would have needed a Unicode
   letter table *and* a case-folding table (words are lowercased, so `CAFÉ` and
   `café` must fold together), generated from one source and used by all three
   ports — including Rust, which could **not** use `char::is_alphabetic`, since
   that tracks whatever Unicode version the toolchain ships and would silently
   drift from a hand-written C table. Staying ASCII keeps the parity property
   trivially true. See the *Implementations* section for the exact contract.
1. ~~**Simplify C's top-chars ranking.**~~ (done) `analyzer_finish` sorted a
   256-long array, then for each rank rescanned `'!'..'~'` for a matching count
   and wrote `-1` sentinels to mark entries consumed. It now collects the
   printable characters with nonzero counts into a 94-entry stack array and sorts
   with `cmp_char_freq_desc` (count descending, ties ascending by character),
   mirroring C++ and Rust. Output is unchanged — the old inner scan ran in
   ascending ASCII order, which is the same tie-break, now made explicit.
1. ~~**Fix the `int` overflow in table growth**~~ (done as part of the hash table
   rewrite). All capacity math is now `size_t`, and the power-of-two rounding
   saturates rather than wrapping.
1. ~~**Consider pimpl for the C++ `Analyzer`.**~~ (done) Members moved into a
   `struct Analyzer::Impl` behind a `unique_ptr`, taking `<array>`, `<climits>`,
   and `<unordered_map>` out of `analyzer.hpp` along with the four internal
   constants. The destructor and move operations are declared in the header and
   defined in the `.cpp` where `Impl` is complete, which `unique_ptr` requires.
1. ~~**JSON float formatting differs between ports.**~~ (done) JSON is now
   byte-identical across all three ports, so `--json` can be covered by
   cross-port diffs alongside text output. Two things had to change:

   - **Floats.** Rust now serializes through a `FixedFloatFormatter` wrapping
     serde_json's `PrettyFormatter`, overriding `write_f64` to emit `{:.4}`
     instead of the shortest round-trip form. Matching in the other direction
     would have meant implementing Ryu/Grisu in C.
   - **Values, not just spelling.** Rust's `frequency()` pre-rounded with
     `f64::round`, which rounds halfway cases *away from zero*, so 5/32 rendered
     as `0.1563` where `printf("%.4f")` gives `0.1562`. The pre-rounding is gone;
     the formatter now performs the single rounding step, and Rust's `{:.4}`
     rounds half to even exactly as `printf` does.
   - **Layout.** C and C++ adopted serde's pretty layout for array elements
     (each ranked entry expanded across lines rather than kept on one).

### Testing

1. ~~**Cross-port golden tests.**~~ (done) Parity was previously maintained by
   eyeball — a tie-break bug in C's word ranking survived until someone happened
   to diff the outputs.

   `testdata/` now holds 13 hand-reviewed edge cases (empty input, blank lines,
   CRLF, count ties, truncation, JSON escapes, non-ASCII and binary bytes,
   quantile boundaries, halfway rounding), each with expected text and JSON under
   two configs — defaults and a fixed alternate (`top_n=3, max_word_len=5`).

   Bazel cannot build the Cargo binary, so rather than shelling out to all three
   ports, **each port compares its own rendering against the same committed
   goldens** — which enforces parity transitively while letting each test run in
   its native runner (`//text_analyzer/{c,cpp}:test_golden` under `bazel test`,
   `tests/golden.rs` under `cargo test`). `bench/run.sh --check` still does a
   direct three-way diff and now covers JSON as well as text.

   Regenerate with `testdata/regenerate.sh`, which verifies all three ports agree
   *before* writing anything, so a single-port bug cannot be baked into the
   expected output. `testdata/make_inputs.sh` builds the inputs from `printf`
   escapes, keeping the binary and no-trailing-newline cases reviewable.
1. **CLI-level integration tests.** No port covers argument parsing, stdin, `-`,
   multiple files, exit codes, or error messages. Currently the largest untested
   surface — and now the only open testing item.
1. ~~**Property tests**~~ (done) `rust/tests/property.rs`, using proptest over
   the public API. Eight properties: chunk invariance (feeding N random chunks
   equals feeding the whole, which directly guards the multi-file accumulator),
   `analyze` agreeing with the accumulator, `char_count`/`line_count` matching
   the raw bytes, `blank_lines <= lines`, word-length stats ordered and
   internally consistent, rankings bounded and sorted with the documented
   tie-break, `render_json` always parsing, and `render_text` reporting its
   totals. Inputs come from a text-weighted byte generator plus unrestricted
   bytes, with configs spanning the degenerate-but-defined settings.

### Features

1. **`--per-file` breakdown.** Per-file sections plus a combined total, instead
   of the current aggregate-only report. Now cheap: construct one accumulator per
   file rather than sharing one.
1. **Expose the full length histogram.** It is already computed and then thrown
   away after the quantiles are taken, so `--histogram` is nearly free.
1. **Content options:** `--min-word-len`, stopword filtering, a case-sensitive
   mode, and bigrams / n-grams.
1. **Sentence counting and readability scores** (Flesch-Kincaid). Needs sentence
   segmentation, which is genuinely interesting given the tokenizer currently
   treats `.` as a plain separator.

## Testing

```sh
bazel test //text_analyzer/...          # C and C++ unit + golden tests
cd text_analyzer/rust && cargo test     # Rust unit + golden + property tests
./text_analyzer/bench/run.sh --check    # direct three-way diff, text and JSON
```

The golden corpus in `testdata/` is the main guard on the three-port parity
property; see the *Testing* TODO section above for how it is laid out and
regenerated.

## Implementations

The same program is implemented three times — `c/`, `cpp/`, and `rust/` — with
matching semantics, so all three produce identical output for the same input, in
both text and JSON form. Notes on the shared behavior:

- **Input is ASCII bytes.** `Characters:` counts bytes, not Unicode codepoints.
  A word is a maximal run of ASCII letters `[A-Za-z]`; every non-ASCII byte
  counts as a character, acts as a word separator, and is counted as neither a
  digit nor a punctuation mark. So `café` is one word (`caf`) and five
  characters. No port calls `setlocale`, so the C and C++ `isalpha`/`isdigit`/
  `ispunct` calls run in the `"C"` locale and are ASCII-only by definition,
  matching Rust's `is_ascii_*` — which is *why* the three agree.
- **Words** are lowercased for counting. Digits and punctuation are separators,
  never part of a word.
- **Word lengths** are true lengths, unaffected by `--max-word-len` truncation of
  the stored spelling. Quantiles use nearest rank (`ceil(p/100 * n)`, 1-based) on
  a length histogram, so they are integers and need no interpolation.
- **Blank lines** are lines with no non-whitespace character. A final line
  without a trailing newline is not counted as a line at all, and so is never
  counted as blank.
- **Multiple files** are analyzed as a single concatenated stream: one combined
  report, not per-file sections. A word split across a file boundary counts once.
- **stdin** is read when no file is given, or when the file argument is `-`.