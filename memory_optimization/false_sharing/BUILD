load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "false_sharing",
    srcs = ["false_sharing.cpp"],
    hdrs = ["false_sharing.hpp"],
    linkopts = ["-pthread"],
)

cc_test(
    name = "test_false_sharing",
    srcs = ["test_false_sharing.cpp"],
    deps = [
        ":false_sharing",
        "//memory_optimization/support:cacheinfo",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_false_sharing",
    srcs = ["bench_false_sharing.cpp"],
    deps = [
        ":false_sharing",
        "@google_benchmark//:benchmark_main",
    ],
)
