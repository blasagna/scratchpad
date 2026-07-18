#include "analyzer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Printable ASCII range excluding space: '!' (33) through '~' (126).
 * These are the only characters considered for the top-chars report.
 */
static const int PRINTABLE_ASCII_MIN = '!';
static const int PRINTABLE_ASCII_MAX = '~';

/* Width of the label column in the text summary, sized for the longest label. */
#define LABEL_WIDTH 13

static int word_table_init(WordTable *t, int capacity) {
  t->capacity = capacity;
  t->size = 0;
  t->entries = malloc(t->capacity * sizeof(WordFreq));
  return t->entries ? 0 : -1;
}

static void word_table_free(WordTable *t) {
  free(t->entries);
  t->entries = NULL;
  t->size = 0;
  t->capacity = 0;
}

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
  /* Break ties ascending by word so the ranking is deterministic (qsort is not
   * stable) and matches the C++ and Rust ports. */
  return strcmp(wa->word, wb->word);
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

int analyzer_init(Analyzer *a, const AnalyzerConfig *config) {
  if (!a)
    return -1;

  memset(a, 0, sizeof(*a));

  a->config = config ? *config : analyzer_config_default();
  if (a->config.max_word_len <= 0 || a->config.max_word_len > MAX_WORD_BUF)
    a->config.max_word_len = MAX_WORD_BUF;
  if (a->config.top_n < 0)
    a->config.top_n = 5;
  if (a->config.word_table_init_cap <= 0)
    a->config.word_table_init_cap = 64;

  return word_table_init(&a->words, a->config.word_table_init_cap);
}

void analyzer_free(Analyzer *a) {
  if (!a)
    return;
  word_table_free(&a->words);
}

/* Records the accumulated word and its length, then resets word state. */
static int flush_word(Analyzer *a) {
  a->word_buf[a->word_len] = '\0';
  if (word_table_add(&a->words, a->word_buf) != 0)
    return -1;
  a->stats.word_count++;

  long len = a->cur_word_len;
  a->length_sum += len;
  if (a->length_max == 0 || len > a->length_max)
    a->length_max = len;
  if (a->length_min == 0 || len < a->length_min)
    a->length_min = len;
  long bucket = len < LENGTH_HIST_BUCKETS ? len : LENGTH_HIST_BUCKETS - 1;
  a->length_hist[bucket]++;

  a->word_len = 0;
  a->cur_word_len = 0;
  a->in_word = 0;
  return 0;
}

int analyzer_feed(Analyzer *a, FILE *f) {
  if (!a || !f)
    return -1;

  int c;
  while ((c = fgetc(f)) != EOF) {
    a->char_counts[(unsigned char)c]++;
    a->stats.char_count++;

    if (c == '\n') {
      a->stats.line_count++;
      if (!a->line_has_content)
        a->stats.blank_line_count++;
      a->line_has_content = 0;
    } else if (!isspace(c)) {
      a->line_has_content = 1;
    }

    if (isalpha(c)) {
      /* Keep at most max_word_len - 1 chars, but measure the true length. */
      if (a->word_len < a->config.max_word_len - 1)
        a->word_buf[a->word_len++] = (char)tolower(c);
      a->cur_word_len++;
      a->in_word = 1;
    } else if (a->in_word) {
      if (flush_word(a) != 0)
        return -1;
    }
  }

  return ferror(f) ? -1 : 0;
}

/*
 * Returns the length at the pct-th percentile by nearest rank, or 0 when there
 * are no words. Integer arithmetic throughout so the three ports agree exactly.
 */
static long quantile(const long *hist, long count, long pct) {
  if (count <= 0)
    return 0;
  /* 1-based rank of the target element: ceil(pct/100 * count), at least 1. */
  long rank = (pct * count + 99) / 100;
  if (rank < 1)
    rank = 1;
  long cumulative = 0;
  for (long len = 0; len < LENGTH_HIST_BUCKETS; len++) {
    cumulative += hist[len];
    if (cumulative >= rank)
      return len;
  }
  return 0;
}

