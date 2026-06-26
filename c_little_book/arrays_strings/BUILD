load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "example",
    srcs = ["example.c"],
    hdrs = ["example.h"],
)

cc_binary(
    name = "main",
    srcs = ["main.c"],
    deps = [
        ":example",
    ]
)

cc_test(
    name = "test_example",
    srcs = ["test_example.c"],
    copts = ["-x", "c++"],
    deps = [
        ":example",
        "@googletest//:gtest_main",
    ],
)
