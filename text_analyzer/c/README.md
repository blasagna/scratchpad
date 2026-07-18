# text_analyzer (C)

A small command-line tool that reads text and reports line, blank line, word,
character, digit, and punctuation counts, the word length distribution, and the
top-N most frequent words and non-space characters. Output can be formatted as
text or JSON.

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
`-h/--help`.

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

2. **Signed-overflow UB in growth math** (`word_table_add`). `int new_cap =
   t->capacity * 2;` overflows (signed → UB) if the table ever exceeds
   `INT_MAX/2` entries. Related: `word_table_init`'s `t->capacity *
   sizeof(WordFreq)` is fine on 64-bit, but a user passing a huge
   `--word-table-cap` makes the first doubling overflow. Practically
   unreachable given `WordFreq` is 264 bytes, but since `--word-table-cap` is
   user-controlled a guard is warranted (cap growth, or use `size_t`).

3. **Nondeterministic ordering on count ties** (`cmp_word_freq_desc`). The
   comparator returns 0 for equal counts and `qsort` isn't stable, so
   equal-frequency words come out in an arbitrary, run-dependent order. The
   tests only assert on distinct counts, so they pass, but output isn't
   reproducible. Consider a tiebreaker (`strcmp` on the word) for stable
   output. Top-chars, by contrast, *is* deterministic (inner scan is ascending
   ASCII).

### Design notes (not bugs)

4. **`word_table_add` is O(n) per word → O(n²) total.** Fine for a learning
   exercise / small files, but the linear scan is the scaling bottleneck. A
   hash table is the natural next step.

5. **Lines without a trailing newline count as one fewer line.**
   `"hello world"` → `line_count == 0` (documented by the tests). Reasonable
   choice, but differs from `wc -l`-style tools that count a final
   unterminated line. Confirm it's the intended behavior.

6. **The top-chars algorithm is correct but roundabout.** It sorts all 256
   counts, then for each rank rescans the 94 printable slots for a matching
   value, using `-1` sentinels to dedupe. It works (including ties and
   skipping non-printables), but it's O(256·94) and a bit subtle. Sorting an
   index array over just the printable range, or a small partial selection
   over `char_counts[33..126]`, would be simpler to reason about.

### Minor

- After a successful `analyze_file`, `main` could also check `ferror(f)`; moot
  once item 1 is in place.
- `CharFreq.ch` is `char` — fine because only `'!'..'~'` are stored, but if the
  char set ever widened to bytes ≥ 128 this would go negative and break
  `print_json_char`'s comparisons. The current range constraint keeps it safe;
  worth a one-line comment on the struct.

Nothing here is a crash or memory bug — allocation failure paths all free
correctly. Item 1 (read-error detection) is the one to fix; the rest are
polish.
