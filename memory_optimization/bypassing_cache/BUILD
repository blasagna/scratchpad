load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "bypass",
    srcs = ["bypass.cpp"],
    hdrs = ["bypass.hpp"],
)

cc_test(
    name = "test_bypass",
    srcs = ["test_bypass.cpp"],
    deps = [
        ":bypass",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_bypass",
    srcs = ["bench_bypass.cpp"],
    deps = [
        ":bypass",
        "@google_benchmark//:benchmark_main",
    ],
)
