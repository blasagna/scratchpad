#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "math.h"
}

TEST(MathTest, CountDown) {
    testing::internal::CaptureStdout();
    count_down(3);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "3 2 1 ");
}

TEST(MathTest, CountUp) {
    testing::internal::CaptureStdout();
    count_up(1, 4);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "1 2 3 4 ");
}

TEST(MathTest, Factorial) {
    EXPECT_EQ(factorial(0), 1);
    EXPECT_EQ(factorial(1), 1);
    EXPECT_EQ(factorial(5), 120);
}

TEST(MathTest, SumToN) {
    EXPECT_EQ(sum_to_n(1), 1);
    EXPECT_EQ(sum_to_n(8), 36);
}

TEST(MathTest, Power) {
    EXPECT_EQ(power(2, 10), 1024);
    EXPECT_EQ(power(3, 3), 27);
}
