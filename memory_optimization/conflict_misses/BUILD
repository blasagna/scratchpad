load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "conflict_misses",
    srcs = ["conflict_misses.cpp"],
    hdrs = ["conflict_misses.hpp"],
    deps = ["//memory_optimization/support:permutation"],
)

cc_test(
    name = "test_conflict_misses",
    srcs = ["test_conflict_misses.cpp"],
    deps = [
        ":conflict_misses",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_conflict_misses",
    srcs = ["bench_conflict_misses.cpp"],
    deps = [
        ":conflict_misses",
        "//memory_optimization/support:rdtsc",
        "@google_benchmark//:benchmark_main",
    ],
)
