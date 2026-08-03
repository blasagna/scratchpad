load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "matrix",
    srcs = ["matrix.cpp"],
    hdrs = ["matrix.hpp"],
    visibility = ["//matrix_ops/bench:__pkg__"],
)

cc_library(
    name = "matrix_io",
    srcs = ["matrix_io.cpp"],
    hdrs = ["matrix_io.hpp"],
    deps = [":matrix"],
)

cc_binary(
    name = "matrix_ops",
    srcs = ["main.cpp"],
    deps = [
        ":matrix",
        ":matrix_io",
    ],
)

cc_test(
    name = "test_matrix",
    srcs = ["test_matrix.cpp"],
    deps = [
        ":matrix",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "test_matrix_io",
    srcs = ["test_matrix_io.cpp"],
    deps = [
        ":matrix_io",
        "@googletest//:gtest_main",
    ],
)
