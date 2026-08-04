load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "shell",
    srcs = ["shell.cpp"],
    hdrs = ["shell.hpp"],
)

cc_binary(
    name = "mini_shell",
    srcs = ["main.cpp"],
    deps = [
        ":shell",
        "@cli11",
    ],
)

cc_test(
    name = "test_shell",
    srcs = ["test_shell.cpp"],
    deps = [
        ":shell",
        "@googletest//:gtest_main",
    ],
)
