load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "tlb_usage",
    srcs = ["tlb_usage.cpp"],
    hdrs = ["tlb_usage.hpp"],
)

cc_test(
    name = "test_tlb_usage",
    srcs = ["test_tlb_usage.cpp"],
    deps = [
        ":tlb_usage",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_tlb_usage",
    srcs = ["bench_tlb_usage.cpp"],
    deps = [
        ":tlb_usage",
        "//memory_optimization/support:rdtsc",
        "@google_benchmark//:benchmark_main",
    ],
)