int analyzer_finish(Analyzer *a, TextStats *out) {
  if (!a || !out)
    return -1;

  if (a->in_word && flush_word(a) != 0)
    return -1;

  for (int i = 0; i < CHAR_TABLE_SIZE; i++) {
    if (isdigit(i))
      a->stats.digit_count += a->char_counts[i];
    else if (ispunct(i))
      a->stats.punct_count += a->char_counts[i];
  }

  a->stats.word_length.count = a->stats.word_count;
  a->stats.word_length.sum = a->length_sum;
  a->stats.word_length.min = a->length_min;
  a->stats.word_length.max = a->length_max;
  a->stats.word_length.p25 = quantile(a->length_hist, a->stats.word_count, 25);
  a->stats.word_length.p50 = quantile(a->length_hist, a->stats.word_count, 50);
  a->stats.word_length.p75 = quantile(a->length_hist, a->stats.word_count, 75);

  *out = a->stats;
  out->top_words = NULL;
  out->top_word_count = 0;
  out->top_chars = NULL;
  out->top_char_count = 0;

  qsort(a->words.entries, a->words.size, sizeof(WordFreq), cmp_word_freq_desc);
  out->top_word_count =
      a->words.size < a->config.top_n ? a->words.size : a->config.top_n;
  if (out->top_word_count > 0) {
    out->top_words = malloc(out->top_word_count * sizeof(WordFreq));
    if (!out->top_words) {
      out->top_word_count = 0;
      return -1;
    }
    for (int i = 0; i < out->top_word_count; i++)
      out->top_words[i] = a->words.entries[i];
  }

  /* Rank characters by walking the sorted counts and, for each, finding the
   * first printable character still holding that count. Consumed entries are
   * marked so duplicates are not reported twice. */
  long char_counts[CHAR_TABLE_SIZE];
  memcpy(char_counts, a->char_counts, sizeof(char_counts));
  long sorted_char_counts[CHAR_TABLE_SIZE];
  memcpy(sorted_char_counts, char_counts, sizeof(char_counts));
  qsort(sorted_char_counts, CHAR_TABLE_SIZE, sizeof(long), cmp_long_desc);

  if (a->config.top_n > 0) {
    out->top_chars = malloc(a->config.top_n * sizeof(CharFreq));
    if (!out->top_chars) {
      free(out->top_words);
      out->top_words = NULL;
      out->top_word_count = 0;
      return -1;
    }
  }

  for (int rank = 0;
       rank < CHAR_TABLE_SIZE && out->top_char_count < a->config.top_n;
       rank++) {
    long target = sorted_char_counts[rank];
    if (target == 0)
      break;
    for (int i = PRINTABLE_ASCII_MIN; i <= PRINTABLE_ASCII_MAX; i++) {
      if (char_counts[i] == target) {
        out->top_chars[out->top_char_count].ch = (char)i;
        out->top_chars[out->top_char_count].count = target;
        out->top_char_count++;
        char_counts[i] = -1;
        break;
      }
    }
  }

  return 0;
}

int analyze_file(FILE *f, const AnalyzerConfig *config, TextStats *out) {
  if (!f || !out)
    return -1;

  Analyzer a;
  if (analyzer_init(&a, config) != 0)
    return -1;

  int rc = analyzer_feed(&a, f);
  if (rc == 0)
    rc = analyzer_finish(&a, out);

  analyzer_free(&a);
  return rc;
}

/* Returns the mean word length, or 0.0 when no words were seen. */
static double word_length_mean(const WordLengthStats *wl) {
  return wl->count > 0 ? (double)wl->sum / wl->count : 0.0;
}

void print_stats(const TextStats *stats) {
  printf("%-*s %ld\n", LABEL_WIDTH, "Lines:", stats->line_count);
  printf("%-*s %ld\n", LABEL_WIDTH, "Blank lines:", stats->blank_line_count);
  printf("%-*s %ld\n", LABEL_WIDTH, "Words:", stats->word_count);
  printf("%-*s %ld\n", LABEL_WIDTH, "Characters:", stats->char_count);
  printf("%-*s %ld\n", LABEL_WIDTH, "Digits:", stats->digit_count);
  printf("%-*s %ld\n", LABEL_WIDTH, "Punctuation:", stats->punct_count);

  const WordLengthStats *wl = &stats->word_length;
  printf("\n%-*s mean %.1f, min %ld, max %ld, p25 %ld, p50 %ld, p75 %ld\n",
         LABEL_WIDTH, "Word length:", word_length_mean(wl), wl->min, wl->max,
         wl->p25, wl->p50, wl->p75);

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
  const WordLengthStats *wl = &stats->word_length;

  printf("{\n");
  printf("  \"lines\": %ld,\n", stats->line_count);
  printf("  \"blank_lines\": %ld,\n", stats->blank_line_count);
  printf("  \"words\": %ld,\n", stats->word_count);
  printf("  \"characters\": %ld,\n", stats->char_count);
  printf("  \"digits\": %ld,\n", stats->digit_count);
  printf("  \"punctuation\": %ld,\n", stats->punct_count);

  printf("  \"word_length\": {\n");
  printf("    \"mean\": %.4f,\n", word_length_mean(wl));
  printf("    \"min\": %ld,\n", wl->min);
  printf("    \"max\": %ld,\n", wl->max);
  printf("    \"p25\": %ld,\n", wl->p25);
  printf("    \"p50\": %ld,\n", wl->p50);
  printf("    \"p75\": %ld\n", wl->p75);
  printf("  },\n");

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
