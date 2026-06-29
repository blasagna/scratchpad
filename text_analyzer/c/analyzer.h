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

int analyze_file(FILE *f, TextStats *out);
void print_stats(const TextStats *stats);

#endif
