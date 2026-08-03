load("@rules_cc//cc:defs.bzl", "cc_binary")

# The only place in the repo that depends on a third-party library. The ports
# themselves (matrix_ops/c, matrix_ops/cpp) stay dependency-free.
#
# These targets are NOT hermetic: xtensor-blas links against a system OpenBLAS.
# See //third_party/xtensor_blas.BUILD and matrix_ops/CLAUDE.md.

cc_binary(
    name = "compare",
    srcs = ["compare.cpp"],
    # Eigen is header-only, so there is no "Eigen built with OpenMP" to depend
    # on: its GEMM is guarded by #ifdef EIGEN_HAS_OPENMP, which keys off the
    # _OPENMP macro the compiler defines for -fopenmp. Putting the flag on THIS
    # target is the whole of enabling Eigen's parallelism. Without it, table 3
    # would compare a 16-thread OpenBLAS against a 1-thread Eigen.
    copts = ["-fopenmp"],
    linkopts = ["-fopenmp"],
    deps = [
        "//matrix_ops/cpp:matrix",
        "@eigen",
        "@xtensor",
        "@xtensor_blas",
    ],
)

# The smallest program that proves all three libraries compile under this
# repo's -Werror and that xt::linalg::dot links. Kept as a fast check when the
# dependency versions change; :compare takes far longer to build.
cc_binary(
    name = "probe",
    srcs = ["probe.cpp"],
    deps = [
        "@eigen",
        "@xtensor",
        "@xtensor_blas",
    ],
)
