load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "logger",
    srcs = ["logger.cpp"],
    hdrs = ["logger.hpp"],
)

cc_binary(
    name = "simple_logger",
    srcs = ["main.cpp"],
    deps = [":logger"],
)

cc_test(
    name = "test_logger",
    srcs = ["test_logger.cpp"],
    deps = [
        ":logger",
        "@googletest//:gtest_main",
    ],
)
