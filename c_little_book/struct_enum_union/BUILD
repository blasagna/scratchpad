load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "variant",
    srcs = ["variant.c"],
    hdrs = ["variant.h"],
)

cc_binary(
    name = "main",
    srcs = ["main.c"],
    deps = [
        ":variant",
    ]
)
