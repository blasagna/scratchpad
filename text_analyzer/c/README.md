# text_analyzer (C)

A small command-line tool that reads text and reports line, blank line, word,
character, digit, and punctuation counts, the word length distribution, and the
top-N most frequent words and non-space characters. Output can be formatted as
text or JSON.

Input is treated as ASCII bytes: characters are counted as bytes rather than
Unicode codepoints, and any non-ASCII byte separates words. See the top-level
README for the full contract.

## Build & run

```sh
bazel run //text_analyzer/c:text_analyzer -- <file>
bazel run //text_analyzer/c:text_analyzer -- --top-n 10 --json <file>
bazel run //text_analyzer/c:text_analyzer -- a.txt b.txt   # analyzed as one stream
cat a.txt | bazel run //text_analyzer/c:text_analyzer      # stdin
bazel test //text_analyzer/c:test_analyzer
```

Multiple files are analyzed as a single concatenated stream. With no file
argument, or with `-` as the file, input is read from stdin.

Options: `--top-n N`, `--max-word-len N`, `--word-table-cap N`, `--json`,
`-h/--help`. `--word-table-cap` is an initial-capacity hint, rounded up to a
power of two (minimum 8); the table grows on its own as needed.

Timings must use an optimized build — the default Bazel build is `fastbuild`
(`-O0`):

```sh
bazel build --config=opt //text_analyzer/c:text_analyzer
```

## Code review notes

Overall this is well-structured, careful C: clear header docs, sane config
validation, consistent error handling with proper cleanup on every failure
path, and correct `(unsigned char)` casts before `char_counts` indexing and
`ctype` calls. The items below are roughly ordered by importance.

### Correctness / robustness

1. ~~**`analyze_file` can't distinguish EOF from a read error**~~ (fixed).
   `while ((c = fgetc(f)) != EOF)` treated a genuine stream error the same as
   end-of-file, silently returning partial stats as success. The read loop now
   lives in `analyzer_feed`, which ends with `return ferror(f) ? -1 : 0;` so
   `main.c` reports the failure instead of printing truncated numbers.

2. ~~**Signed-overflow UB in growth math**~~ (fixed). `int new_cap =
   t->capacity * 2;` overflowed (signed → UB) if the table ever exceeded
   `INT_MAX/2` entries, and a large `--word-table-cap` could overflow the first
   doubling. All table capacity math is now `size_t`, and `round_up_pow2`
   saturates rather than wrapping.

3. ~~**Nondeterministic ordering on count ties**~~ (fixed). The comparator
   returned 0 for equal counts and `qsort` isn't stable, so equal-frequency
   words came out in an arbitrary, run-dependent order. `cmp_word_entry_desc`
   now breaks ties with `strcmp`, matching the C++ and Rust ports — which
   matters more since the hash table's slot order is arbitrary. Top-chars is
   likewise deterministic: `cmp_char_freq_desc` breaks count ties ascending by
   character.

### Design notes (not bugs)

4. ~~**`word_table_add` is O(n) per word → O(n²) total.**~~ (fixed). The linear
   scan was the scaling bottleneck: on a 2.7 MB corpus it cost 11.25s at 40,000
   distinct words versus 0.030s at 50, on identical input size and word count.
   `WordTable` is now an open-addressed hash table (FNV-1a, linear probing,
   power-of-two capacity, doubling at 70% load) over words interned into a bump
   allocator. Same corpus now runs in 0.021s. See `../bench/run.sh`.

   Two details worth knowing:
   - Table entries hold a `const char *` into the arena rather than an inline
     `char[256]`, so an entry is 16 bytes instead of 264. Arena blocks are never
     reallocated, which is what makes those pointers safe across a rehash.
   - `WordFreq` — the *public* output type in `TextStats` — deliberately still
     embeds `char word[MAX_WORD_BUF]`. It holds only `top_n` entries, so it was
     never the memory problem, and leaving it alone kept the API and every test
     unchanged.

5. **Lines without a trailing newline count as one fewer line.**
   `"hello world"` → `line_count == 0` (documented by the tests). Reasonable
   choice, but differs from `wc -l`-style tools that count a final
   unterminated line. Confirm it's the intended behavior.

6. ~~**The top-chars algorithm is correct but roundabout.**~~ (fixed). It used
   to sort all 256 counts, then for each rank rescan the 94 printable slots for
   a matching value, using `-1` sentinels to dedupe — O(256·94) and a bit
   subtle. `analyzer_finish` now collects only the printable characters that
   actually occurred into a small `CharFreq` array and `qsort`s that with
   `cmp_char_freq_desc`, which is both simpler and cheaper.

### Minor

- `CharFreq.ch` is `char` — fine because only `'!'..'~'` are stored, but if the
  char set ever widened to bytes ≥ 128 this would go negative and break
  `print_json_char`'s comparisons. The current range constraint keeps it safe;
  worth a one-line comment on the struct.

Nothing here is a crash or memory bug — allocation failure paths all free
correctly. The correctness and performance items (1–4, 6) have since been
fixed; what remains (item 5, and the minor notes below) is polish and a
behavior confirmation.
