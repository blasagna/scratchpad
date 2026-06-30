load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "analyzer",
    srcs = ["analyzer.cpp"],
    hdrs = ["analyzer.hpp"],
)

cc_binary(
    name = "text_analyzer",
    srcs = ["main.cpp"],
    deps = [":analyzer"],
)

cc_test(
    name = "test_analyzer",
    srcs = ["test_analyzer.cpp"],
    deps = [
        ":analyzer",
        "@googletest//:gtest_main",
    ],
)
