load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "matrix",
    srcs = ["matrix.c"],
    hdrs = ["matrix.h"],
)

cc_library(
    name = "matrix_io",
    srcs = ["matrix_io.c"],
    hdrs = ["matrix_io.h"],
    deps = [":matrix"],
)

cc_binary(
    name = "matrix_ops",
    srcs = ["main.c"],
    deps = [
        ":matrix",
        ":matrix_io",
    ],
)

cc_test(
    name = "test_matrix",
    srcs = ["test_matrix.c"],
    copts = ["-x", "c++"],
    deps = [
        ":matrix",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "test_matrix_io",
    srcs = ["test_matrix_io.c"],
    copts = ["-x", "c++"],
    deps = [
        ":matrix_io",
        "@googletest//:gtest_main",
    ],
)
