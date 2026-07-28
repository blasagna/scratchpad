load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

# The pure C++ library. Nothing here knows about Rust; the cargo build in
# ../rust compiles these same sources a second time through its build script.
cc_library(
    name = "exprkit",
    srcs = ["exprkit.cpp"],
    hdrs = ["exprkit.hpp"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "exprkit_cli",
    srcs = ["main.cpp"],
    deps = [":exprkit"],
)

cc_test(
    name = "test_exprkit",
    srcs = ["test_exprkit.cpp"],
    deps = [
        ":exprkit",
        "@googletest//:gtest_main",
    ],
)
