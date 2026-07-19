# Shared corpus for the cross-port golden tests. The C, C++, and Rust ports all
# compare against these same files, so all three matching means all three agree
# with each other.

filegroup(
    name = "golden",
    srcs = glob(["*"]),
    visibility = ["//text_analyzer:__subpackages__"],
)
