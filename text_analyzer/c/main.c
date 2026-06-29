#include <stdio.h>
#include <stdlib.h>

#include "analyzer.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: text_analyzer <file>\n");
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
