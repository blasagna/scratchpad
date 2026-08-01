#!/usr/bin/env bash
#
# Build both matrix_ops ports, run the same invocations against each, and check
# they agree byte for byte.
#
#   ./matrix_ops/check_parity.sh          # build, run every case, report
#   ./matrix_ops/check_parity.sh --keep   # keep the work dir for inspection
#
# The C port is the reference; every other port is diffed against it. Three
# things are compared: stdout, the exit status, and — unlike simple_logger,
# where clap's wording legitimately differs — stderr as well. Both ports here
# hand-write their messages from the same contract, so an error message that
# drifts is a real divergence and worth catching.
#
# Builds are unoptimized: there are no timings here, so the --config=opt that
# bench/run.sh needs does not apply.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/matrix_ops_parity"

KEEP=0
if [[ "${1:-}" == "--keep" ]]; then
  KEEP=1
elif [[ $# -gt 0 ]]; then
  echo "usage: check_parity.sh [--keep]" >&2
  exit 2
fi

failed=0
CASES=()

# Ports under test, reference first.
binaries() {
  echo "C|${REPO_ROOT}/bazel-bin/matrix_ops/c/matrix_ops"
  echo "C++|${REPO_ROOT}/bazel-bin/matrix_ops/cpp/matrix_ops"
}

build() {
  echo "building..." >&2
  (cd "${REPO_ROOT}" && bazel build \
    //matrix_ops/c:matrix_ops //matrix_ops/cpp:matrix_ops) >&2
}

# A binary that cannot be executed makes every port fail identically, so every
# case would pass vacuously. Fail loudly instead.
check_binaries() {
  local port bin missing=0
  while IFS='|' read -r port bin; do
    if [[ ! -x "${bin}" ]]; then
      echo "missing or not executable: ${port} at ${bin}" >&2
      missing=1
    fi
  done < <(binaries)
  return "${missing}"
}

# run_case <name> <stdin-file|-> <args...>
#
# Runs every port with @FIXTURE@ in the arguments replaced by the fixture dir.
run_case() {
  local name="$1" stdin_file="$2"
  shift 2

  [[ "${stdin_file}" == "-" ]] && stdin_file="/dev/null"
  CASES+=("${name}")

  local port bin args arg status
  while IFS='|' read -r port bin; do
    args=()
    for arg in "$@"; do
      args+=("${arg//@FIXTURE@/${WORK}/fixtures}")
    done

    status=0
    "${bin}" "${args[@]}" \
      >"${WORK}/${name}.${port}.out" \
      2>"${WORK}/${name}.${port}.err" \
      <"${stdin_file}" || status=$?
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

compare() {
  local name port bin ref_port ref_bin
  IFS='|' read -r ref_port ref_bin < <(binaries)

  for name in ${CASES[@]+"${CASES[@]}"}; do
    local case_failed=0
    while IFS='|' read -r port bin; do
      [[ "${port}" == "${ref_port}" ]] && continue

      local stream
      for stream in out err status; do
        if ! diff -q "${WORK}/${name}.${ref_port}.${stream}" \
                     "${WORK}/${name}.${port}.${stream}" >/dev/null; then
          echo "PARITY FAILURE: ${name}: ${port} differs from ${ref_port} on ${stream}" >&2
          diff "${WORK}/${name}.${ref_port}.${stream}" \
               "${WORK}/${name}.${port}.${stream}" >&2 || true
          case_failed=1
          failed=1
        fi
      done
    done < <(binaries)
    [[ "${case_failed}" == "0" ]] && echo "ok: ${name}" >&2
  done
}

make_fixtures() {
  local dir="${WORK}/fixtures"
  mkdir -p "${dir}"
  printf '1 2 3\n4 5 6\n'      >"${dir}/a2x3.txt"
  printf '7 8\n9 10\n11 12\n'  >"${dir}/b3x2.txt"
  printf '1 2 3 4 5 6'         >"${dir}/flat6.txt"
  printf '1 2 3\n4 5\n'        >"${dir}/ragged.txt"
  printf '1 2\r\n3 4\r\n'      >"${dir}/crlf.txt"
  printf '\n\n  1   2  \n\n  3   4  \n\n' >"${dir}/padded.txt"
  printf ''                    >"${dir}/empty.txt"
  printf '1 abc 3\n'           >"${dir}/badnum.txt"
}

main() {
  build
  check_binaries

  rm -rf "${WORK}"
  mkdir -p "${WORK}"
  make_fixtures

  # --- happy paths, shape inference ---
  run_case row_vectors        - add --values "1 2 3" --values "4 5 6"
  run_case single_value       - add --values "7" --values "8"
  run_case from_files         - add --file @FIXTURE@/a2x3.txt --file @FIXTURE@/a2x3.txt
  run_case crlf_file          - add --file @FIXTURE@/crlf.txt --file @FIXTURE@/crlf.txt
  run_case padded_file        - add --file @FIXTURE@/padded.txt --file @FIXTURE@/padded.txt
  run_case sub_basic          - sub --values "10 20 30" --values "1 2 3"

  # --- shape overrides ---
  run_case reshape_both       - mul --rows 2 --cols 3 --values "1 2 3 4 5 6" \
                                    --rows 3 --cols 2 --values "7 8 9 10 11 12"
  run_case reshape_rows_only  - add --rows 2 --values "1 2 3 4 5 6" \
                                    --rows 2 --values "1 1 1 1 1 1"
  run_case reshape_cols_only  - add --cols 3 --values "1 2 3 4 5 6" \
                                    --cols 3 --values "1 1 1 1 1 1"
  run_case override_layout    - add --rows 3 --cols 2 --file @FIXTURE@/a2x3.txt \
                                    --rows 3 --cols 2 --file @FIXTURE@/a2x3.txt
  run_case mul_nonsquare      - mul --file @FIXTURE@/a2x3.txt --file @FIXTURE@/b3x2.txt
  run_case mul_vectors        - mul --rows 1 --cols 3 --values "1 2 3" \
                                    --rows 3 --cols 1 --values "4 5 6"
  run_case mul_outer          - mul --rows 3 --cols 1 --values "1 2 3" \
                                    --rows 1 --cols 3 --values "4 5 6"

  # --- scale and formatting ---
  run_case scale_basic        - scale --scalar 2.5 --file @FIXTURE@/a2x3.txt
  run_case scale_zero         - scale --scalar 0 --values "1 -2 3"
  run_case scale_negative     - scale --scalar -1 --values "1 -2 3"
  run_case scale_third        - scale --scalar 0.333333 --rows 2 --cols 2 \
                                     --values "1 10 100 1000"
  run_case precision_3        - scale --scalar 0.333333 --precision 3 \
                                     --values "1 10 100 1000"
  run_case precision_0        - scale --scalar 0.5 --precision 0 --values "1 2 3 4"
  run_case rounding_tie       - scale --scalar 1 --precision 1 --values "0.25 0.5"
  run_case negative_zero      - scale --scalar 0 --precision 2 --values "-1 2 -3"
  run_case wide_columns       - add --values "1 22 333" --values "4444 5 6"
  run_case exponents          - add --values "1e3 -1.5E-2 .5 4." \
                                    --values "+3 0 0 0"

  # --- stdin ---
  run_case stdin_matrix   "${WORK}/fixtures/a2x3.txt" scale --scalar 2 --file -
  run_case stdin_flat     "${WORK}/fixtures/flat6.txt" scale --scalar 2 --rows 2 --cols 3 --file -

  # --- option handling ---
  run_case help               - --help
  run_case permuted_op        - --values "1 2 3" --values "4 5 6" add
  run_case short_flags        - mul -r 2 -c 3 -v "1 2 3 4 5 6" -r 3 -c 2 -v "1 2 3 4 5 6"

  # --- errors: every one must agree on message and exit code ---
  run_case err_shape_mismatch - add --values "1 2" --values "1 2 3"
  run_case err_inner_dim      - mul --values "1 2 3" --values "1 2 3"
  run_case err_ragged         - scale --scalar 1 --file @FIXTURE@/ragged.txt
  run_case err_ragged_stdin   "${WORK}/fixtures/ragged.txt" scale --scalar 1 --file -
  run_case err_empty_values   - add --values "" --values "1"
  run_case err_empty_file     - scale --scalar 1 --file @FIXTURE@/empty.txt
  run_case err_bad_number     - scale --scalar 1 --file @FIXTURE@/badnum.txt
  run_case err_nan            - add --values "1 nan 3" --values "1 2 3"
  run_case err_inf            - add --values "1 inf 3" --values "1 2 3"
  run_case err_overflow_num   - add --values "1e400" --values "1"
  run_case err_missing_file   - add --file /nonexistent --values "1"
  run_case err_bad_shape      - add --rows 2 --cols 4 --values "1 2 3 4 5 6" --values "1"
  run_case err_uneven_divide  - add --rows 2 --values "1 2 3 4 5" --values "1"
  run_case err_dangling_dims  - add --values "1 2 3 4 5" --rows 2
  run_case err_zero_rows      - add --rows 0 --values "1 2"
  run_case err_too_many       - add --values "1" --values "2" --values "3"
  run_case err_unknown_op     - frobnicate --values "1" --values "2"
  run_case err_missing_op     - --values "1" --values "2"
  run_case err_wrong_count    - add --values "1"
  run_case err_scale_no_k     - scale --values "1 2"
  run_case err_scalar_misuse  - add --scalar 3 --values "1" --values "2"
  run_case err_bad_scalar     - scale --scalar abc --values "1"
  run_case err_scalar_inf     - scale --scalar inf --values "1"
  run_case err_extra_arg      - add extra --values "1" --values "2"
  run_case err_unknown_opt    - add --bogus --values "1" --values "2"
  run_case err_unknown_short  - add -z --values "1" --values "2"
  run_case err_missing_value  - add --values "1" --values "2" --rows
  run_case err_missing_short  - add --values "1" --values "2" -r

  # --- attached option values, and the numeric bounds around them ---
  # Everything below was found by a code review, not by this script: the
  # original cases only ever used the "--option value" spelling, so a whole
  # class of divergence went uncovered.
  run_case attached_long      - scale --scalar 1 --rows=2 --values "1 2 3 4"
  run_case attached_short     - scale --scalar 1 -r2 --values "1 2 3 4"
  run_case attached_both_dims - mul --rows=2 --cols=3 --values "1 2 3 4 5 6" \
                                    -r3 -c2 --values "1 2 3 4 5 6"
  run_case attached_values    - add --values="1 2 3" --values="4 5 6"
  run_case attached_scalar    - scale --scalar=2.5 --values "1 2"
  run_case attached_precision - scale --scalar 1 --precision=2 --values "0.125"
  run_case attached_file      - scale --scalar 1 --file=@FIXTURE@/a2x3.txt
  run_case err_value_on_flag  - add --help=x --values "1"
  run_case err_unknown_clustered - add -zq --values "1" --values "2"

  # --precision feeds a width argument in both ports; an unbounded one used to
  # abort the C++ port outright and silently wrap at 2^32.
  run_case err_precision_intmax - scale --scalar 1 --precision 2147483648 --values "1.5"
  run_case err_precision_uintmax - scale --scalar 1 --precision 4294967296 --values "1.5"
  run_case err_rows_huge      - scale --scalar 1 --rows 5000000000 --values "1 2 3 4"
  run_case err_cols_huge      - scale --scalar 1 --cols 5000000000 --values "1 2 3 4"

  # A wide render: 309 digits before the point plus 300 after, which overran a
  # fixed buffer in the C port and was reported as "out of memory: Success".
  run_case wide_precision     - scale --scalar 1e300 --precision 300 --values "1"
  run_case max_precision      - scale --scalar 1 --precision 400 --values "0.1"

  # Reading a directory: an operational failure (exit 1), not a usage error.
  run_case err_read_directory - scale --scalar 1 --file /tmp

  compare

  if [[ "${KEEP}" == "1" ]]; then
    echo "work dir kept at ${WORK}" >&2
  else
    rm -rf "${WORK}"
  fi

  if [[ "${failed}" == "0" ]]; then
    echo "all ${#CASES[@]} cases agree" >&2
  else
    echo "PARITY CHECK FAILED" >&2
  fi
  return "${failed}"
}

main "$@"
