load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# Shared helpers used across the Section-6 demos. Visible to every project under
# //memory_optimization/... but nowhere else.

cc_library(
    name = "cacheinfo",
    srcs = ["cacheinfo.cpp"],
    hdrs = ["cacheinfo.hpp"],
    visibility = ["//memory_optimization:__subpackages__"],
)

# Header-only cycle counter. srcs is empty; consumers get the inline __rdtsc()
# wrapper.
cc_library(
    name = "rdtsc",
    hdrs = ["rdtsc.hpp"],
    visibility = ["//memory_optimization:__subpackages__"],
)

cc_test(
    name = "test_cacheinfo",
    srcs = ["test_cacheinfo.cpp"],
    deps = [
        ":cacheinfo",
        "@googletest//:gtest_main",
    ],
)
