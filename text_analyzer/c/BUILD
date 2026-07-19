load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "analyzer",
    srcs = ["analyzer.c"],
    hdrs = ["analyzer.h"],
)

cc_binary(
    name = "text_analyzer",
    srcs = ["main.c"],
    deps = [":analyzer"],
)

cc_test(
    name = "test_analyzer",
    srcs = ["test_analyzer.c"],
    copts = ["-x", "c++"],
    deps = [
        ":analyzer",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "test_golden",
    srcs = ["test_golden.c"],
    copts = ["-x", "c++"],
    data = ["//text_analyzer/testdata:golden"],
    deps = [
        ":analyzer",
        "@googletest//:gtest_main",
    ],
)
