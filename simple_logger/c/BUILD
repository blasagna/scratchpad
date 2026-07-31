load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "logger",
    srcs = ["logger.c"],
    hdrs = ["logger.h"],
)

cc_binary(
    name = "simple_logger",
    srcs = ["main.c"],
    deps = [":logger"],
)

cc_test(
    name = "test_logger",
    srcs = ["test_logger.c"],
    copts = ["-x", "c++"],
    deps = [
        ":logger",
        "@googletest//:gtest_main",
    ],
)
