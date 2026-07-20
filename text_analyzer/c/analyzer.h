#ifndef TEXT_ANALYZER_ANALYZER_H
#define TEXT_ANALYZER_ANALYZER_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Compile-time upper bound for word storage. Must be a #define (not a const
 * int) because C requires an integer constant expression for struct member
 * array sizes.
 */
#define MAX_WORD_BUF 256

/*
 * Text handling: input is processed as bytes, in the "C" locale (no port calls
 * setlocale), so isalpha/isdigit/ispunct classification is ASCII-only. That is
 * what keeps this port byte-for-byte identical to the C++ and Rust ones.
 *
 * Consequences: char_count counts bytes, not Unicode codepoints, and a word is
 * a maximal run of ASCII letters [A-Za-z]. Any non-ASCII byte counts as a
 * character, separates words, and is counted as neither a digit nor a
 * punctuation mark. So "cafe" spelled with U+00E9 is one word ("caf") and five
 * characters.
 */

/* All possible byte values (0-255), used to size the character frequency
 * table. */
#define CHAR_TABLE_SIZE (UCHAR_MAX + 1)

/*
 * Buckets in the word length histogram, indexed by length. Lengths at or above
 * the last index are clamped into it, so quantiles (but never the exactly
 * tracked count/sum/min/max) lose resolution for words longer than MAX_WORD_BUF
 * characters.
 */
#define LENGTH_HIST_BUCKETS (MAX_WORD_BUF + 1)

typedef struct {
  char word[MAX_WORD_BUF];
  long count;
} WordFreq;

typedef struct {
  char ch;
  long count;
} CharFreq;

/*
 * Runtime configuration for the analyzer. Initialize with
 * analyzer_config_default() then override any field before use.
 */
typedef struct {
  int max_word_len; /* chars kept per word (must be <= MAX_WORD_BUF); default
                       256 */
  int top_n;        /* number of top words/chars to report; default 5 */
  int word_table_init_cap; /* initial capacity of the word frequency table;
                              default 64 */
} AnalyzerConfig;

/* Returns an AnalyzerConfig populated with sane defaults. */
AnalyzerConfig analyzer_config_default(void);

/*
 * Distribution of word lengths, measured in alphabetic characters.
 *
 * Lengths are the true lengths of each word in the input, unaffected by
 * max_word_len truncation of the stored spelling. The mean is not stored:
 * derive it as sum / count, guarding against count == 0. All fields are zero
 * when no words were seen.
 */
typedef struct {
  long count; /* number of words measured; equals TextStats.word_count */
  long sum;   /* total of all word lengths */
  long min;
  long max;
  long p25; /* 25th, 50th, and 75th percentile lengths by nearest rank */
  long p50;
  long p75;
} WordLengthStats;

typedef struct {
  long line_count;
  long blank_line_count; /* lines containing no non-whitespace characters */
  long word_count;
  long char_count;
  long digit_count;
  long punct_count;
  WordLengthStats word_length;
  WordFreq
      *top_words; /* top_word_count entries, heap-allocated by the analyzer */
  int top_word_count;
  CharFreq
      *top_chars; /* top_char_count entries, heap-allocated by the analyzer */
  int top_char_count;
} TextStats;

/* Frees heap memory allocated inside stats by the analyzer. */
void text_stats_free(TextStats *stats);

/*
 * One distinct word and its count. The spelling is interned into the table's
 * arena rather than stored inline, keeping entries small enough that a large
 * vocabulary stays cheap to probe and to grow.
 */
typedef struct {
  const char *word; /* interned into the arena; NULL marks an empty slot */
  long count;
} WordEntry;

/*
 * A block of interned word bytes. Blocks are bump-allocated and never moved or
 * reallocated, so pointers into them stay valid as the table grows. The layout
 * is private to analyzer.c; only the pointer type is needed here.
 */
typedef struct ArenaBlock ArenaBlock;

/*
 * Hash table of distinct words and their counts, using open addressing with
 * linear probing. Internal to the analyzer; declared here only so Analyzer can
 * be stack-allocated by callers.
 */
