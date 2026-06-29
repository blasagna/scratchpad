#ifndef TEXT_ANALYZER_ANALYZER_H
#define TEXT_ANALYZER_ANALYZER_H

#include <stdio.h>

#define MAX_WORD_LEN 256
#define TOP_N 5

typedef struct {
    char word[MAX_WORD_LEN];
    long count;
} WordFreq;

typedef struct {
    char ch;
    long count;
} CharFreq;

typedef struct {
    long line_count;
    long word_count;
    long char_count;
    WordFreq top_words[TOP_N];
    int top_word_count;
    CharFreq top_chars[TOP_N];
    int top_char_count;
} TextStats;

/*
 * analyze_file - reads all bytes from f and populates out with statistics.
 *
 * Input:  f   - open, readable FILE* positioned at the start of the content.
 *               The caller retains ownership and must fclose() it afterward.
 *         out - pointer to a TextStats struct that will be fully overwritten.
 *
 * Output: Returns 0 on success. Returns -1 if either argument is NULL.
 *         On success, out->line_count, word_count, and char_count reflect the
 *         totals for the entire file, and top_words / top_chars hold the TOP_N
 *         most frequent words (case-insensitive) and non-space printable ASCII
 *         characters, sorted descending by count.
 */
int analyze_file(FILE *f, TextStats *out);

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

#endif
