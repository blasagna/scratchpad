load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "copyfile",
    srcs = ["copyfile.cpp"],
    hdrs = ["copyfile.hpp"],
)

cc_binary(
    name = "copy_file",
    srcs = ["main.cpp"],
    deps = [
        ":copyfile",
        "@cli11",
    ],
)

cc_test(
    name = "test_copyfile",
    srcs = ["test_copyfile.cpp"],
    deps = [
        ":copyfile",
        "@googletest//:gtest_main",
    ],
)
