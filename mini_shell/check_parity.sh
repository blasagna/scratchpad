#!/usr/bin/env bash
#
# Build the C, C++, and Rust mini_shell ports, feed each the same scripted
# stdin, and check they agree byte for byte.
#
# Every port forks and execs one program per line, so there is no /bin/sh in
# any of them to normalize away differences: the splitting, the PATH lookup's
# outcome, and the wording of every failure are each port's own code. That is
# what makes these comparisons worth running.
#
#   ./mini_shell/check_parity.sh          # build, run every case, report
#   ./mini_shell/check_parity.sh --keep   # keep the work dir for inspection
#
# The C port is the reference; every other port is diffed against it. What is
# compared depends on who reports the outcome:
#
#   run_case              — stdout, stderr, and exit status. The default, and
#                           where the contract lives. Unlike matrix_ops, stderr
#                           IS compared here: the per-command status lines are
#                           the reporting contract, every port writes the same
#                           bytes, and a case that only checked stdout would
#                           check nothing at all for a failing command.
#   run_case_parser_error — stdout only, plus "every port must fail". These are
#                           the failures the argument parser reports, and the
#                           three ports do not share one: C uses getopt_long,
#                           C++ uses CLI11, Rust uses clap, so neither the
#                           wording nor the exit code is shared. clap does land
#                           on C's exit 2, but that is a coincidence rather than
#                           a contract. What still has to hold is that the same
#                           command line is rejected by all of them.
#   run_case_status_only  — exit status only, for --help, whose text is CLI11's
#                           on the C++ side and clap's on the Rust one.
#
# One case does not fit the "the ports agree" mold and is checked absolutely
# instead, by check_unbuffered below: a shell must not read ahead past the
# command it is about to run. Every port could regress together and every
# comparison here would still pass.
#
# Builds are unoptimized: there are no timings here.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/mini_shell_parity"
# Two cases below put a fixture's path inside a command line, and the splitter
# has no quoting: whitespace anywhere in this path would break the path into
# several words, and both cases would degrade into "command not found" — in all
# three ports at once, so the comparison would still print ok while asserting
# nothing about signals or EACCES. Do not trust TMPDIR to be free of it.
if [[ "${WORK}" == *[[:space:]]* ]]; then
  echo "note: TMPDIR contains whitespace; using /tmp for fixtures" >&2
  WORK="/tmp/mini_shell_parity"
fi

KEEP=0
if [[ "${1:-}" == "--keep" ]]; then
  KEEP=1
