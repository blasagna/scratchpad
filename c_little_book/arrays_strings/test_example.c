#include <gtest/gtest.h>

#include <stddef.h>

extern "C" {
#include "example.h"
}

TEST(ExampleTest, SumArray) {
  int arr[] = {2, 4, 6, 8};
  EXPECT_EQ(sum_array(arr, 4), 20);
}

TEST(ExampleTest, SumArrayWithNegatives) {
  int arr[] = {5, -3, -2};
  EXPECT_EQ(sum_array(arr, 3), 0);
}

TEST(ExampleTest, SumArrayOfLengthZeroIsZero) {
  int arr[] = {1, 2, 3};
  EXPECT_EQ(sum_array(arr, 0), 0);
}

TEST(ExampleTest, SumArrayReadsOnlyTheGivenLength) {
  int arr[] = {1, 2, 100};
  EXPECT_EQ(sum_array(arr, 2), 3);
}

TEST(ExampleTest, SizeofOnAnArrayParameterMeasuresThePointer) {
  // The exercise's point: inside a function the parameter has decayed to an
  // int *, so sizeof reports sizeof(int *) / sizeof(int) no matter how long the
  // caller's array actually was.
  int four[] = {2, 4, 6, 8};
  int two[] = {1, 2};

  // Spelled through variables because writing sizeof(int *) / sizeof(int)
  // directly trips -Wsizeof-pointer-div -- the compiler flags the very pattern
  // this test exists to pin down.
  const size_t pointer_size = sizeof(int *);
  const size_t int_size = sizeof(int);
  const size_t pointer_sized = pointer_size / int_size;

  EXPECT_EQ(func_sizeof_array(four), pointer_sized);
  EXPECT_EQ(func_sizeof_array(two), pointer_sized);

  // Same answer for both arrays regardless of their real lengths -- which is
  // why a length has to be passed separately, as sum_array does. Only `four` is
  // checked against its true length: on LP64 `pointer_sized` is 2, which is
  // coincidentally `two`'s real length, so the symmetric EXPECT_NE would be
  // false here. That coincidence is a property of this platform, not a
  // guarantee, so it is not asserted either way.
  EXPECT_EQ(func_sizeof_array(four), func_sizeof_array(two));
  EXPECT_NE(func_sizeof_array(four), sizeof(four) / sizeof(four[0]));
}
