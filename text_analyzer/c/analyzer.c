#include "analyzer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    WordFreq *entries;
    int size;
    int capacity;
} WordTable;

static void word_table_init(WordTable *t) {
    t->capacity = 64;
    t->size = 0;
    t->entries = malloc(t->capacity * sizeof(WordFreq));
}

static void word_table_free(WordTable *t) {
    free(t->entries);
}

static void word_table_add(WordTable *t, const char *word) {
    for (int i = 0; i < t->size; i++) {
        if (strcmp(t->entries[i].word, word) == 0) {
            t->entries[i].count++;
            return;
        }
    }
    if (t->size == t->capacity) {
        t->capacity *= 2;
        t->entries = realloc(t->entries, t->capacity * sizeof(WordFreq));
    }
    strncpy(t->entries[t->size].word, word, MAX_WORD_LEN - 1);
    t->entries[t->size].word[MAX_WORD_LEN - 1] = '\0';
    t->entries[t->size].count = 1;
    t->size++;
}

static int cmp_word_entry_desc(const void *a, const void *b) {
    const WordFreq *wa = (const WordFreq *)a;
    const WordFreq *wb = (const WordFreq *)b;
    if (wb->count > wa->count) return 1;
    if (wb->count < wa->count) return -1;
    return 0;
}

static int cmp_long_desc(const void *a, const void *b) {
    long va = *(const long *)a;
    long vb = *(const long *)b;
    if (vb > va) return 1;
    if (vb < va) return -1;
    return 0;
}

int analyze_file(FILE *f, TextStats *out) {
    if (!f || !out) return -1;

    memset(out, 0, sizeof(*out));

    long char_counts[256] = {0};
    WordTable words;
    word_table_init(&words);

    char word_buf[MAX_WORD_LEN];
    int word_len = 0;
    int in_word = 0;

    int c;
    while ((c = fgetc(f)) != EOF) {
        char_counts[(unsigned char)c]++;
        out->char_count++;

        if (c == '\n') {
            out->line_count++;
        }

        if (isalpha(c)) {
            if (word_len < MAX_WORD_LEN - 1) {
                word_buf[word_len++] = (char)tolower(c);
            }
            in_word = 1;
        } else {
            if (in_word) {
                word_buf[word_len] = '\0';
                word_table_add(&words, word_buf);
                out->word_count++;
                word_len = 0;
                in_word = 0;
            }
        }
    }
    if (in_word) {
        word_buf[word_len] = '\0';
        word_table_add(&words, word_buf);
        out->word_count++;
    }

    qsort(words.entries, words.size, sizeof(WordFreq), cmp_word_entry_desc);
    out->top_word_count = words.size < TOP_N ? words.size : TOP_N;
    for (int i = 0; i < out->top_word_count; i++) {
        out->top_words[i] = words.entries[i];
    }

    long sorted_char_counts[256];
    memcpy(sorted_char_counts, char_counts, sizeof(char_counts));
    qsort(sorted_char_counts, 256, sizeof(long), cmp_long_desc);

    out->top_char_count = 0;
    for (int rank = 0; rank < TOP_N && rank < 256; rank++) {
        long target = sorted_char_counts[rank];
        if (target == 0) break;
        for (int i = 33; i < 127; i++) {
            if (char_counts[i] == target) {
                out->top_chars[out->top_char_count].ch = (char)i;
                out->top_chars[out->top_char_count].count = target;
                out->top_char_count++;
                char_counts[i] = -1;
                break;
            }
        }
        if (out->top_char_count >= TOP_N) break;
    }

    word_table_free(&words);
    return 0;
}

void print_stats(const TextStats *stats) {
    printf("Lines:      %ld\n", stats->line_count);
    printf("Words:      %ld\n", stats->word_count);
    printf("Characters: %ld\n", stats->char_count);

    printf("\nTop words:\n");
    for (int i = 0; i < stats->top_word_count; i++) {
        printf("  %d. %s (%ld)\n", i + 1,
               stats->top_words[i].word,
               stats->top_words[i].count);
    }

    printf("\nTop characters:\n");
    for (int i = 0; i < stats->top_char_count; i++) {
        printf("  %d. '%c' (%ld)\n", i + 1,
               stats->top_chars[i].ch,
               stats->top_chars[i].count);
    }
}
