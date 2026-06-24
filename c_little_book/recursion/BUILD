load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "math",
    srcs = ["math.c"],
    hdrs = ["math.h"],
)

cc_binary(
    name = "main",
    srcs = ["main.c"],
    deps = [
        ":math",
    ]
)
