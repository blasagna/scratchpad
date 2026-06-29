#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analyzer.h"

static void print_help(void) {
    printf("usage: text_analyzer <file>\n");
    printf("       text_analyzer -h | --help\n");
    printf("\n");
    printf("Reads a text file and prints statistics:\n");
    printf("  - total line, word, and character counts\n");
    printf("  - top %d most frequent words (case-insensitive)\n", TOP_N);
    printf("  - top %d most frequent non-space characters\n", TOP_N);
}

int main(int argc, char *argv[]) {
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_help();
        return 0;
    }

    if (argc != 2) {
        fprintf(stderr, "usage: text_analyzer <file>\n");
        fprintf(stderr, "       text_analyzer --help\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }

    TextStats stats;
    if (analyze_file(f, &stats) != 0) {
        fprintf(stderr, "error: failed to analyze file\n");
        fclose(f);
        return 1;
    }

    fclose(f);
    print_stats(&stats);
    return 0;
}
