#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "analyzer.h"

static const int DEFAULT_TOP_N = 5;
static const int DEFAULT_MAX_WORD_LEN = MAX_WORD_BUF;
static const int DEFAULT_WORD_TABLE_CAP = 64;

static void print_help(void) {
  printf("usage: text_analyzer [options] <file>\n");
  printf("       text_analyzer -h | --help\n");
  printf("\n");
  printf("Reads a text file and prints statistics:\n");
  printf("  - total line, word, and character counts\n");
  printf("  - top N most frequent words (case-insensitive)\n");
  printf("  - top N most frequent non-space characters\n");
  printf("\n");
  printf("Options:\n");
  printf("  --top-n N           number of top words/chars to report (default: "
         "%d)\n",
         DEFAULT_TOP_N);
  printf("  --max-word-len N    max characters per word before truncation "
         "(default: %d)\n",
         DEFAULT_MAX_WORD_LEN);
  printf("  --word-table-cap N  initial word frequency table capacity "
         "(default: %d)\n",
         DEFAULT_WORD_TABLE_CAP);
  printf("  --json              print the summary as JSON instead of text\n");
  printf("  -h, --help          show this help\n");
}

static void print_usage_error(void) {
  fprintf(stderr, "usage: text_analyzer [options] <file>\n");
  fprintf(stderr, "       text_analyzer --help\n");
}

/*
 * Parses value as a positive integer (>= 1, fitting in int). On success stores
 * it in *out and returns 0. On failure prints an error mentioning opt_name and
 * returns -1. Rejects empty input, trailing junk, and non-positive values.
 */
static int parse_positive(const char *opt_name, const char *value, int *out) {
  char *endp;
  long n = strtol(value, &endp, 10);
  if (endp == value || *endp != '\0' || n < 1 || n > INT_MAX) {
    fprintf(stderr,
            "error: invalid value '%s' for %s (expected a positive integer)\n",
            value, opt_name);
    return -1;
  }
  *out = (int)n;
  return 0;
}

int main(int argc, char *argv[]) {
  AnalyzerConfig config = {
      .top_n = DEFAULT_TOP_N,
      .max_word_len = DEFAULT_MAX_WORD_LEN,
      .word_table_init_cap = DEFAULT_WORD_TABLE_CAP,
  };

  int json_output = 0;

  static struct option long_opts[] = {
      {"top-n", required_argument, NULL, 't'},
      {"max-word-len", required_argument, NULL, 'm'},
      {"word-table-cap", required_argument, NULL, 'w'},
      {"json", no_argument, NULL, 'j'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
    switch (opt) {
    case 't':
      if (parse_positive("--top-n", optarg, &config.top_n) != 0)
        return 1;
      break;
    case 'm':
      if (parse_positive("--max-word-len", optarg, &config.max_word_len) != 0)
        return 1;
      break;
    case 'w':
      if (parse_positive("--word-table-cap", optarg,
                         &config.word_table_init_cap) != 0)
        return 1;
      break;
    case 'j':
      json_output = 1;
      break;
    case 'h':
      print_help();
      return 0;
    default:
      print_usage_error();
      return 1;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "error: no file specified\n");
    print_usage_error();
    return 1;
  }

  const char *filename = argv[optind];
  FILE *f = fopen(filename, "r");
  if (!f) {
    perror(filename);
    return 1;
  }

  TextStats stats;
  if (analyze_file(f, &config, &stats) != 0) {
    fprintf(stderr, "error: failed to analyze file\n");
    fclose(f);
    return 1;
  }

  fclose(f);
  if (json_output) {
    print_stats_json(&stats);
  } else {
    print_stats(&stats);
  }
  text_stats_free(&stats);
  return 0;
}
