load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "instruction_cache",
    srcs = ["instruction_cache.cpp"],
    hdrs = ["instruction_cache.hpp"],
)

cc_test(
    name = "test_instruction_cache",
    srcs = ["test_instruction_cache.cpp"],
    deps = [
        ":instruction_cache",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_instruction_cache",
    srcs = ["bench_instruction_cache.cpp"],
    deps = [
        ":instruction_cache",
        "@google_benchmark//:benchmark_main",
    ],
)
