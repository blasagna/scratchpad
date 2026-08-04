load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "shell",
    srcs = ["shell.c"],
    hdrs = ["shell.h"],
)

cc_binary(
    name = "mini_shell",
    srcs = ["main.c"],
    deps = [":shell"],
)

cc_test(
    name = "test_shell",
    srcs = ["test_shell.c"],
    copts = ["-x", "c++"],
    deps = [
        ":shell",
        "@googletest//:gtest_main",
    ],
)
