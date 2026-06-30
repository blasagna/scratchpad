#ifndef TEXT_ANALYZER_ANALYZER_H
#define TEXT_ANALYZER_ANALYZER_H

#include <stdio.h>

/*
 * Compile-time upper bound for word storage. Must be a #define (not a const
 * int) because C requires an integer constant expression for struct member
 * array sizes.
 */
#define MAX_WORD_BUF 256

typedef struct {
  char word[MAX_WORD_BUF];
  long count;
} WordFreq;

typedef struct {
  char ch;
  long count;
} CharFreq;

/*
 * Runtime configuration for analyze_file. Initialize with
 * analyzer_config_default() then override any field before calling
 * analyze_file.
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

typedef struct {
  long line_count;
  long word_count;
  long char_count;
  WordFreq
      *top_words; /* top_word_count entries, heap-allocated by analyze_file */
  int top_word_count;
  CharFreq
      *top_chars; /* top_char_count entries, heap-allocated by analyze_file */
  int top_char_count;
} TextStats;

/* Frees heap memory allocated inside stats by analyze_file. */
void text_stats_free(TextStats *stats);

/*
 * analyze_file - reads all bytes from f and populates out with statistics.
 *
 * Input:  f      - open, readable FILE* positioned at the start of the content.
 *                  The caller retains ownership and must fclose() it afterward.
 *         config - runtime options; pass NULL to use sane defaults.
 *         out    - pointer to a TextStats struct that will be fully
 * overwritten. Call text_stats_free(out) when done.
 *
 * Output: Returns 0 on success. Returns -1 if f or out is NULL.
 *         On success, out->top_words and out->top_chars are heap-allocated
 *         arrays of up to config->top_n entries, sorted descending by count.
 */
int analyze_file(FILE *f, const AnalyzerConfig *config, TextStats *out);

/*
 * print_stats - prints a formatted summary of stats to stdout.
 *
 * Input:  stats - pointer to a populated TextStats struct (e.g. from
 *                 analyze_file). Must not be NULL.
 *
 * Output: Writes lines/words/characters totals followed by ranked lists of
 *         the top words and top characters. Returns nothing.
 */
void print_stats(const TextStats *stats);

/*
 * print_stats_json - prints stats to stdout as a single JSON object.
 *
 * Input:  stats - pointer to a populated TextStats struct (e.g. from
 *                 analyze_file). Must not be NULL.
 *
 * Output: Writes one JSON object with line/word/character totals plus
 *         top_words and top_characters arrays. Each entry carries its count
 *         and a frequency expressed as a ratio in [0, 1]. Returns nothing.
 */
void print_stats_json(const TextStats *stats);

#endif
