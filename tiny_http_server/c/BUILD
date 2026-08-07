load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "http",
    srcs = ["http.c"],
    hdrs = ["http.h"],
)

cc_library(
    name = "server",
    srcs = ["server.c"],
    hdrs = ["server.h"],
    deps = [":http"],
)

cc_binary(
    name = "tiny_http_server",
    srcs = ["main.c"],
    deps = [
        ":http",
        ":server",
    ],
)

cc_test(
    name = "test_http",
    srcs = ["test_http.c"],
    copts = ["-x", "c++"],
    deps = [
        ":http",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "test_server",
    srcs = ["test_server.c"],
    copts = ["-x", "c++"],
    deps = [
        ":server",
        "@googletest//:gtest_main",
    ],
)
