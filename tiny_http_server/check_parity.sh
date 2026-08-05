#!/usr/bin/env bash
#
# Build the C and C++ tiny_http_server ports, drive each with the same requests
# over a real socket, and check they agree byte for byte.
#
# Nothing here is mocked: each case starts a real server on an ephemeral port,
# opens a real TCP connection to it, and compares the bytes that came back. The
# response is the contract, so it is compared unnormalized.
#
#   ./tiny_http_server/check_parity.sh          # build, run every case, report
#   ./tiny_http_server/check_parity.sh --keep   # keep the work dir for inspection
#
# The C port is the reference; every other port is diffed against it. What is
# compared depends on who reports the outcome:
#
#   serve_case         — the response bytes, the log, and the exit status. The
#                        default, and where the contract lives. The log IS
#                        compared, because the event lines are part of the
#                        contract and every port writes the same bytes - after
#                        one normalization, below.
#   startup_case       — the log and the exit status, for a server that never
#                        gets as far as listening (an unreadable --file).
#   parser_error_case  — stdout only, plus "every port must fail". These are the
#                        failures the argument parser reports, and the ports do
#                        not share one: C uses getopt_long and exits 2, C++ uses
#                        CLI11 and brings its own wording and its own code. What
#                        still has to hold is that the same command line is
#                        rejected by all of them. See the divergence table in
#                        README.md.
#   help_case          — exit status only, for --help, whose text the C port
#                        hand-writes and the C++ port gets from CLI11.
#
# The one normalization: every port binds --port 0 so a case never collides with
# a server left running in another terminal, and the kernel's choice then shows
# up in "listening on 127.0.0.1:<port>" and the client's source port shows up in
# "connection from 127.0.0.1:<port>". Both are replaced with :PORT before the
# diff. Nothing else is touched.
#
# Two cases do not fit the "the ports agree" mold and are checked absolutely
# instead, by check_drain and check_ephemeral_port below. Every port could
# regress together and every comparison here would still pass.
#
# Builds are unoptimized: there are no timings here.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/tiny_http_server_parity"

KEEP=0
if [[ "${1:-}" == "--keep" ]]; then
  KEEP=1
