load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

cc_library(
    name = "copyfile",
    srcs = ["copyfile.c"],
    hdrs = ["copyfile.h"],
)

cc_binary(
    name = "copy_file",
    srcs = ["main.c"],
    deps = [":copyfile"],
)

cc_test(
    name = "copyfile_test",
    srcs = ["copyfile_test.c"],
    copts = ["-x", "c++"],
    deps = [
        ":copyfile",
        "@googletest//:gtest_main",
    ],
)
