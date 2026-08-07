
#include <stddef.h>
#include <stdio.h>

#include "example.h"

int main(void) {
  int arr[] = {2, 4, 6, 8};
  size_t len = sizeof(arr) / sizeof(arr[0]);
  int sum = sum_array(arr, len);
  printf("sum: %d\n", sum);

  size_t wrong_len = func_sizeof_array(arr);
  printf("main scoped array length: %zu\n", len);
  printf("function scoped array length: %zu\n", wrong_len);

  return 0;
}
