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

Measured on a 2.7 MB file, 400k words, on the same input size and word count —
the only variable is vocabulary size:

| distinct words | C | C++ | Rust |
|---|---|---|---|
| 50 | 0.033s | — | — |
| 40,000 | **11.58s** | 0.150s | 0.028s |

1. **Replace C's word table with a real hash table.** `word_table_add`
   (`c/analyzer.c`) does a linear `strcmp` scan over every distinct word seen so
   far, so cost is quadratic in vocabulary — the entire gap above. C++ and Rust
   use hash maps and don't degrade. Worth roughly 350x on realistic prose, and
   the highest-value item in this list.
1. **Shrink `WordFreq` in C.** It embeds `char word[MAX_WORD_BUF]`, so every
   entry costs 264 bytes regardless of the actual word: 40k words is ~10 MB, and
   each `realloc` copies all of it. Interning into a char arena with `char*`
   entries would cut memory ~20x and make growth cheap. Pairs naturally with the
   hash table.
1. **Bulk reads instead of byte-at-a-time.** Lower priority — C sustains
   ~80 MB/s once the table is fixed. But C++ is 5x slower than Rust; the suspects
   are `istream::get()` sentry overhead per char and hashing `std::string` keys.
   Use `rdbuf()->sbumpc()` in C++, `fill_buf`/`consume` in Rust, `fread` into a
   buffer in C.

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
1. **Fix the `int` overflow in table growth** (`t->capacity * 2` in
   `word_table_add`). Already noted in `c/README.md`, and `--word-table-cap` is
   user-controlled.
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
   outputs. A script that runs all three binaries over a corpus and diffs would
   catch that automatically. This is the test that matches the actual design
   goal, and the one to write first.
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