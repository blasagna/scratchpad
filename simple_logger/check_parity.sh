#!/usr/bin/env bash
#
# Build all three simple_logger ports, run the same invocations against each
# with a pinned fake clock, and check they produce byte-identical log files.
#
#   ./simple_logger/check_parity.sh          # build, run every case, report
#   ./simple_logger/check_parity.sh --keep   # keep the work dir for inspection
#
# For each case every port writes to its own log file; the C port is the
# reference and the others are diffed against it. Three things are compared:
# the log file bytes, stdout (which must be empty everywhere, since success is
# silent), and the exit status. Stderr is deliberately NOT compared — each port
# reaches for its own argument parser (getopt_long, CLI11, clap) and their
# wording legitimately differs — it is only required to be non-empty when a run
# fails.
#
# Cases registered with run_case_parser_error are the ones the argument parsers
# reject rather than the program, so their exit codes are each parser's own too.
# There, every port must merely fail; the code is not compared.
#
# The clock is pinned through SIMPLE_LOGGER_FAKE_TIME, which every port reads
# only at its CLI boundary. A malformed value is a hard error in all three
# rather than a fallback to the real clock, so this script cannot pass by
# accidentally comparing three real clocks.
#
# Builds are unoptimized: there are no timings here, so the --config=opt dance
# that bench/run.sh needs does not apply.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/simple_logger_parity"

# Any epoch second works; this one is 2025-07-01T00:00:00Z, which is easy to
# recognize in the output when a case fails.
DEFAULT_FAKE_TIME=1751328000

KEEP=0
if [[ "${1:-}" == "--keep" ]]; then
  KEEP=1
