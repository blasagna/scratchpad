load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "atomicity",
    srcs = ["atomicity.cpp"],
    hdrs = ["atomicity.hpp"],
    linkopts = ["-pthread"],
)

cc_test(
    name = "test_atomicity",
    srcs = ["test_atomicity.cpp"],
    deps = [
        ":atomicity",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "bench_atomicity",
    srcs = ["bench_atomicity.cpp"],
    deps = [
        ":atomicity",
        "@google_benchmark//:benchmark_main",
    ],
)