elif [[ $# -gt 0 ]]; then
  echo "usage: check_parity.sh [--keep]" >&2
  exit 2
fi

failed=0
CASES=()

# Per-case comparison policy, filled in by the run helpers from the two
# variables below, so the case list itself records which cases are parser-owned.
declare -A CASE_STREAMS
declare -A CASE_MUST_FAIL

STREAMS="out log status"
MUST_FAIL=0

# Ports under test, reference first. Rust joins this list when it lands.
binaries() {
  echo "C|${REPO_ROOT}/bazel-bin/tiny_http_server/c/tiny_http_server"
  echo "C++|${REPO_ROOT}/bazel-bin/tiny_http_server/cpp/tiny_http_server"
}

build() {
  echo "building..." >&2
  (cd "${REPO_ROOT}" && bazel build \
    //tiny_http_server/c:tiny_http_server \
    //tiny_http_server/cpp:tiny_http_server) >&2
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

# The client. Raw bytes in, raw bytes out, with no HTTP library between the
# request and the socket - curl would normalize a malformed request line before
# it ever reached the server, which is most of what is being tested here.
#
# It shuts down its write side after sending so the server's drain sees end of
# input rather than waiting out the receive timeout. A real client closing after
# its request does the same thing; without it the oversized-header case takes
# five seconds in every port.
make_client() {
  cat >"${WORK}/client.py" <<'PY'
import socket, sys

port = int(sys.argv[1])
with open(sys.argv[2], "rb") as f:
    request = f.read()

s = socket.create_connection(("127.0.0.1", port), timeout=15)
s.sendall(request)
s.shutdown(socket.SHUT_WR)
chunks = []
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    chunks.append(chunk)
s.close()
sys.stdout.buffer.write(b"".join(chunks))
PY
}

# Reads the port out of a server's log once it has bound, or fails after a few
# seconds. --port 0 is what makes this necessary and is worth the trouble: a
# fixed port collides with the server somebody left running, which is precisely
# when they are running this script.
wait_for_port() {
  local log="$1" i port
  for ((i = 0; i < 200; i++)); do
    port="$(sed -n 's/^tiny_http_server: listening on 127\.0\.0\.1:\([0-9]*\)$/\1/p' \
      "${log}" 2>/dev/null | head -1)"
    if [[ -n "${port}" ]]; then
      echo "${port}"
      return 0
    fi
    sleep 0.05
  done
  return 1
}

record_case() {
  CASES+=("$1")
  CASE_STREAMS["$1"]="${STREAMS}"
  CASE_MUST_FAIL["$1"]="${MUST_FAIL}"
}

# serve_case <name> <request-file> [server args...]
#
# Starts each port with --port 0 --once, sends the request bytes, and keeps the
# response, the log, and the exit status.
#
# Every case here must be one the server answers, because --once stops after an
# answered request: a case the server does not answer would leave it running
# until this script's timeout. The cases that are deliberately unanswered - a
# client that sends nothing, one that hangs up mid-header - are covered by the
# unit suites instead.
serve_case() {
  local name="$1" request="${WORK}/requests/$2"
  shift 2
  record_case "${name}"

  local port bin status server_port pid
  while IFS='|' read -r port bin; do
    status=0
    : >"${WORK}/${name}.${port}.log"
    timeout 30 "${bin}" --host 127.0.0.1 --port 0 --once "$@" \
      >"${WORK}/${name}.${port}.stdout" \
      2>"${WORK}/${name}.${port}.log" &
    pid=$!

    if server_port="$(wait_for_port "${WORK}/${name}.${port}.log")"; then
      echo "${server_port}" >"${WORK}/${name}.${port}.port"
      python3 "${WORK}/client.py" "${server_port}" "${request}" \
        >"${WORK}/${name}.${port}.out" 2>/dev/null || true
    else
      echo "PARITY FAILURE: ${name}: ${port} never reported a listening port" >&2
      failed=1
      : >"${WORK}/${name}.${port}.out"
      echo 0 >"${WORK}/${name}.${port}.port"
    fi

    wait "${pid}" || status=$?
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

# startup_case <name> [server args...]
#
# For a server that fails before it can listen, so there is no client and no
# response - only what it said and what it exited with.
startup_case() {
  local name="$1"
  shift
  STREAMS="log status"
  record_case "${name}"
  STREAMS="out log status"

  local port bin status
  while IFS='|' read -r port bin; do
    status=0
    timeout 30 "${bin}" "$@" \
      >"${WORK}/${name}.${port}.stdout" \
      2>"${WORK}/${name}.${port}.log" || status=$?
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

# parser_error_case <name> [server args...]
#
# For a command line the argument parser rejects. getopt_long reports it in the
# C port and CLI11 in the C++ one, so the message and the exit code are each
# parser's own; what is asserted is that stdout agrees (empty in both) and that
# every port rejected it.
parser_error_case() {
  local name="$1"
  shift
  STREAMS="stdout"
  MUST_FAIL=1
  record_case "${name}"
  STREAMS="out log status"
  MUST_FAIL=0

  local port bin status
  while IFS='|' read -r port bin; do
    status=0
    timeout 30 "${bin}" "$@" \
      >"${WORK}/${name}.${port}.stdout" \
      2>"${WORK}/${name}.${port}.log" || status=$?
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

# help_case <name> [server args...]
#
# Only the exit status is compared: the help text is the C port's own and
# CLI11's respectively.
help_case() {
  local name="$1"
  shift
  STREAMS="status"
  record_case "${name}"
  STREAMS="out log status"

  local port bin status
  while IFS='|' read -r port bin; do
    status=0
    timeout 30 "${bin}" "$@" \
      >"${WORK}/${name}.${port}.stdout" \
      2>"${WORK}/${name}.${port}.log" || status=$?
    echo "${status}" >"${WORK}/${name}.${port}.status"
  done < <(binaries)
}

# The ephemeral port the kernel chose, and the client's source port, are the
# only bytes in the log that cannot agree between two runs of the same binary.
normalize_logs() {
  local f
  for f in "${WORK}"/*.log; do
    [[ -e "${f}" ]] || continue
    sed -E 's/(127\.0\.0\.1):[0-9]+/\1:PORT/g' "${f}" >"${f}.n"
    mv "${f}.n" "${f}"
  done
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

# The first absolute assertion, because agreement cannot express it.
#
# A request body is deliberately never read, and Linux sends an RST rather than a
# FIN when a socket is closed with unread inbound data - so a bare close makes the
# peer throw away the 405 it already received. Every port could lose its shutdown
# and drain together, and every diff above would still be clean, because both
# would return nothing.
check_drain() {
  local port bin body
  while IFS='|' read -r port bin; do
    body="$(cat "${WORK}/post_with_body.${port}.out")"
    if [[ "${body}" != "HTTP/1.1 405 Method Not Allowed"* ]]; then
      echo "PARITY FAILURE: drain: ${port} did not deliver the 405 for a" \
           "request with an unread body" >&2
      printf 'got: %q\n' "${body}" >&2
      failed=1
    else
      echo "ok: drain (${port})" >&2
    fi
  done < <(binaries)
}

# The second one. --port 0 has to report the port the kernel really chose, and
# the normalization above would happily hide a port that stayed 0 in every port
# at once.
check_ephemeral_port() {
  local port bin chosen
  while IFS='|' read -r port bin; do
    chosen="$(cat "${WORK}/root.${port}.port")"
    if [[ "${chosen}" == "0" ]]; then
      echo "PARITY FAILURE: ephemeral_port: ${port} reported port 0 rather than" \
           "the one it bound" >&2
      failed=1
    else
      echo "ok: ephemeral_port (${port} bound ${chosen})" >&2
    fi
  done < <(binaries)
}

make_requests() {
  local dir="${WORK}/requests"
  mkdir -p "${dir}"

  printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n'              >"${dir}/root"
  printf 'GET /index.html HTTP/1.1\r\n\r\n'               >"${dir}/index"
  printf 'GET /?a=1 HTTP/1.1\r\n\r\n'                     >"${dir}/query"
  printf 'GET /favicon.ico HTTP/1.1\r\n\r\n'              >"${dir}/favicon"
  printf 'GET /nope HTTP/1.1\r\n\r\n'                     >"${dir}/not_found"
  printf 'HEAD / HTTP/1.1\r\n\r\n'                        >"${dir}/head"
  printf 'POST / HTTP/1.1\r\n\r\n'                        >"${dir}/post"
  # What `curl -d x` sends. The body is never read, which is the drain case.
  printf 'POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello' \
                                                          >"${dir}/post_body"
  # nc, telnet, and every hand-written script send lone LFs.
  printf 'GET / HTTP/1.1\n\n'                             >"${dir}/lone_lf"
  # RFC 7230 3.5's stray CRLF from a previous request.
  printf '\r\nGET / HTTP/1.1\r\n\r\n'                     >"${dir}/leading_blank"
  # HTTP/0.9, four fields, and a version token that says nothing: all 400.
  printf 'GET /\r\n\r\n'                                  >"${dir}/two_field"
  printf 'GET /a b HTTP/1.1\r\n\r\n'                      >"${dir}/four_field"
  printf 'GET / HTTP/1\r\n\r\n'                           >"${dir}/bad_version"
  # A NUL is refused rather than truncated into a valid request line.
  printf 'GET /\000x HTTP/1.1\r\n\r\n'                    >"${dir}/nul_byte"
  # An HTTP/2 preface: 505 and not 405, which is the status precedence.
  printf 'PRI * HTTP/2.0\r\n\r\n'                         >"${dir}/http2_preface"
  printf 'GET / HTTP/1.9\r\n\r\n'                         >"${dir}/minor_version"
  printf 'get / HTTP/1.1\r\n\r\n'                         >"${dir}/lowercase_get"
  # Attacker-controlled bytes on the way to somebody's terminal: the ESC has to
  # be gone from the log, and the newline must not forge a second line.
  printf 'GET /\033[2J\012x HTTP/1.1\r\n\r\n'             >"${dir}/escape_target"
  # No 414: a long target parses and 404s like any other unknown path.
  { printf 'GET /'; printf 'a%.0s' {1..4000}; printf ' HTTP/1.1\r\n\r\n'; } \
                                                          >"${dir}/long_target"
  # Over 8 KiB with no blank line: the one read failure whose client is still
  # there to be answered, with a 431.
  { printf 'GET / HTTP/1.1\r\nX-Pad: '; printf 'a%.0s' {1..10000}; } \
                                                          >"${dir}/oversize"

  # --file fixtures.
  printf '<p>from --file</p>\n'  >"${WORK}/page.html"
  : >"${WORK}/empty.html"
  head -c 1048577 /dev/zero | tr '\0' 'x' >"${WORK}/huge.html"
}

main() {
  build
  check_binaries

  rm -rf "${WORK}"
  mkdir -p "${WORK}"
  make_requests
  make_client

  # --- the ordinary answers ---
  serve_case root           root
  serve_case index_html     index
  serve_case query_string   query
  serve_case favicon        favicon
  serve_case not_found      not_found
  serve_case head           head
  serve_case long_target    long_target

  # --- methods and versions, in precedence order ---
  serve_case post           post
  serve_case lowercase_get  lowercase_get
  serve_case http2_preface  http2_preface
  serve_case minor_version  minor_version

  # --- malformed request lines ---
  serve_case two_field      two_field
  serve_case four_field     four_field
  serve_case bad_version    bad_version
  serve_case nul_byte       nul_byte

  # --- line endings and byte handling ---
  serve_case lone_lf        lone_lf
  serve_case leading_blank  leading_blank
  serve_case escape_target  escape_target

  # --- the two connection-shape cases ---
  serve_case post_with_body post_body
  serve_case oversize       oversize

  # --- --file ---
  serve_case file_page       root  --file "${WORK}/page.html"
  serve_case file_empty      root  --file "${WORK}/empty.html"

  # --- startup failures ---
  startup_case file_missing    --file "${WORK}/nope.html"
  startup_case file_directory  --file "${WORK}"
  startup_case file_too_large  --file "${WORK}/huge.html"
  # Binding a privileged port is the kernel's refusal, not the parser's, and it
  # is only a refusal for a non-root user.
  if [[ "${EUID}" != "0" ]]; then
    startup_case port_privileged --port 80
  else
    echo "note: running as root; skipping the privileged-port case" >&2
  fi

  # --- option handling ---
  help_case         help          --help
  parser_error_case port_abc      --port abc
  parser_error_case port_negative --port -1
  parser_error_case port_too_big  --port 65536
  parser_error_case host_name     --host localhost
  parser_error_case unknown_opt   --nope
  parser_error_case stray_operand extra
  # --port 0x1F90 and --port 8_080 are NOT here: CLI11 reads base 0 and strips
  # group separators, so the C++ port accepts spellings the C one rejects. That
  # is the documented divergence, not something to assert jointly.

  normalize_logs
  compare
  check_drain
  check_ephemeral_port

  if [[ "${failed}" == "0" ]]; then
    echo "all ${#CASES[@]} cases agree, every port delivers the 405 past an" \
         "unread body, and every port binds a real ephemeral port" >&2
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