elif [[ $# -gt 0 ]]; then
  echo "usage: check_parity.sh [--keep]" >&2
  exit 2
fi

failed=0
# Case names in the order they were first run, filled in by run_case so the
# comparison pass cannot drift out of sync with the cases above it.
CASES=()

# Cases whose exit code belongs to the argument parser rather than the program.
declare -A CASE_MUST_FAIL
MUST_FAIL=0

# Ports under test, reference first.
binaries() {
  echo "C|${REPO_ROOT}/bazel-bin/simple_logger/c/simple_logger"
  echo "C++|${REPO_ROOT}/bazel-bin/simple_logger/cpp/simple_logger"
  echo "Rust|${REPO_ROOT}/target/debug/simple_logger"
}

build() {
  echo "building..." >&2
  (cd "${REPO_ROOT}" && bazel build \
    //simple_logger/c:simple_logger //simple_logger/cpp:simple_logger) >&2
  (cd "${REPO_ROOT}" && cargo build --quiet -p simple_logger) >&2
}

# run_case <name> <fake-time|-> <stdin-file|-> <args...>
#
# Runs every port with @LOG@ in the arguments replaced by that port's own log
# file. Repeating a case name appends to the same files, which is how the
# multi-run cases work.
run_case() {
  local name="$1" fake_time="$2" stdin_file="$3"
  shift 3

  [[ "${fake_time}" == "-" ]] && fake_time="${DEFAULT_FAKE_TIME}"
  [[ "${stdin_file}" == "-" ]] && stdin_file="/dev/null"

  local seen=0 existing
  for existing in ${CASES[@]+"${CASES[@]}"}; do
    [[ "${existing}" == "${name}" ]] && seen=1
  done
  [[ "${seen}" == "0" ]] && CASES+=("${name}")
  CASE_MUST_FAIL["${name}"]="${MUST_FAIL}"

  local port bin log args arg status
  while IFS='|' read -r port bin; do
    log="${WORK}/${name}.${port}.log"

    args=()
    for arg in "$@"; do
      args+=("${arg//@LOG@/${log}}")
    done

    set +e
    SIMPLE_LOGGER_FAKE_TIME="${fake_time}" "${bin}" ${args[@]+"${args[@]}"} \
      <"${stdin_file}" \
      >"${WORK}/${name}.${port}.out" \
      2>"${WORK}/${name}.${port}.err"
    status=$?
    set -e
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

# run_case_parser_error <name> <fake-time|-> <stdin-file|-> <args...>
#
# For a command line the argument parser rejects. getopt_long reports it in the
# C port, CLI11 in the C++ one, and clap in the Rust one, so the exit code is
# each parser's own; all that is asserted is that every port rejected it.
run_case_parser_error() {
  MUST_FAIL=1
  run_case "$@"
  MUST_FAIL=0
}

compare_case() {
  local name="$1"
  local ref_log="${WORK}/${name}.C.log"
  local case_failed=0 port bin ref_status status log

  ref_status="$(cat "${WORK}/${name}.C.status")"

  while IFS='|' read -r port bin; do
    # Success is silent in every port, so anything on stdout is a bug on its
    # own, reference port included.
    if [[ -s "${WORK}/${name}.${port}.out" ]]; then
      echo "PARITY FAILURE: ${port} wrote to stdout on '${name}'" >&2
      cat "${WORK}/${name}.${port}.out" >&2
      case_failed=1
    fi

    [[ "${port}" == "C" ]] && continue

    status="$(cat "${WORK}/${name}.${port}.status")"
    log="${WORK}/${name}.${port}.log"

    if [[ "${CASE_MUST_FAIL[${name}]}" == "1" ]]; then
      if [[ "${status}" == "0" || "${ref_status}" == "0" ]]; then
        echo "PARITY FAILURE: ${port} exited ${status} and C exited" \
          "${ref_status} on '${name}'; both must reject it" >&2
        case_failed=1
      fi
    elif [[ "${status}" != "${ref_status}" ]]; then
      echo "PARITY FAILURE: ${port} exited ${status}, C exited ${ref_status}," \
        "on '${name}'" >&2
      case_failed=1
    fi

    # A failing run must say why, even though the wording is its own.
    if [[ "${status}" != "0" && ! -s "${WORK}/${name}.${port}.err" ]]; then
      echo "PARITY FAILURE: ${port} failed silently on '${name}'" >&2
      case_failed=1
    fi

    if [[ -e "${ref_log}" || -e "${log}" ]]; then
      if ! diff -u "${ref_log}" "${log}" >/dev/null 2>&1; then
        echo "PARITY FAILURE: ${port} log differs from C on '${name}'" >&2
        diff -u "${ref_log}" "${log}" >&2 || true
        case_failed=1
      fi
    fi
  done < <(binaries)

  if [[ "${case_failed}" != "0" ]]; then
    failed=1
    return
  fi

  local detail="exit ${ref_status}"
  if [[ -s "${ref_log}" ]]; then
    detail+=", $(wc -c <"${ref_log}" | tr -d ' ') bytes"
  else
    # A case that writes nothing and exits 0 would otherwise pass vacuously.
    detail+=", empty log"
  fi
  echo "parity ok: ${name} (${detail})"
}

main() {
  build

  # build() proves the build ran, not that it put the binaries where binaries()
  # says. A wrong path there would make every port fail to exec identically:
  # each exits 127, none creates a log file, so the diff is skipped and the
  # stderr check is satisfied by the shell's own "No such file" — every case
  # would report "parity ok ... empty log" while running nothing at all.
  local port bin
  while IFS='|' read -r port bin; do
    if [[ ! -x "${bin}" ]]; then
      echo "missing ${port} binary: ${bin}" >&2
      echo "the build succeeded but did not produce it; fix binaries()" >&2
      exit 1
    fi
  done < <(binaries)

  rm -rf "${WORK}"
  mkdir -p "${WORK}"
  if [[ "${KEEP}" == "0" ]]; then
    trap 'rm -rf "${WORK}"' EXIT
  fi

  # Stdin fixtures, written with printf so the exact bytes are visible here.
  printf 'a\nb\nc\n' >"${WORK}/lines.in"
  printf 'a\nb' >"${WORK}/no_final_newline.in"
  printf 'a\n\nb\n' >"${WORK}/blank_lines.in"
  printf 'a\r\nb\r\n' >"${WORK}/crlf.in"
  printf '' >"${WORK}/empty.in"

  # --- entries from arguments ---
  run_case basic - - @LOG@ "server started"
  run_case multi_message - - @LOG@ one two three
  run_case empty_message - - @LOG@ ""
  run_case embedded_newline - - @LOG@ "$(printf 'first\nsecond')"
  run_case unicode - - @LOG@ "héllo — 日本語 🚀"
  run_case leading_dash - - @LOG@ -- -not-an-option

  # Every level, all four appended to one file.
  local level
  for level in debug info warning error; do
    run_case levels - - --level "${level}" @LOG@ "at ${level}"
  done

  # An option after the positional must still parse as an option.
  run_case permuted_options - - @LOG@ first --level error second

  # --- formatting options ---
  # Attached values, which used to be a three-way divergence: the hand-rolled
  # C++ loop matched option tokens exactly and rejected them. CLI11 accepts them
  # the way getopt_long and clap always did, so they are a shared behavior now
  # and get cases to keep them one.
  run_case attached_level - - --level=error @LOG@ x
  run_case attached_short_level - - -lerror @LOG@ x

  run_case custom_delims - - -d ' | ' -s '\n\n' @LOG@ hi
  run_case tab_delimiter - - -d '\t' @LOG@ hi
  run_case escaped_backslash - - -d '\\' @LOG@ hi
  run_case no_timestamp - - --no-timestamp @LOG@ hi
  run_case no_level - - --no-level @LOG@ hi
  run_case no_fields - - --no-timestamp --no-level @LOG@ hi

  # --- entries from stdin ---
  run_case stdin_lines - "${WORK}/lines.in" @LOG@
  run_case stdin_no_final_newline - "${WORK}/no_final_newline.in" @LOG@
  run_case stdin_blank_lines - "${WORK}/blank_lines.in" @LOG@
  run_case stdin_crlf - "${WORK}/crlf.in" @LOG@
  run_case stdin_empty - "${WORK}/empty.in" @LOG@

  # An unreadable stdin must fail, not look like a clean end of input. A
  # directory is the easiest reliably-unreadable stream. This case exists
  # because C++ once exited 0 here: std::cin's stdio_sync_filebuf swallows the
  # error without setting badbit, so write_lines' in.bad() check never saw it.
  mkdir -p "${WORK}/unreadable_stdin"
  run_case stdin_read_error - "${WORK}/unreadable_stdin" @LOG@

  # --- append semantics ---
  run_case append_twice - - @LOG@ "first run"
  run_case append_twice - - @LOG@ "second run"

  # --- the clock ---
  run_case negative_time -1 - @LOG@ "before the epoch"
  run_case epoch_zero 0 - @LOG@ "the epoch"
  run_case far_future 2147483648 - @LOG@ "past the 2038 wrap"

  # --- failures: all ports must agree on the exit status ---
  run_case_parser_error bad_level - - --level warn @LOG@ x
  run_case_parser_error bad_escape - - -s '\q' @LOG@ x
  run_case_parser_error trailing_backslash - - -d '\' @LOG@ x
  run_case missing_logfile - -
  run_case empty_logfile - - "" x
  run_case_parser_error unknown_option - - --nope @LOG@ x
  run_case bad_fake_time nope - @LOG@ x

  # A directory can never be appended to, in any port.
  mkdir -p "${WORK}/a_directory"
  run_case unwritable_target - - "${WORK}/a_directory" x

  local name
  for name in "${CASES[@]}"; do
    compare_case "${name}"
  done

  if [[ "${failed}" != "0" ]]; then
    echo "" >&2
    echo "the ports have diverged; fix the offending port, not this script" >&2
    exit 1
  fi
  echo ""
  echo "all ${#CASES[@]} cases agree across C, C++, and Rust"
}

main
