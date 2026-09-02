load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "software_prefetch",
    srcs = ["software_prefetch.cpp"],
    hdrs = ["software_prefetch.hpp"],
    deps = ["//memory_optimization/support:permutation"],
)

cc_test(
    name = "test_software_prefetch",
    srcs = ["test_software_prefetch.cpp"],
    deps = [
        ":software_prefetch",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_software_prefetch",
    srcs = ["bench_software_prefetch.cpp"],
    deps = [
        ":software_prefetch",
        "//memory_optimization/support:rdtsc",
        "@google_benchmark//:benchmark_main",
    ],
)
