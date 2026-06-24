#include <stdio.h>

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

int main(void) {
    int count_down_start = 10;
    printf("count down from %d: \n", count_down_start);
    count_down(count_down_start);
    printf("\n==========\n");
    
    int count_up_stop = 16;
    printf("count up to %d: \n", count_up_stop);
    count_up(1, count_up_stop);
    printf("\n==========\n");
    
    int factorial_in= 5;
    printf("%d!: %d", factorial_in, factorial(factorial_in));
    printf("\n==========\n");
    
    int sum_n = 8;
    printf("sum to %d: %d", sum_n, sum_to_n(sum_n));
    printf("\n==========\n");
    return 0;
}
