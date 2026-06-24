load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "math",
    srcs = ["math.c"],
    hdrs = ["math.h"],
)

cc_binary(
    name = "main",
    srcs = ["main.c"],
    deps = [
        ":math",
    ]
)

cc_test(
    name = "test_math",
    srcs = ["test_math.c"],
    copts = ["-x", "c++"],
    deps = [
        ":math",
        "@googletest//:gtest_main",
    ],
)
