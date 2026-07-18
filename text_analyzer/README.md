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

1. **Decide what to do about UTF-8.** Everything is byte-oriented, so
   `Characters:` is really a byte count, and `café` tokenizes as `caf` plus a
   stray byte. Either commit to ASCII explicitly in the docs and CLI help, or
   decode UTF-8 and count codepoints. Worth an explicit decision rather than
   drift; it is the largest remaining semantic gap.
1. **Simplify C's top-chars ranking.** `analyzer_finish` sorts a 256-long array,
   then for each rank rescans `'!'..'~'` for a matching count and writes `-1`
   sentinels to mark entries consumed. Correct, but O(top_n × 256) and hard to
   follow; C++ and Rust just build a vector and sort it. Making C match would
   remove the last structural divergence between the ports.
1. ~~**Fix the `int` overflow in table growth**~~ (done as part of the hash table
   rewrite). All capacity math is now `size_t`, and the power-of-two rounding
   saturates rather than wrapping.
1. **Consider pimpl for the C++ `Analyzer`.** Giving the class members pulled
   `<unordered_map>` and `<array>` into `analyzer.hpp`. Fine for a learning
   project, but it is a real tradeoff worth revisiting.
1. **JSON float formatting differs between ports.** serde prints `3.0` where
   `%.4f` prints `3.0000`, so JSON output is not byte-identical across ports the
   way text output is. Predates the multi-file work (it already affected
   `frequency`). Either match formatting or document text as the parity target.

### Testing

1. **Cross-port golden tests.** The premise of this project is one program
   written three times, but parity is currently maintained by eyeball — a
   tie-break bug in C's word ranking survived until someone happened to diff the
   outputs. `bench/run.sh --check` now diffs all three ports over the benchmark
   corpora, which is a start, but it is not wired into `bazel test` and covers
   only two inputs. Promoting it to a real golden-file suite over a corpus of
   edge cases is still the test that best matches the design goal.
1. **CLI-level integration tests.** No port covers argument parsing, stdin, `-`,
   multiple files, exit codes, or error messages. Currently the largest untested
   surface.
1. **Property tests** (e.g. proptest in Rust): feeding input in N random chunks
   equals feeding it whole; `blank_lines <= lines`; `min <= mean <= max`; top-N
   counts sum to at most `word_count`. The chunk-invariance property directly
   guards the accumulator refactor.

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

## Implementations

The same program is implemented three times — `c/`, `cpp/`, and `rust/` — with
matching semantics, so all three produce identical text output for the same
input. Notes on the shared behavior:

- **Words** are maximal runs of alphabetic ASCII, lowercased. Digits and
  punctuation are separators, never part of a word.
- **Word lengths** are true lengths, unaffected by `--max-word-len` truncation of
  the stored spelling. Quantiles use nearest rank (`ceil(p/100 * n)`, 1-based) on
  a length histogram, so they are integers and need no interpolation.
- **Blank lines** are lines with no non-whitespace character. A final line
  without a trailing newline is not counted as a line at all, and so is never
  counted as blank.
- **Multiple files** are analyzed as a single concatenated stream: one combined
  report, not per-file sections. A word split across a file boundary counts once.
- **stdin** is read when no file is given, or when the file argument is `-`.