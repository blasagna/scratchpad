# mini_shell (C++)

A prototype shell: it prints a `$` prompt, reads one command per line from stdin, runs
it with `std::system`, and reports the status of anything that did not exit 0. Written
in idiomatic C++20 with the same semantics as the C port. See the top-level
`mini_shell/README.md` for the full contract.

## Why `std::system`

Because C++ has no other answer, and does not need one here. `std::system` is `<cstdlib>`'s
name for the same libc `system()` the C port calls; nothing in C++23/26 supersedes it,
and there is still no standard process-launching API. The alternatives are each a
different program:

- **`popen`/`pclose`** captures the command's stdout into a pipe. The contract says the
  command inherits mini_shell's stdout and stderr, so this changes what the program
  does, not how it is spelled.
- **`fork` + `execvp`, or `posix_spawn`**, is what a real shell does — and it skips the
  command interpreter, so `ls | wc -l`, `echo *`, and `exit 3` stop working. That is a
  different exercise; this one is about `system()`.
- **Boost.Process** would be a dependency, and the ports here take only CLI11.

Two things do not get easier in C++, and both are load-bearing:

- **The return value is still a raw wait status.** The standard calls it
  implementation-defined; POSIX makes it a wait status, so `decode_status` still includes
  `<sys/wait.h>` and still checks `WIFSIGNALED` before `WIFEXITED`. Reading the raw value
  as an exit code would report a command killed by `SIGKILL` as "exited with status 0".
- **`std::system(nullptr)`** is still the startup check for whether an interpreter exists
  at all.

## Design

Everything lives in `namespace shell`, with file-local helpers in an unnamed namespace.
**No exceptions on the failure paths**: they are returned as values, a
`Result { Stage stage; std::error_code ec; }` in the shape `simple_logger` uses.
`std::bad_alloc` is the one exception in play, and it is caught in `main.cpp` so that
running out of memory still prints `mini_shell: out of memory` and exits 1, as the C
port's `SHELL_ERR_NOMEM` does.

The port keeps both of the C port's seams:

- **A runner seam.** `run` never calls `std::system`. It calls `opts.runner(command)`,
  a `std::function<int(const std::string &)>`, and `main` passes `shell::system_runner`
  — the one impure function here. The tests bind a `Recorder` that returns canned wait
  statuses, so the whole loop is exercised without forking a single process.
- **A stream seam.** `run(in, out, err, opts)` takes three stream references rather than
  reaching for `std::cin`/`cout`/`cerr`, so the tests drive it with
  `std::istringstream` / `std::ostringstream` — which is most of why this suite is
  shorter than the C one, whose `fmemopen` scaffolding all collapses. `err` is separate
  from `out` on purpose: it is the contract that the shell's own complaints never mix
  into the command's output.

`decode_status` is pure and separate from `report_status`, which is what makes the
signal case testable without arranging for a real process to be killed.
`RealSystem.EncodingMatchesTheMacros` is the one test that really forks, and it pins the
hand-built wait statuses the rest of the suite uses against the libc actually linked in.

Idiom differences from the C port, beyond the namespace and `enum class` spellings:

- **`Options::show_banner` is a plain `bool`.** The C header spells it `int` and spends
  five lines explaining why — that header is compiled both as C17 and, for the tests, as
  C++, where `bool` is a different type. This header is only ever C++, so the hazard
  is gone rather than worked around.
- **`trim` returns a narrowed `std::string_view`** instead of mutating a
  `(const char **, size_t *)` pair, and the whitespace set is written out as
  `" \t\n\v\f\r"` rather than delegated to `std::isspace`, which is locale-dependent in
  principle.
- **`std::getline` into a `std::string`** replaces POSIX `getline`. It carries its own
  length for the same reason: a line with an embedded NUL arrives intact and is
  **refused** rather than silently truncated, since `std::system` takes a
  NUL-terminated string and `echo a\0rm -rf /` would otherwise run as `echo a`. It also
  removes the manual `free` and the `int saved = errno` dance around it.
- **`Result::ec` is captured at the point of failure** instead of leaving `errno` in
  place for `main` to pair with `strerror`.
- `std::getline` consumes the `'\n'` itself, so only the single trailing `'\r'` is
  stripped — the C port strips both, in that order, and the two agree.

`main.cpp` parses options with CLI11, which owns `--help` and rejects unknown options
and stray operands on its own, with its own wording and its own exit codes. The
`--no-banner` flag uses CLI11's negated-flag spelling, `"!--no-banner"`, the same one
`simple_logger` uses for `--no-timestamp`.

Two details in `main.cpp` are worth not undoing:

- **`std::setvbuf(stdin, nullptr, _IONBF, 0)`** reaches `std::cin` because
  `sync_with_stdio` is true by default, which makes `std::cin` read through that very
  `FILE *`. **Never call `std::ios::sync_with_stdio(false)` here**: it installs a
  `filebuf` with its own read-ahead, and `printf 'cat\necho done\n' | mini_shell` goes
  back to handing `cat` an empty stdin. No unit test can catch it — the suite drives
  `run` with a string stream, where buffering is invisible — so `check_parity.sh` asserts
  it directly.
- **The `std::ferror(stdin)` check** after `run` returns. `std::cin` is backed by a
  `stdio_sync_filebuf` whose `underflow()` returns EOF on error without setting
  `badbit`, so a failed read is indistinguishable from a clean end of input at the
  iostream level. Without it, unreadable stdin would exit 0. (Same fixup, same reason,
  as `simple_logger/cpp/main.cpp`.)

## Build & run

```sh
bazel run  //mini_shell/cpp:mini_shell
bazel test //mini_shell/cpp:test_shell

printf 'echo hi\nfalse\nexit\n' | bazel-bin/mini_shell/cpp/mini_shell
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so a command
using a relative path resolves somewhere surprising. Run
`bazel-bin/mini_shell/cpp/mini_shell` directly when that matters.

The program exits `0` whenever the loop ended cleanly, **whatever the commands did**;
`1` for its own failure (stdin read, stdout/stderr write, out of memory, no command
interpreter); and, for a bad command line, whatever CLI11 returns — `109` for an unknown
option or a stray operand, where the C port exits `2`. See the divergence table in
`mini_shell/README.md`.