typedef struct {
  WordEntry *slots; /* capacity is always a power of two */
  size_t size;      /* occupied slots */
  size_t capacity;
  ArenaBlock *arena;
} WordTable;

/*
 * Accumulates statistics across one or more streams.
 *
 * Feed any number of streams with analyzer_feed, then call analyzer_finish to
 * rank and return the totals. Scan state persists between feeds, so feeding two
 * streams is equivalent to feeding their concatenation: a word or line split
 * across the boundary is counted once, not twice.
 *
 * Treat the fields as private; use the analyzer_* functions.
 */
typedef struct {
  AnalyzerConfig config;
  TextStats stats;
  long char_counts[CHAR_TABLE_SIZE];
  WordTable words;
  long length_hist[LENGTH_HIST_BUCKETS];
  long length_sum;
  long length_min;
  long length_max;
  char word_buf[MAX_WORD_BUF]; /* spelling in progress, truncated */
  int word_len;                /* bytes stored in word_buf */
  long cur_word_len;           /* true length of the word in progress */
  int in_word;
  int line_has_content;
} Analyzer;

/*
 * analyzer_init - prepares a to accumulate statistics.
 *
 * Input:  a      - pointer to an Analyzer that will be fully overwritten.
 *         config - runtime options; pass NULL to use sane defaults.
 * Out-of-range fields are replaced with their defaults.
 *
 * Output: Returns 0 on success, -1 on allocation failure or if a is NULL.
 *         On success the caller must eventually call analyzer_free(a).
 */
int analyzer_init(Analyzer *a, const AnalyzerConfig *config);

/*
 * analyzer_feed - reads all bytes from f into the accumulator.
 *
 * Input:  a - initialized Analyzer.
 *         f - open, readable FILE*. The caller retains ownership and must
 *             fclose() it afterward.
 *
 * Output: Returns 0 on success, -1 on read or allocation failure.
 */
int analyzer_feed(Analyzer *a, FILE *f);

/*
 * analyzer_finish - flushes any trailing word, then ranks and returns totals.
 *
 * Input:  a   - initialized Analyzer, already fed.
 *         out - pointer to a TextStats struct that will be fully overwritten.
 *               Call text_stats_free(out) when done.
 *
 * Output: Returns 0 on success, -1 on allocation failure or NULL argument.
 *         On success, out->top_words and out->top_chars are heap-allocated
 *         arrays of up to config.top_n entries, sorted descending by count.
 */
int analyzer_finish(Analyzer *a, TextStats *out);

/* Frees heap memory held by the analyzer. Safe to call once after
 * analyzer_init succeeded, whether or not analyzer_finish was called. */
void analyzer_free(Analyzer *a);

/*
 * analyze_file - reads all bytes from f and populates out with statistics.
 *
 * A convenience wrapper over analyzer_init, analyzer_feed, analyzer_finish, and
 * analyzer_free for the single-stream case.
 *
 * Input:  f      - open, readable FILE* positioned at the start of the content.
 *                  The caller retains ownership and must fclose() it afterward.
 *         config - runtime options; pass NULL to use sane defaults.
 *         out    - pointer to a TextStats struct that will be fully
 *                  overwritten. Call text_stats_free(out) when done.
 *
 * Output: Returns 0 on success, -1 on failure.
 */
int analyze_file(FILE *f, const AnalyzerConfig *config, TextStats *out);

/*
 * print_stats - prints a formatted summary of stats to stdout.
 *
 * Input:  stats - pointer to a populated TextStats struct. Must not be NULL.
 *
 * Output: Writes the counts and word length distribution followed by ranked
 *         lists of the top words and top characters. Returns nothing.
 */
void print_stats(const TextStats *stats);

/*
 * print_stats_json - prints stats to stdout as a single JSON object.
 *
 * Input:  stats - pointer to a populated TextStats struct. Must not be NULL.
 *
 * Output: Writes one JSON object with the counts, a word_length object, and
 *         top_words and top_characters arrays. Each ranked entry carries its
 *         count and a frequency expressed as a ratio in [0, 1]. Returns
 *         nothing.
 */
void print_stats_json(const TextStats *stats);

#endif