elif [[ $# -gt 0 ]]; then
  echo "usage: check_parity.sh [--keep]" >&2
  exit 2
fi

failed=0
CASES=()

# Per-case comparison policy, filled in by run_case from the two variables
# below, so the case list itself records which cases are parser-owned.
declare -A CASE_STREAMS
declare -A CASE_MUST_FAIL

# Streams the next run_case compares, and whether it only requires that every
# port failed rather than that they failed alike. Set by the run_case_* wrappers.
STREAMS="out err status"
MUST_FAIL=0

# Ports under test, reference first.
binaries() {
  echo "C|${REPO_ROOT}/bazel-bin/mini_shell/c/mini_shell"
  echo "C++|${REPO_ROOT}/bazel-bin/mini_shell/cpp/mini_shell"
  echo "Rust|${REPO_ROOT}/target/debug/mini_shell"
}

build() {
  echo "building..." >&2
  (cd "${REPO_ROOT}" && bazel build \
    //mini_shell/c:mini_shell //mini_shell/cpp:mini_shell) >&2
  (cd "${REPO_ROOT}" && cargo build --quiet -p mini_shell) >&2
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
run_case() {
  local name="$1" stdin_file="$2"
  shift 2

  if [[ "${stdin_file}" == "-" ]]; then
    stdin_file="/dev/null"
  else
    stdin_file="${WORK}/scripts/${stdin_file}"
  fi
  CASES+=("${name}")
  CASE_STREAMS["${name}"]="${STREAMS}"
  CASE_MUST_FAIL["${name}"]="${MUST_FAIL}"

  local port bin status
  while IFS='|' read -r port bin; do
    status=0
    "${bin}" "$@" \
      >"${WORK}/${name}.${port}.out" \
      2>"${WORK}/${name}.${port}.err" \
      <"${stdin_file}" || status=$?
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

# run_case_parser_error <name> <stdin-file|-> <args...>
#
# For a command line the argument parser rejects. getopt_long reports it in the
# C port and CLI11 in the C++ one, so the message and the exit code are each
# parser's own; what is asserted is that stdout agrees (empty in both) and that
# every port rejected it.
run_case_parser_error() {
  STREAMS="out"
  MUST_FAIL=1
  run_case "$@"
  STREAMS="out err status"
  MUST_FAIL=0
}

# run_case_status_only <name> <stdin-file|-> <args...>
#
# For --help, whose text the C port hand-writes and the C++ port gets from
# CLI11. Only the exit status is compared.
run_case_status_only() {
  STREAMS="status"
  run_case "$@"
  STREAMS="out err status"
}

compare() {
  local name port bin ref_port ref_bin
  IFS='|' read -r ref_port ref_bin < <(binaries)

  for name in ${CASES[@]+"${CASES[@]}"}; do
    local case_failed=0

    # A parser-owned case asserts only that every port rejected the command
    # line, the reference included.
    if [[ "${CASE_MUST_FAIL[${name}]}" == "1" ]]; then
      while IFS='|' read -r port bin; do
        if [[ "$(cat "${WORK}/${name}.${port}.status")" == "0" ]]; then
          echo "PARITY FAILURE: ${name}: ${port} exited 0, expected a rejection" >&2
          case_failed=1
          failed=1
        fi
      done < <(binaries)
    fi

    while IFS='|' read -r port bin; do
      [[ "${port}" == "${ref_port}" ]] && continue

      local stream
      for stream in ${CASE_STREAMS[${name}]}; do
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

# The one absolute assertion here, because agreement cannot express it.
#
# A shell must not read ahead past the command it is about to run: `cat` has to
# receive the input mini_shell has not consumed yet. Buffered, stdio pulls the
# whole pipe in before the fork, `cat` gets an empty stdin, and mini_shell
# runs the second line itself -- so the output says "done" instead of the
# "echo done" that cat echoed. Every port could regress together and every diff
# above would still be clean, which is why this is checked against a fixed
# expectation rather than against the reference port.
#
# This is also the one behavior no unit suite can reach: every suite drives the
# loop with an in-memory stream, where buffering is invisible. (The Rust port
# additionally pins it from cargo test, in rust/tests/cli.rs, by spawning the
# binary -- which is the same trick as this, not a unit test.)
check_unbuffered() {
  local port bin out
  while IFS='|' read -r port bin; do
    out="$(printf 'cat\necho done\n' | "${bin}" --no-banner 2>/dev/null)"
    if [[ "${out}" != *"echo done"* ]]; then
      echo "PARITY FAILURE: stdin_passthrough: ${port} read ahead;" \
           "cat never saw the second line" >&2
      printf 'got: %q\n' "${out}" >&2
      failed=1
    else
      echo "ok: stdin_passthrough (${port})" >&2
    fi
  done < <(binaries)
}

make_scripts() {
  local dir="${WORK}/scripts"
  mkdir -p "${dir}"

  printf 'echo hi\nexit\n'                    >"${dir}/basic.txt"
  printf 'echo one\necho two\nfalse\necho three\nexit\n' >"${dir}/several.txt"
  printf 'false\nexit\n'                      >"${dir}/failing.txt"
  printf '\n   \n\t\nls /dev/null\nexit\n'    >"${dir}/blank.txt"
  printf '  exit  \n'                         >"${dir}/exit_spaced.txt"
  # Every spelling but the bare word is a command like any other, and none of
  # them is a program on this machine -- `exit 3` included, now that there is no
  # subshell to exit.
  printf 'exitx\nEXIT\nexit 3\nexit\n'        >"${dir}/exit_variants.txt"
  printf 'echo a\r\nexit\r\n'                 >"${dir}/crlf.txt"
  printf 'echo tail'                          >"${dir}/no_newline.txt"
  printf 'echo hi\n'                          >"${dir}/eof.txt"
  printf ''                                   >"${dir}/empty.txt"
  printf 'nosuchcommand_xyzzy\nexit\n'        >"${dir}/notfound.txt"
  printf 'echo \xc3\xa9\nexit\n'              >"${dir}/nonascii.txt"
  # A NUL inside a command line. execvp takes NUL-terminated strings, so every
  # port refuses the line rather than run the truncated "echo a".
  printf 'echo a\000rm -rf /\nexit\n'         >"${dir}/nul.txt"
  # cd is not a program, so it is not found. pwd is one, and runs.
  printf 'cd /\npwd\nexit\n'                  >"${dir}/cd.txt"

  # A program and its arguments, and the whitespace between them: runs collapse,
  # so echo receives exactly three arguments and prints them space-separated.
  printf 'echo one two three\nexit\n'         >"${dir}/args.txt"
  printf 'echo   a  \t b\nexit\n'             >"${dir}/spacing.txt"
  # Nothing splits or expands these, so echo prints them back verbatim. This is
  # the whole of what was given up with /bin/sh, pinned as a behavior.
  printf 'echo a | wc\necho * $HOME > out\nexit\n' >"${dir}/metachars.txt"

  # Two fixtures, because neither case can be spelled in a command line any
  # more. A command killed by a signal needs a program that kills itself, and
  # `kill -9 $$` at the prompt would now run /usr/bin/kill with the literal
  # argument "$$".
  printf '#!/bin/sh\nkill -9 $$\n'             >"${dir}/suicide.sh"
  chmod +x "${dir}/suicide.sh"
  printf '%s\nexit\n' "${dir}/suicide.sh"      >"${dir}/signal.txt"

  # A file that exists and is not executable: execvp reports EACCES, and the
  # port turns that into its own "permission denied" line.
  printf 'not a program\n'                     >"${dir}/not_executable"
  printf '%s\nexit\n' "${dir}/not_executable" >"${dir}/noexec.txt"
}

main() {
  build
  check_binaries

  rm -rf "${WORK}"
  mkdir -p "${WORK}"
  make_scripts

  # --- the loop ---
  run_case basic          basic.txt          --no-banner
  run_case several        several.txt        --no-banner
  run_case banner         basic.txt
  run_case blank_lines    blank.txt          --no-banner
  run_case no_newline     no_newline.txt     --no-banner
  run_case end_of_input   eof.txt            --no-banner
  run_case empty_input    empty.txt          --no-banner
  run_case closed_stdin   -                  --no-banner

  # --- exit, and the spellings that are not it ---
  run_case exit_spaced    exit_spaced.txt    --no-banner
  run_case exit_variants  exit_variants.txt  --no-banner

  # --- splitting, which is the whole grammar ---
  run_case arguments      args.txt           --no-banner
  run_case whitespace_runs spacing.txt       --no-banner
  run_case metacharacters metachars.txt      --no-banner

  # --- status reporting ---
  run_case failing        failing.txt        --no-banner
  run_case signaled       signal.txt         --no-banner
  run_case not_found      notfound.txt       --no-banner
  run_case not_executable noexec.txt         --no-banner

  # --- byte handling ---
  run_case crlf           crlf.txt           --no-banner
  run_case non_ascii      nonascii.txt       --no-banner
  run_case nul_byte       nul.txt            --no-banner

  # --- documented non-behavior ---
  # cd is not a builtin and is not a program either, so it is reported as
  # missing; pwd still prints the directory mini_shell was started in.
  run_case cd_is_not_a_builtin cd.txt        --no-banner

  # --- option handling ---
  run_case_status_only  help          -  --help
  run_case_parser_error unknown_opt   -  --nope
  run_case_parser_error unknown_short -  -z
  run_case_parser_error stray_operand -  ls
  # getopt_long accepts an abbreviated long option and neither CLI11 nor clap
  # does, so this is not a shared spelling either -- but no port may silently
  # ignore it. Only the C port accepts it, so there is nothing to assert
  # jointly; see the divergence table in README.md.

  compare
  check_unbuffered

  if [[ "${failed}" == "0" ]]; then
    echo "all ${#CASES[@]} cases agree, and every port reads unbuffered" >&2
  else
    echo "PARITY CHECK FAILED" >&2
  fi

  if [[ "${KEEP}" == "1" ]]; then
    echo "work dir kept at ${WORK}" >&2
  else
    rm -rf "${WORK}"
  fi
  return "${failed}"
}

main
