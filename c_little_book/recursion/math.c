#include <stdio.h>
#include "math.h"

void count_down(int n) {
    if (n == 0) {
        return;
    }
    printf("%d ", n);
    count_down(n - 1);
}

void count_up(int current, int n) {
   if (current > n) {
       return;
   }

   printf("%d ", current);

   count_up(current + 1, n);
}

int factorial(int n) {
    if (n == 0) {
        return 1;
    }
    return n * factorial(n - 1);
}

int sum_to_n(int n) {
    if (n == 1) {
        return 1;
    }
    return n + sum_to_n(n - 1);
}

int power(int a, int b) {
    if (b == 1) {
        return a;
    }
    return a * power(a, b - 1);
}