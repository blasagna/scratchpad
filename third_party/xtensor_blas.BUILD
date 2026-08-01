load("@rules_cc//cc:defs.bzl", "cc_library")

# xtensor-blas: xt::linalg on top of xtensor. Not in the Bazel Central Registry,
# so this BUILD file is ours to maintain; see //MODULE.bazel for the archive.

cc_library(
    name = "xtensor_blas",
    # The vendored FLENS tree splits every routine into a .h declaration and a
    # .tcc definition that the .h includes at the bottom. The .tcc files are
    # textual_hdrs rather than hdrs because they are not standalone translation
    # units and do not compile on their own.
    hdrs = glob([
        "include/**/*.hpp",
        "include/**/*.h",
    ]),
    # This is load-bearing, and its absence fails silently.
    #
    # xtensor-blas's own xblas_config.hpp does `#ifndef XTENSOR_USE_FLENS_BLAS
    # / #define HAVE_CBLAS 1`, but the vendored cxxblas headers are
    # preprocessed BEFORE that header is reached, so every `#ifdef HAVE_CBLAS`
    # block in them -- including the cblas_dgemm overloads in
    # level3/gemm.tcc -- is skipped. cxxblas then falls back to its own
    # generic C++ gemm, which still gives the right answer, so nothing fails:
    # it is simply ~50x slower than the BLAS you thought you were calling, and
    # `ldd` still shows libopenblas because it is linked, just never called.
    #
    # Upstream's CMake does not hit this because it passes HAVE_CBLAS as a
    # compile definition on the command line. This does the same.
    #
    # Verify with: nm -D --undefined-only <binary> | grep cblas
    defines = ["HAVE_CBLAS=1"],
    linkopts = [
        # xtensor-blas reaches BLAS through FLENS cxxblas, which declares the
        # Fortran symbols itself rather than including a BLAS header. So no
        # cblas.h/lapacke.h is needed anywhere -- only the shared library at
        # link time. This is what makes //matrix_ops/bench non-hermetic: it
        # requires a system OpenBLAS. See matrix_ops/CLAUDE.md.
        "-lopenblas",
    ],
    strip_include_prefix = "include",
    textual_hdrs = glob([
        "include/**/*.tcc",
        "include/**/*.cxx",
        "include/**/*.cc",
    ]),
    visibility = ["//visibility:public"],
    deps = ["@xtensor"],
)
