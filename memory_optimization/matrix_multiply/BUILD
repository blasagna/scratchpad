load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "matmul",
    srcs = ["matmul.cpp"],
    hdrs = ["matmul.hpp"],
    deps = ["//memory_optimization/support:cacheinfo"],
)

cc_test(
    name = "test_matmul",
    srcs = ["test_matmul.cpp"],
    deps = [
        ":matmul",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_matmul",
    srcs = ["bench_matmul.cpp"],
    deps = [
        ":matmul",
        "//memory_optimization/support:rdtsc",
        "@google_benchmark//:benchmark_main",
    ],
)
