#include <stdio.h>

int main(void) {
    printf("Hello, world!\n");
    printf("Hello, blasagna!\n");
    
    // puts() writes the string s and a trailing newline to stdout.
    int res_puts = puts("Hello");
    printf("puts returns a nonnegative number on success, or EOF on error. result: %d\n", res_puts);
    if (res_puts == EOF) {
        printf("puts failed\n");
    }
    return 0;
}
