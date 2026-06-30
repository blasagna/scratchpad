#include <stddef.h>

#include "example.h"

int sum_array(int arr[], size_t len) {
  int sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += arr[i];
  }
  return sum;
}

size_t func_sizeof_array(int arr[]) {
  // demonstrate that sizeof is unexpected in a function scope when array is a
  // parameter
  return sizeof(arr) / sizeof(arr[0]);
}
