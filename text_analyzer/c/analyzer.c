#include "analyzer.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* All possible byte values (0-255), used to size the character frequency table.
 */
#define CHAR_TABLE_SIZE (UCHAR_MAX + 1)

/*
 * Printable ASCII range excluding space: '!' (33) through '~' (126).
 * These are the only characters considered for the top-chars report.
 */
static const int PRINTABLE_ASCII_MIN = '!';
static const int PRINTABLE_ASCII_MAX = '~';

typedef struct {
  WordFreq *entries;
  int size;
  int capacity;
} WordTable;

static int word_table_init(WordTable *t, int capacity) {
  t->capacity = capacity;
  t->size = 0;
  t->entries = malloc(t->capacity * sizeof(WordFreq));
  return t->entries ? 0 : -1;
}

static void word_table_free(WordTable *t) { free(t->entries); }

static int word_table_add(WordTable *t, const char *word) {
  for (int i = 0; i < t->size; i++) {
    if (strcmp(t->entries[i].word, word) == 0) {
      t->entries[i].count++;
      return 0;
    }
  }
  if (t->size == t->capacity) {
    int new_cap = t->capacity * 2;
    WordFreq *tmp = realloc(t->entries, new_cap * sizeof(WordFreq));
    if (!tmp)
      return -1;
    t->entries = tmp;
    t->capacity = new_cap;
  }
  strncpy(t->entries[t->size].word, word, MAX_WORD_BUF - 1);
  t->entries[t->size].word[MAX_WORD_BUF - 1] = '\0';
  t->entries[t->size].count = 1;
  t->size++;
  return 0;
}

static int cmp_word_freq_desc(const void *a, const void *b) {
  const WordFreq *wa = (const WordFreq *)a;
  const WordFreq *wb = (const WordFreq *)b;
  if (wb->count > wa->count)
    return 1;
  if (wb->count < wa->count)
    return -1;
  return 0;
}

static int cmp_long_desc(const void *a, const void *b) {
  long va = *(const long *)a;
  long vb = *(const long *)b;
  if (vb > va)
    return 1;
  if (vb < va)
    return -1;
  return 0;
}

AnalyzerConfig analyzer_config_default(void) {
  AnalyzerConfig cfg;
  cfg.max_word_len = MAX_WORD_BUF;
  cfg.top_n = 5;
  cfg.word_table_init_cap = 64;
  return cfg;
}

void text_stats_free(TextStats *stats) {
  if (!stats)
    return;
  free(stats->top_words);
  free(stats->top_chars);
  stats->top_words = NULL;
  stats->top_chars = NULL;
}

int analyze_file(FILE *f, const AnalyzerConfig *config, TextStats *out) {
  if (!f || !out)
    return -1;

  AnalyzerConfig cfg = config ? *config : analyzer_config_default();
  if (cfg.max_word_len <= 0 || cfg.max_word_len > MAX_WORD_BUF)
    cfg.max_word_len = MAX_WORD_BUF;
  if (cfg.top_n <= 0)
    cfg.top_n = 5;
  if (cfg.word_table_init_cap <= 0)
    cfg.word_table_init_cap = 64;

  memset(out, 0, sizeof(*out));

  long char_counts[CHAR_TABLE_SIZE] = {0};
  WordTable words;
  if (word_table_init(&words, cfg.word_table_init_cap) != 0)
    return -1;

  char word_buf[MAX_WORD_BUF];
  int word_len = 0;
  int in_word = 0;
  int err = 0;

  int c;
  while ((c = fgetc(f)) != EOF) {
    char_counts[(unsigned char)c]++;
    out->char_count++;

    if (c == '\n') {
      out->line_count++;
    }

    if (isalpha(c)) {
      if (word_len < cfg.max_word_len - 1) {
        word_buf[word_len++] = (char)tolower(c);
      }
      in_word = 1;
    } else {
      if (in_word) {
        word_buf[word_len] = '\0';
        if (word_table_add(&words, word_buf) != 0) {
          err = 1;
          break;
        }
        out->word_count++;
        word_len = 0;
        in_word = 0;
      }
    }
  }
  if (!err && in_word) {
    word_buf[word_len] = '\0';
    if (word_table_add(&words, word_buf) != 0)
      err = 1;
    else
      out->word_count++;
  }
  if (err) {
    word_table_free(&words);
    return -1;
  }

  qsort(words.entries, words.size, sizeof(WordFreq), cmp_word_freq_desc);
  out->top_word_count = words.size < cfg.top_n ? words.size : cfg.top_n;
  if (out->top_word_count > 0) {
    out->top_words = malloc(out->top_word_count * sizeof(WordFreq));
    if (!out->top_words) {
      word_table_free(&words);
      return -1;
    }
    for (int i = 0; i < out->top_word_count; i++) {
      out->top_words[i] = words.entries[i];
    }
  }

  long sorted_char_counts[CHAR_TABLE_SIZE];
  memcpy(sorted_char_counts, char_counts, sizeof(char_counts));
  qsort(sorted_char_counts, CHAR_TABLE_SIZE, sizeof(long), cmp_long_desc);

  CharFreq *top_chars_buf = NULL;
  if (cfg.top_n > 0) {
    top_chars_buf = malloc(cfg.top_n * sizeof(CharFreq));
    if (!top_chars_buf) {
      free(out->top_words);
      out->top_words = NULL;
      word_table_free(&words);
      return -1;
    }
  }
  out->top_char_count = 0;

  for (int rank = 0; rank < CHAR_TABLE_SIZE && out->top_char_count < cfg.top_n;
       rank++) {
    long target = sorted_char_counts[rank];
    if (target == 0)
      break;
    for (int i = PRINTABLE_ASCII_MIN; i <= PRINTABLE_ASCII_MAX; i++) {
      if (char_counts[i] == target) {
        top_chars_buf[out->top_char_count].ch = (char)i;
        top_chars_buf[out->top_char_count].count = target;
        out->top_char_count++;
        char_counts[i] = -1;
        break;
      }
    }
  }
  out->top_chars = top_chars_buf;

  word_table_free(&words);
  return 0;
}

