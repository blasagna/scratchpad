#include "analyzer.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Printable ASCII range excluding space: '!' (33) through '~' (126).
 * These are the only characters considered for the top-chars report.
 */
static const int PRINTABLE_ASCII_MIN = '!';
static const int PRINTABLE_ASCII_MAX = '~';

/* Width of the label column in the text summary, sized for the longest label.
 */
#define LABEL_WIDTH 13

/* Bytes read per fread() call. Reading in blocks rather than one getc() at a
 * time keeps the scan loop off the stdio locking path. */
#define READ_BUF_SIZE (64 * 1024)

/* Payload bytes in a normal arena block. Words too long to fit get their own
 * oversized block. */
#define ARENA_BLOCK_SIZE (64 * 1024)

/* Smallest permitted table capacity, and the load factor (as a percentage) at
 * which the table doubles. Open addressing degrades badly as it fills, so this
 * stays well below 100. */
#define MIN_TABLE_CAPACITY 8
#define MAX_LOAD_PERCENT 70

/*
 * A block of interned word bytes, bump-allocated and never moved. Declared
 * opaquely in analyzer.h; the flexible array member keeps the header and its
 * bytes in a single allocation.
 */
struct ArenaBlock {
  struct ArenaBlock *next;
  size_t used;
  size_t cap;
  char data[];
};

/*
 * Copies len bytes of word plus a terminator into the arena and returns a
 * pointer to them, or NULL on allocation failure. Blocks are never moved, so
 * the returned pointer stays valid for the lifetime of the table.
 */
static const char *arena_intern(WordTable *t, const char *word, size_t len) {
  const size_t need = len + 1;

  if (!t->arena || t->arena->cap - t->arena->used < need) {
    const size_t cap = need > ARENA_BLOCK_SIZE ? need : ARENA_BLOCK_SIZE;
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + cap);
    if (!block)
      return NULL;
    block->next = t->arena;
    block->used = 0;
    block->cap = cap;
    t->arena = block;
  }

  char *dst = t->arena->data + t->arena->used;
  memcpy(dst, word, len);
  dst[len] = '\0';
  t->arena->used += need;
  return dst;
}

static void arena_free(ArenaBlock *block) {
  while (block) {
    ArenaBlock *next = block->next;
    free(block);
    block = next;
  }
}

