#!/usr/bin/env bash
#
# Compare matrix_ops' hand-written implementation against faer and nalgebra.
#
#   ./matrix_ops/bench/rust/run.sh            # verify, benchmark, print tables
#   ./matrix_ops/bench/rust/run.sh --check    # correctness check only
#
# The Rust counterpart of ../run.sh. The agreement test runs first and a failure
# stops everything, so a printed table also means the three implementations
# agree elementwise.
#
# Everything is built with --release. A timing taken on a debug build compares
# nothing useful, and the naive 1024x1024 multiply is minutes rather than
# seconds there.
#
# Unlike ../run.sh there is no third-party link to verify and no thread-pool
# contention to work around: faer and nalgebra are both pure Rust, nalgebra does
# not thread at all, and faer's parallelism is one global setting. That is why
# this script has three tables where the C++ one needs four.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CRITERION_DIR="${REPO_ROOT}/target/criterion"
THREADS="$(nproc)"

CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK_ONLY=1
elif [[ $# -gt 0 ]]; then
  echo "usage: run.sh [--check]" >&2
  exit 2
fi

# The correctness gate. compare.cpp verifies elementwise before printing any
# timing; criterion has no equivalent hook, so the check is a test and it runs
# here first.
check() {
  echo "checking that ours, faer, and nalgebra agree..." >&2
  (cd "${REPO_ROOT}" && cargo test --quiet --release -p matrix_ops_bench) >&2
  echo "all three agree on every operation and size" >&2
}

# bench <threads> [rustflags]
#
# criterion keys its output directory on the benchmark name alone, so each run
# overwrites the last. The table is extracted immediately after each run rather
# than at the end.
#
# Changing RUSTFLAGS forces a full rebuild of the dependency tree, which is why
# the two ISA tables are slower to produce than to read.
bench() {
  local threads="$1"
  local rustflags="${2:-}"
  (cd "${REPO_ROOT}" &&
    MATRIX_OPS_BENCH_THREADS="${threads}" RUSTFLAGS="${rustflags}" \
      cargo bench --quiet -p matrix_ops_bench) >&2
}

# Extracts criterion's point estimates into a Markdown table.
#
# estimates.json holds nanoseconds. `slope` is null under the flat sampling the
# slow benchmarks use, so `mean` is the field to read -- it is present under
# both sampling modes, which `slope` is not.
report() {
  python3 - "${CRITERION_DIR}" <<'PY'
import json, os, sys

root = sys.argv[1]
sizes, rows = [], {}
for op in ("add", "sub", "mul", "scale"):
    for impl in ("ours", "faer", "nalgebra"):
        base = os.path.join(root, op, impl)
        if not os.path.isdir(base):
            continue
        for size in os.listdir(base):
            path = os.path.join(base, size, "new", "estimates.json")
            if not os.path.exists(path):
                continue
            with open(path) as f:
                ns = json.load(f)["mean"]["point_estimate"]
            rows[(op, int(size), impl)] = ns / 1e6
            if int(size) not in sizes:
                sizes.append(int(size))

print("| op | size | ours | faer | nalgebra |")
print("|---|---|---|---|---|")
for op in ("add", "sub", "mul", "scale"):
    for n in sorted(sizes):
        cells = []
        for impl in ("ours", "faer", "nalgebra"):
            ms = rows.get((op, n, impl))
            cells.append("-" if ms is None else f"{ms:.3f} ms")
        print(f"| {op} | {n}x{n} | " + " | ".join(cells) + " |")
PY
}

main() {
  check
  if [[ "${CHECK_ONLY}" == "1" ]]; then
    return
  fi
  echo >&2

  # Three tables, because any one alone is misleading. Two variables are in
  # play and they have to be separated:
  #
  #   ISA       - faer and nalgebra dispatch some kernels at runtime, but our
  #               loop only gets the instructions it is compiled with. Comparing
  #               under the default baseline x86-64 build decides part of the
  #               result on compiler flags rather than on the code.
  #   threading - faer threads through rayon by default; nalgebra never does.
  echo "### 1. Baseline ISA, single-threaded"
  echo
  echo "What --release gives you by default: the baseline x86-64 instruction"
  echo "set, no AVX. The gap here is partly our flags rather than the kernels."
  echo
  bench 1
  report

  echo
  echo "### 2. Native ISA (-C target-cpu=native), single-threaded"
  echo
  echo "The like-for-like comparison: same instruction set, one thread each."
  echo "What is left is genuine kernel quality."
  echo
  bench 1 "-C target-cpu=native"
  report

  echo
  echo "### 3. Native ISA, ${THREADS} threads"
  echo
  echo "Only faer's column changes: nalgebra is single-threaded by construction"
  echo "and ours is a plain loop. Because nothing here contends for a shared"
  echo "thread pool, one table suffices -- the C++ benchmark needs two runs and"
  echo "tells you to read only one column from each."
  echo
  bench "${THREADS}" "-C target-cpu=native"
  report
}

main "$@"