void print_stats(const TextStats *stats) {
  printf("Lines:      %ld\n", stats->line_count);
  printf("Words:      %ld\n", stats->word_count);
  printf("Characters: %ld\n", stats->char_count);

  printf("\nTop words:\n");
  for (int i = 0; i < stats->top_word_count; i++) {
    double pct = stats->word_count > 0
                     ? 100.0 * stats->top_words[i].count / stats->word_count
                     : 0.0;
    printf("  %d. %s (%ld, %.1f%%)\n", i + 1, stats->top_words[i].word,
           stats->top_words[i].count, pct);
  }

  printf("\nTop characters:\n");
  for (int i = 0; i < stats->top_char_count; i++) {
    double pct = stats->char_count > 0
                     ? 100.0 * stats->top_chars[i].count / stats->char_count
                     : 0.0;
    printf("  %d. '%c' (%ld, %.1f%%)\n", i + 1, stats->top_chars[i].ch,
           stats->top_chars[i].count, pct);
  }
}

/* Prints c as the contents of a JSON string, escaping the two characters that
 * are otherwise illegal inside one. The char range used for top characters
 * ('!'..'~') contains no control characters, so only '"' and '\\' can appear.
 */
static void print_json_char(char c) {
  if (c == '"' || c == '\\') {
    printf("\\%c", c);
  } else {
    printf("%c", c);
  }
}

void print_stats_json(const TextStats *stats) {
  printf("{\n");
  printf("  \"lines\": %ld,\n", stats->line_count);
  printf("  \"words\": %ld,\n", stats->word_count);
  printf("  \"characters\": %ld,\n", stats->char_count);

  printf("  \"top_words\": [");
  for (int i = 0; i < stats->top_word_count; i++) {
    double freq = stats->word_count > 0
                      ? (double)stats->top_words[i].count / stats->word_count
                      : 0.0;
    printf(
        "%s\n    {\"word\": \"%s\", \"count\": %ld, \"frequency\": %.4f}",
        i == 0 ? "" : ",",
        stats->top_words[i].word, /* words are alpha-only; no escaping needed */
        stats->top_words[i].count, freq);
  }
  printf("%s],\n", stats->top_word_count > 0 ? "\n  " : "");

  printf("  \"top_characters\": [");
  for (int i = 0; i < stats->top_char_count; i++) {
    double freq = stats->char_count > 0
                      ? (double)stats->top_chars[i].count / stats->char_count
                      : 0.0;
    printf("%s\n    {\"char\": \"", i == 0 ? "" : ",");
    print_json_char(stats->top_chars[i].ch);
    printf("\", \"count\": %ld, \"frequency\": %.4f}",
           stats->top_chars[i].count, freq);
  }
  printf("%s]\n", stats->top_char_count > 0 ? "\n  " : "");

  printf("}\n");
}
