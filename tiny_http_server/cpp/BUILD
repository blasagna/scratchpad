load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "http",
    srcs = ["http.cpp"],
    hdrs = ["http.hpp"],
)

cc_library(
    name = "server",
    srcs = ["server.cpp"],
    hdrs = ["server.hpp"],
    deps = [":http"],
)

cc_binary(
    name = "tiny_http_server",
    srcs = ["main.cpp"],
    deps = [
        ":http",
        ":server",
        "@cli11",
    ],
)

cc_test(
    name = "test_http",
    srcs = ["test_http.cpp"],
    deps = [
        ":http",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "test_server",
    srcs = ["test_server.cpp"],
    deps = [
        ":server",
        "@googletest//:gtest_main",
    ],
)
