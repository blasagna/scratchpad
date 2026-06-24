#include <stdio.h>

#include "math.h"

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

    int power_a = 2;
    int power_b = 10;
    printf("%d^%d = %d", power_a, power_b, power(power_a, power_b));
    printf("\n==========\n");
    
    return 0;
}
