#!/usr/bin/env bash
#
# Regenerate the expected golden outputs.
#
# Runs every case through all three ports and refuses to write anything unless
# they all agree, so a bug in one port can never be silently baked into the
# expected output. Run after an intentional behavior change, then review the
# resulting diff.
#
#   ./regenerate.sh

set -euo pipefail

TESTDATA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${TESTDATA_DIR}/../.." && pwd)"

# The alternate config, which must stay in sync with kAltConfig / ALT_CONFIG in
# the three golden tests.
ALT_FLAGS=(--top-n 3 --max-word-len 5)

C_BIN="${REPO_ROOT}/bazel-bin/text_analyzer/c/text_analyzer"
CPP_BIN="${REPO_ROOT}/bazel-bin/text_analyzer/cpp/text_analyzer"
RUST_BIN="${REPO_ROOT}/target/release/text_analyzer"

build() {
  echo "building all three ports..." >&2
  (cd "${REPO_ROOT}" && bazel build //text_analyzer/c:text_analyzer \
    //text_analyzer/cpp:text_analyzer) >&2
  (cd "${REPO_ROOT}/text_analyzer/rust" && cargo build --release --quiet) >&2
}

# Runs the three ports on one case and echoes the agreed output, or fails.
agreed_output() {
  local input="$1"
  shift
  local flags=("$@")

  local c_out cpp_out rust_out
  c_out="$("${C_BIN}" "${flags[@]}" "${input}")"
  cpp_out="$("${CPP_BIN}" "${flags[@]}" "${input}")"
  rust_out="$("${RUST_BIN}" "${flags[@]}" "${input}")"

  if [[ "${c_out}" != "${cpp_out}" ]]; then
    echo "DISAGREEMENT on ${input} ${flags[*]}: C vs C++" >&2
    diff <(echo "${c_out}") <(echo "${cpp_out}") >&2 || true
    return 1
  fi
  if [[ "${c_out}" != "${rust_out}" ]]; then
    echo "DISAGREEMENT on ${input} ${flags[*]}: C vs Rust" >&2
    diff <(echo "${c_out}") <(echo "${rust_out}") >&2 || true
    return 1
  fi
  printf '%s\n' "${c_out}"
}

main() {
  build

  local cases=()
  while IFS= read -r line; do
    [[ -n "${line}" ]] && cases+=("${line}")
  done < "${TESTDATA_DIR}/cases.txt"

  if [[ "${#cases[@]}" -eq 0 ]]; then
    echo "error: cases.txt is empty" >&2
    exit 1
  fi

  # Verify every case first; write only once all of them agree, so a failure
  # partway through leaves the committed goldens untouched.
  local -A outputs=()
  local case input
  for case in "${cases[@]}"; do
    input="${TESTDATA_DIR}/${case}"
    if [[ ! -f "${input}" ]]; then
      echo "error: missing input ${input} (run ./make_inputs.sh)" >&2
      exit 1
    fi
    outputs["${case}.out"]="$(agreed_output "${input}")"
    outputs["${case}.json"]="$(agreed_output "${input}" --json)"
    outputs["${case}.alt.out"]="$(agreed_output "${input}" "${ALT_FLAGS[@]}")"
    outputs["${case}.alt.json"]="$(agreed_output "${input}" --json "${ALT_FLAGS[@]}")"
    echo "ok: ${case}" >&2
  done

  local name
  for name in "${!outputs[@]}"; do
    printf '%s\n' "${outputs[${name}]}" > "${TESTDATA_DIR}/${name}"
  done

  echo "wrote ${#outputs[@]} golden files for ${#cases[@]} cases" >&2
}

main "$@"
