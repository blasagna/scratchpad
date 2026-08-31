load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "working_set",
    srcs = ["working_set.cpp"],
    hdrs = ["working_set.hpp"],
    deps = ["//memory_optimization/support:cacheinfo"],
)

cc_test(
    name = "test_working_set",
    srcs = ["test_working_set.cpp"],
    deps = [
        ":working_set",
        "@googletest//:gtest_main",
    ],
)

# This bench provides its own main() (it registers the sweep at runtime), so it
# links @google_benchmark//:benchmark rather than :benchmark_main.
cc_binary(
    name = "bench_working_set",
    srcs = ["bench_working_set.cpp"],
    deps = [
        ":working_set",
        "//memory_optimization/support:cacheinfo",
        "@google_benchmark//:benchmark",
    ],
)
