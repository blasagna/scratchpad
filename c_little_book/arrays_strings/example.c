#include <stddef.h>

#include "example.h"

int sum_array(const int arr[], size_t len) {
  int sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += arr[i];
  }
  return sum;
}

size_t func_sizeof_array(int arr[]) {
  // demonstrate that sizeof is unexpected in a function scope when array is a
  // parameter: arr has decayed to an int *, so this measures the pointer
  //
  // -Wsizeof-array-argument diagnoses exactly this mistake, and the repo builds
  // with -Werror, so it is suppressed for this one expression on purpose --
  // getting the wrong answer here is the whole point of the exercise.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsizeof-array-argument"
  // cppcheck-suppress sizeofwithsilentarraypointer
  return sizeof(arr) / sizeof(arr[0]);
#pragma GCC diagnostic pop
}
