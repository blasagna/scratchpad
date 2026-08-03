#!/usr/bin/env bash
#
# Compare matrix_ops' hand-written implementation against Eigen and xtensor.
#
#   ./matrix_ops/bench/run.sh            # build optimized, verify, print a table
#   ./matrix_ops/bench/run.sh --check    # correctness check only, no timings
#
# The binary verifies all three libraries agree elementwise before it reports
# any timing, so a green table also means the implementations match.
#
# Everything is built with --config=opt. The default Bazel build is fastbuild
# (-O0), and a timing taken there compares nothing useful.
#
# NOT hermetic: xtensor-blas links against a system OpenBLAS. If the link
# fails, see matrix_ops/CLAUDE.md for the pure-C++ FLENS fallback and note that
# it changes the mul numbers by roughly an order of magnitude.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${REPO_ROOT}/bazel-bin/matrix_ops/bench/compare"

THREADS="$(nproc)"
CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK_ONLY=1
elif [[ $# -gt 0 ]]; then
  echo "usage: run.sh [--check]" >&2
  exit 2
fi

# build [extra bazel configs...]
build() {
  echo "building (${*:-opt})..." >&2
  (cd "${REPO_ROOT}" && bazel build --config=opt "$@" //matrix_ops/bench:compare) >&2
}

# Check that xtensor is really calling BLAS, and fail if it is not.
#
# This deliberately inspects the *symbols the binary references*, not `ldd`.
# libopenblas can be linked and never called -- that is exactly what happens
# when HAVE_CBLAS is not defined early enough (see //third_party/xtensor_blas.BUILD),
# and an ldd-based check reports a confident green while xtensor is quietly
# running FLENS's generic gemm at roughly 1/50th the speed. A benchmark that
# mislabels which implementation it measured is worse than no benchmark.
check_blas() {
  local symbols
  symbols="$(nm -D --undefined-only "${BIN}" 2>/dev/null | grep -ci 'cblas_' || true)"
  if [[ "${symbols}" -eq 0 ]]; then
    echo "ERROR: ${BIN} references no cblas_* symbols." >&2
    echo "xtensor is running the FLENS generic fallback, not a real BLAS." >&2
    echo "See //third_party/xtensor_blas.BUILD -- HAVE_CBLAS must be a -D." >&2
    exit 1
  fi
  local lib
  lib="$(ldd "${BIN}" 2>/dev/null | grep -io '[a-z0-9_]*blas[a-z0-9._]*' | head -1)"
  echo "xtensor BLAS: ${lib:-<none>} (${symbols} cblas symbols referenced)" >&2
}

main() {
  build

  if [[ ! -x "${BIN}" ]]; then
    echo "missing or not executable: ${BIN}" >&2
    exit 1
  fi

  check_blas
  echo >&2

  if [[ "${CHECK_ONLY}" == "1" ]]; then
    "${BIN}" >/dev/null
    return
  fi

  # Three tables, because any one of them alone is misleading. Two variables
  # are in play and they have to be separated:
  #
  #   ISA       - OpenBLAS dispatches to an AVX-512 kernel at runtime via
  #               CPUID regardless of our flags, while Eigen only gets what we
  #               compile it with. Comparing the two under the default
  #               baseline-x86-64 build decides the winner on compiler flags.
  #   threading - OpenBLAS threads by default; Eigen's GEMM does not unless
  #               built with OpenMP.
  echo "### 1. Baseline ISA (-march=x86-64), single-threaded"
  echo
  echo "What --config=opt gives you by default: SSE2, no FMA. OpenBLAS ignores"
  echo "this and uses AVX-512 anyway, so the gap here is mostly our flags."
  echo
  MATRIX_OPS_BENCH_THREADS=1 "${BIN}"

  build --config=native
  echo
  echo "### 2. Native ISA (-march=native), single-threaded"
  echo
  echo "The like-for-like comparison: same instruction set, one thread each."
  echo "What is left is genuine kernel quality."
  echo
  MATRIX_OPS_BENCH_THREADS=1 "${BIN}"

  # Threading needs TWO runs, and only one column of each is trustworthy.
  #
  # Both libraries thread, and their pools do not compose inside one process:
  #
  #   - OpenBLAS's idle threads busy-wait for a long timeout, so they are still
  #     burning cores while Eigen is being measured. compare.cpp pins the idle
  #     library to one thread to limit this, which helps but does not fully
  #     settle a pool that is already spinning.
  #   - OMP_PROC_BIND=true is what makes Eigen's numbers stable (without it the
  #     OS migrates its threads and 256x256 swings by 20x) -- but it binds the
  #     master thread, and OpenBLAS's pthread pool inherits that affinity mask
  #     and collapses onto one core.
  #
  # So each library gets a run configured in its favour, and you read only its
  # own column from that run. Doing this properly would mean one process per
  # library; this is the honest version of what a single process can measure.
  echo
  echo "### 3a. Native ISA, ${THREADS} threads -- read the **Eigen** column"
  echo
  echo "OMP_PROC_BIND=true, which stabilizes Eigen and handicaps OpenBLAS."
  echo
  OMP_PROC_BIND=true "${BIN}"

  echo
  echo "### 3b. Native ISA, ${THREADS} threads -- read the **xtensor** column"
  echo
  echo "No thread binding, so OpenBLAS gets all cores. Eigen's column here is"
  echo "unstable and should be ignored."
  echo
  "${BIN}"
}

main "$@"
