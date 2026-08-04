# mini_shell

A prototype shell: it prints a `$` prompt, reads one command per line, hands it to the
system command interpreter with `system()`, and reports the status of anything that did
not exit 0. Only the C port (`c/`, Bazel) exists so far; C++ and Rust follow. The full
contract — the loop, the reporting format, edge cases, exit codes — is in
[`README.md`](README.md), and the C port has its own design notes in
[`c/README.md`](c/README.md).

## Commands

```sh
bazel run  //mini_shell/c:mini_shell

bazel test //mini_shell/c:test_shell
bazel test //mini_shell/c:all

printf 'echo hi\nfalse\nexit\n' | bazel-bin/mini_shell/c/mini_shell --no-banner
```

## Shared behavior (keep the ports in sync)

- **`system()`'s return value is a wait status, not an exit code.** It is decoded with
  `WIFSIGNALED`/`WTERMSIG` before `WIFEXITED`/`WEXITSTATUS`, and `-1` (the command never
  ran) is checked before either. Reading the raw value as a code is the bug this
  ordering prevents: it reports a command killed by `SIGKILL` as "exited with status 0".
  Any port that gets a decoded status from its standard library for free still has to
  produce the same three cases.
- **The decoding is pure and separate from the reporting** (`shell_decode_status` vs
  `shell_report_status`). That is what makes the signal branch testable without
  arranging for a real process to be killed, and it is why the C suite forks exactly
  once — `RealSystem.EncodingMatchesTheMacros`, which pins the hand-built wait-status
  encoding the other tests use against the libc actually linked in.
- **`system()` is behind a function-pointer seam** (`ShellRunner`), so the loop is
  tested with a recording fake and never forks. Each port needs the equivalent — a
  `std::function` in C++, a trait or `fn` in Rust — or its command loop becomes
  untestable.
- **The banner and prompt always print**, TTY or not. Do not add an `isatty` branch:
  it makes the output depend on how the program was invoked, which every port would
  then have to reproduce and every test override. `--no-banner` covers scripting.
- **The prompt is flushed before every read.** The command inherits stdout's file
  descriptor, so an unflushed prompt surfaces *after* the command's output.
- **A failed command never ends the loop and never changes the exit code.** The shell
  ran what it was asked to; `0` means the loop ended cleanly, `1` is the shell's own
  I/O failure, `2` is a usage error. Making the exit code the last command's status was
  considered and rejected: it collides with `1` meaning "the shell itself broke".
- **`exit` is the only builtin**, matched as the bare word after trimming ASCII
  whitespace. `EXIT`, `exitx`, and `exit 3` go to the interpreter. **`cd` is
  deliberately absent** — every command gets a fresh subshell, so `cd` appears to do
  nothing, and that is documented rather than papered over. One builtin invites the
  rest of them; the exercise is about `system()`.
- **A line containing a NUL is refused, not truncated.** `system()` takes a
  NUL-terminated string, so running `echo a\0rm -rf /` would run `echo a` and silently
  drop the rest. This is the one place the port is not byte-transparent, unlike
  `simple_logger`, and the `system()` signature is the reason.
- **End of input writes one closing newline; `exit` does not.** The prompt just written
  is the last thing on its line, so Ctrl-D leaves the cursor somewhere sane.
- **Status 127 is not special-cased.** The interpreter has already printed its own
  "not found"; the status line is the one thing mini_shell actually knows.

## Gotchas

- **ASCII art in a C string literal.** Every backslash in the banner must be doubled —
  the compiler reads `\_` as an unknown escape, and `-Werror` makes that fatal. A `??`
  pair would be a trigraph. The banner is a `static const char *const[]` written with
  `fputs`, not `printf`, so a `%` in a future banner is not a format specifier. Check
  all three before changing the art.
- **`bazel run` and relative paths.** `bazel run` executes from Bazel's runfiles
  directory, not your shell's cwd, so a command using a relative path resolves
  somewhere surprising. Run `bazel-bin/mini_shell/c/mini_shell` directly when that
  matters. (Same trap as `copy_file` and `simple_logger`.)
- **`valgrind` does not run on this machine.** Use sanitizers instead:
  ```sh
  bazel test //mini_shell/c:all --config=permissive \
    --copt=-fsanitize=address --copt=-g --linkopt=-fsanitize=address
  ```
- **Tests that assert on the prompt count are asserting on the loop shape.** `ls\nexit\n`
  produces `"$ $ "` — one prompt per line read, including the one `exit` answered. If a
  refactor changes when the prompt is written, those are the tests that will say so.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
