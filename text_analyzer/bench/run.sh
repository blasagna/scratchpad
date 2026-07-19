#!/usr/bin/env bash
#
# Benchmark the three text_analyzer ports against each other, and check that
# they still agree byte for byte.
#
#   ./bench/run.sh            # build, benchmark, print a markdown table
#   ./bench/run.sh --check    # parity check only, no timings
#
# All binaries are built optimized. The default Bazel build is fastbuild (-O0),
# so timings taken without --config=opt are not comparable to cargo --release.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH_DIR="${REPO_ROOT}/text_analyzer/bench"
CORPUS_DIR="${TMPDIR:-/tmp}/text_analyzer_bench"

REPETITIONS=5

# Corpora: same size and word count, differing only in vocabulary size.
CORPUS_NAMES=("40,000 distinct" "50 distinct")
CORPUS_FILES=("${CORPUS_DIR}/big.txt" "${CORPUS_DIR}/few.txt")
CORPUS_DISTINCT=(40000 50)

build() {
  echo "building (optimized)..." >&2
  (cd "${REPO_ROOT}" && bazel build --config=opt \
    //text_analyzer/c:text_analyzer //text_analyzer/cpp:text_analyzer) >&2
  (cd "${REPO_ROOT}/text_analyzer/rust" && cargo build --release --quiet) >&2
}

generate_corpora() {
  mkdir -p "${CORPUS_DIR}"
  for i in "${!CORPUS_FILES[@]}"; do
    if [[ ! -f "${CORPUS_FILES[$i]}" ]]; then
      echo "generating ${CORPUS_FILES[$i]}..." >&2
      python3 "${BENCH_DIR}/gen_corpus.py" \
        --distinct "${CORPUS_DISTINCT[$i]}" --out "${CORPUS_FILES[$i]}"
    fi
  done
}

# Binaries under test, in report order.
binaries() {
  echo "C|${REPO_ROOT}/bazel-bin/text_analyzer/c/text_analyzer"
  echo "C++|${REPO_ROOT}/bazel-bin/text_analyzer/cpp/text_analyzer"
  echo "Rust|${REPO_ROOT}/target/release/text_analyzer"
}

# Best wall-clock time of $REPETITIONS runs, in seconds to three decimals.
# Reports the best rather than the mean: the fastest run is the one least
# perturbed by unrelated system noise.
best_time() {
  local bin="$1" corpus="$2" best="" start end elapsed
  for _ in $(seq "${REPETITIONS}"); do
    start=$(date +%s%N)
    "${bin}" "${corpus}" >/dev/null
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    if [[ -z "${best}" || "${elapsed}" -lt "${best}" ]]; then
      best="${elapsed}"
    fi
  done
  printf '%d.%03d' $((best / 1000)) $((best % 1000))
}

# All three ports must produce identical output in both formats, not just text.
check_parity() {
  local failed=0
  for i in "${!CORPUS_FILES[@]}"; do
    local corpus="${CORPUS_FILES[$i]}"
    for format in text json; do
      local reference="" name bin flags=()
      [[ "${format}" == "json" ]] && flags=(--json)
      while IFS='|' read -r name bin; do
        local out
        out="$("${bin}" "${flags[@]}" "${corpus}")"
        if [[ -z "${reference}" ]]; then
          reference="${out}"
        elif [[ "${out}" != "${reference}" ]]; then
          echo "PARITY FAILURE: ${name} differs on ${corpus} (${format})" >&2
          diff <(echo "${reference}") <(echo "${out}") >&2 || true
          failed=1
        fi
      done < <(binaries)
      echo "parity ok: ${CORPUS_NAMES[$i]} (${format})" >&2
    done
  done
  return "${failed}"
}

main() {
  build
  generate_corpora

  if [[ "${1:-}" == "--check" ]]; then
    check_parity
    return
  fi

  check_parity

  echo
  echo "| corpus (2.7 MB, 400k words) | C | C++ | Rust |"
  echo "|---|---|---|---|"
  for i in "${!CORPUS_FILES[@]}"; do
    local row="| ${CORPUS_NAMES[$i]} |"
    local name bin
    while IFS='|' read -r name bin; do
      row+=" $(best_time "${bin}" "${CORPUS_FILES[$i]}")s |"
    done < <(binaries)
    echo "${row}"
  done
  echo
  echo "best of ${REPETITIONS} runs, optimized builds"
}

main "$@"
