#include <getopt.h>
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
    printf("  --top-n N           number of top words/chars to report (default: %d)\n",
           DEFAULT_TOP_N);
    printf("  --max-word-len N    max characters per word before truncation (default: %d)\n",
           DEFAULT_MAX_WORD_LEN);
    printf("  --word-table-cap N  initial word frequency table capacity (default: %d)\n",
           DEFAULT_WORD_TABLE_CAP);
    printf("  --json              print the summary as JSON instead of text\n");
    printf("  -h, --help          show this help\n");
}

int main(int argc, char *argv[]) {
    AnalyzerConfig config = {
        .top_n = DEFAULT_TOP_N,
        .max_word_len = DEFAULT_MAX_WORD_LEN,
        .word_table_init_cap = DEFAULT_WORD_TABLE_CAP,
    };

    int json_output = 0;

    static struct option long_opts[] = {
        {"top-n",          required_argument, NULL, 't'},
        {"max-word-len",   required_argument, NULL, 'm'},
        {"word-table-cap", required_argument, NULL, 'w'},
        {"json",           no_argument,       NULL, 'j'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 't': config.top_n              = atoi(optarg); break;
            case 'm': config.max_word_len        = atoi(optarg); break;
            case 'w': config.word_table_init_cap = atoi(optarg); break;
            case 'j': json_output = 1; break;
            case 'h':
                print_help();
                return 0;
            default:
                fprintf(stderr, "usage: text_analyzer [options] <file>\n");
                fprintf(stderr, "       text_analyzer --help\n");
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: no file specified\n");
        fprintf(stderr, "usage: text_analyzer [options] <file>\n");
        fprintf(stderr, "       text_analyzer --help\n");
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
