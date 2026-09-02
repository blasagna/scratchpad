load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "data_layout",
    srcs = ["data_layout.cpp"],
    hdrs = ["data_layout.hpp"],
)

cc_test(
    name = "test_data_layout",
    srcs = ["test_data_layout.cpp"],
    deps = [
        ":data_layout",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_data_layout",
    srcs = ["bench_data_layout.cpp"],
    deps = [
        ":data_layout",
        "@google_benchmark//:benchmark_main",
    ],
)