/* FNV-1a, 64-bit. Cheap, no setup, and well behaved on short ASCII keys. */
static unsigned long long hash_word(const char *word, size_t len) {
  unsigned long long h = 14695981039346656037ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)word[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* Rounds capacity up to a power of two so the hash can be masked rather than
 * divided. */
static size_t round_up_pow2(size_t n) {
  size_t cap = MIN_TABLE_CAPACITY;
  while (cap < n) {
    /* Saturate rather than wrap if a caller asks for something absurd. */
    if (cap > SIZE_MAX / 2)
      return cap;
    cap *= 2;
  }
  return cap;
}

static int word_table_init(WordTable *t, int capacity) {
  t->capacity = round_up_pow2(capacity > 0 ? (size_t)capacity : 0);
  t->size = 0;
  t->arena = NULL;
  t->slots = calloc(t->capacity, sizeof(WordEntry));
  return t->slots ? 0 : -1;
}

static void word_table_free(WordTable *t) {
  free(t->slots);
  arena_free(t->arena);
  t->slots = NULL;
  t->arena = NULL;
  t->size = 0;
  t->capacity = 0;
}

/*
 * Returns the slot holding word, or the empty slot where it belongs. The table
 * is never full when this is called, so the probe always terminates.
 */
static WordEntry *word_table_lookup(WordEntry *slots, size_t capacity,
                                    const char *word, unsigned long long hash) {
  size_t i = (size_t)hash & (capacity - 1);
  while (slots[i].word && strcmp(slots[i].word, word) != 0) {
    i = (i + 1) & (capacity - 1);
  }
  return &slots[i];
}

/* Doubles the table and reinserts every entry. Interned pointers are reused
 * as-is, so no string bytes are copied. */
static int word_table_grow(WordTable *t) {
  const size_t new_cap = t->capacity * 2;
  WordEntry *new_slots = calloc(new_cap, sizeof(WordEntry));
  if (!new_slots)
    return -1;

  for (size_t i = 0; i < t->capacity; i++) {
    const WordEntry *e = &t->slots[i];
    if (!e->word)
      continue;
    const size_t len = strlen(e->word);
    WordEntry *dst =
        word_table_lookup(new_slots, new_cap, e->word, hash_word(e->word, len));
    *dst = *e;
  }

  free(t->slots);
  t->slots = new_slots;
  t->capacity = new_cap;
  return 0;
}

static int word_table_add(WordTable *t, const char *word, size_t len) {
  WordEntry *slot =
      word_table_lookup(t->slots, t->capacity, word, hash_word(word, len));
  if (slot->word) {
    slot->count++;
    return 0;
  }

  const char *interned = arena_intern(t, word, len);
  if (!interned)
    return -1;
  slot->word = interned;
  slot->count = 1;
  t->size++;

  if (t->size * 100 >= t->capacity * MAX_LOAD_PERCENT)
    return word_table_grow(t);
  return 0;
}

static int cmp_word_entry_desc(const void *a, const void *b) {
  const WordEntry *wa = (const WordEntry *)a;
  const WordEntry *wb = (const WordEntry *)b;
  if (wb->count > wa->count)
    return 1;
  if (wb->count < wa->count)
    return -1;
  /* Break ties ascending by word so the ranking is deterministic (qsort is not
   * stable) and matches the C++ and Rust ports. */
  return strcmp(wa->word, wb->word);
}

static int cmp_char_freq_desc(const void *a, const void *b) {
  const CharFreq *ca = (const CharFreq *)a;
  const CharFreq *cb = (const CharFreq *)b;
  if (cb->count > ca->count)
    return 1;
  if (cb->count < ca->count)
    return -1;
  /* Break ties ascending by character, matching the C++ and Rust ports. */
  return (unsigned char)ca->ch - (unsigned char)cb->ch;
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
  if (word_table_add(&a->words, a->word_buf, (size_t)a->word_len) != 0)
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

  char buf[READ_BUF_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    for (size_t i = 0; i < n; i++) {
      const int c = (unsigned char)buf[i];
      a->char_counts[c]++;
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

  /* Collect the occupied slots, rank them, and copy the top entries out. The
   * hash table's slot order is arbitrary, but the comparator is a total order
   * over distinct words, so the ranking is still reproducible. */
  if (a->words.size > 0 && a->config.top_n > 0) {
    WordEntry *ranked = malloc(a->words.size * sizeof(WordEntry));
    if (!ranked)
      return -1;
    size_t ranked_count = 0;
    for (size_t i = 0; i < a->words.capacity; i++) {
      if (a->words.slots[i].word)
        ranked[ranked_count++] = a->words.slots[i];
    }
    qsort(ranked, ranked_count, sizeof(WordEntry), cmp_word_entry_desc);

    out->top_word_count = ranked_count < (size_t)a->config.top_n
                              ? (int)ranked_count
                              : a->config.top_n;
    out->top_words = malloc((size_t)out->top_word_count * sizeof(WordFreq));
    if (!out->top_words) {
      free(ranked);
      out->top_word_count = 0;
      return -1;
    }
    for (int i = 0; i < out->top_word_count; i++) {
      /* The interned word is already bounded by max_word_len <= MAX_WORD_BUF,
       * so it always fits; copy the exact bytes plus the terminator. */
      const size_t len = strlen(ranked[i].word);
      memcpy(out->top_words[i].word, ranked[i].word, len + 1);
      out->top_words[i].count = ranked[i].count;
    }
    free(ranked);
  }

  /* Rank the printable characters that occurred at least once, sorted by count
   * descending with ties broken ascending by character. */
  CharFreq chars[PRINTABLE_ASCII_MAX - PRINTABLE_ASCII_MIN + 1];
  memset(chars, 0, sizeof(chars));
  int char_count = 0;
  for (int i = PRINTABLE_ASCII_MIN; i <= PRINTABLE_ASCII_MAX; i++) {
    if (a->char_counts[i] > 0) {
      chars[char_count].ch = (char)i;
      chars[char_count].count = a->char_counts[i];
      char_count++;
    }
  }
  qsort(chars, (size_t)char_count, sizeof(CharFreq), cmp_char_freq_desc);

  out->top_char_count =
      char_count < a->config.top_n ? char_count : a->config.top_n;
  if (out->top_char_count > 0) {
    out->top_chars = malloc((size_t)out->top_char_count * sizeof(CharFreq));
    if (!out->top_chars) {
      free(out->top_words);
      out->top_words = NULL;
      out->top_word_count = 0;
      out->top_char_count = 0;
      return -1;
    }
    memcpy(out->top_chars, chars,
           (size_t)out->top_char_count * sizeof(CharFreq));
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
    printf("%s\n    {\n", i == 0 ? "" : ",");
    /* words are ASCII letters only; no escaping needed */
    printf("      \"word\": \"%s\",\n", stats->top_words[i].word);
    printf("      \"count\": %ld,\n", stats->top_words[i].count);
    printf("      \"frequency\": %.4f\n", freq);
    printf("    }");
  }
  printf("%s],\n", stats->top_word_count > 0 ? "\n  " : "");

  printf("  \"top_characters\": [");
  for (int i = 0; i < stats->top_char_count; i++) {
    double freq = stats->char_count > 0
                      ? (double)stats->top_chars[i].count / stats->char_count
                      : 0.0;
    printf("%s\n    {\n", i == 0 ? "" : ",");
    printf("      \"char\": \"");
    print_json_char(stats->top_chars[i].ch);
    printf("\",\n");
    printf("      \"count\": %ld,\n", stats->top_chars[i].count);
    printf("      \"frequency\": %.4f\n", freq);
    printf("    }");
  }
  printf("%s]\n", stats->top_char_count > 0 ? "\n  " : "");

  printf("}\n");
}
