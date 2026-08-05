# mini_shell (C)

A prototype shell: it prints a `$` prompt, reads one command per line from stdin, runs
it with `system()`, and reports the status of anything that did not exit 0. See the
top-level `mini_shell/README.md` for the full contract the later ports will share.

## Design

The package is split into a `shell` library and a thin CLI, with two seams that keep
the command loop testable:

- **A runner seam.** `shell_run` never calls `system()`. It calls
  `opts->runner(command, opts->runner_ctx)`, and `main` passes `shell_system_runner`,
  the one impure function here. The tests pass a recording fake that returns canned
  wait statuses, so the whole loop — prompting, trimming, `exit` detection, status
  reporting — is exercised without forking a single process. Same shape as the clock
  seam in `simple_logger` (`log_clock_resolve` pure, `log_clock_now` impure).
- **A stream seam.** `shell_run(in, out, err, opts)` takes three `FILE *` rather than
  reaching for `stdin`/`stdout`/`stderr`, so tests drive it with `fmemopen`. `err` is
  separate from `out` on purpose: it is the contract that the shell's own complaints
  never mix into the command's output.

`shell_decode_status` is **pure and separate from the reporting**, which is what makes
the signal case testable at all — arranging for a real process to be killed just to
check one branch would be a fork per assertion. It exists because the value `system()`
returns is a wait status, not an exit code: `WIFSIGNALED` is checked before
`WIFEXITED`, since reading the raw value as a code reports a command killed by
`SIGKILL` as "exited with status 0". `-1` is checked first of all and means the command
never ran.

The one test that really forks, `RealSystem.EncodingMatchesTheMacros`, exists to pin
the assumption the rest of the suite is built on: the `exited(code)`/`signaled(sig)`
helpers in `test_shell.c` construct wait statuses by hand, and if this libc laid them
out differently every hand-built status would be meaningless.

`shell_run` uses POSIX `getline`, which reports a byte count, so a line containing a
NUL arrives intact and can be **refused** rather than silently truncated —
`system()` takes a NUL-terminated string, so `echo a\0rm -rf /` would otherwise run as
`echo a`. This is the one place the port is deliberately not byte-transparent, and it
is the only honest option given the `system()` signature.

Two smaller details:

- The prompt is **flushed** before every read, and `stdin` is set **unbuffered** in
  `main`. The command inherits both of `out`'s and `in`'s file descriptors: an
  unflushed prompt would surface after the command's own output, and a buffered `in`
  would let stdio swallow a piped script whole before the first fork, handing `cat` an
  empty stdin. The `setvbuf` lives in `main` rather than `shell_run`, which takes a
  stream its caller owns — and note that no unit test can catch its absence, since the
  suite drives the loop through `fmemopen`.
- Errors are a `ShellResult` enum naming the stage that failed, with `errno` left in
  place by the libc-backed stages (preserved across `free` with the usual
  `int saved = errno` idiom) so `main` can pair the stage with `strerror`. A *command*
  failing is not one of these: it is a `ShellStatus`, reported per command, and it
  never ends the loop.

## Build & run

```sh
bazel run  //mini_shell/c:mini_shell
bazel test //mini_shell/c:test_shell

printf 'echo hi\nfalse\nexit\n' | bazel-bin/mini_shell/c/mini_shell
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so a command
using a relative path resolves somewhere surprising. Run
`bazel-bin/mini_shell/c/mini_shell` directly when that matters.

The program exits `0` whenever the loop ended cleanly, **whatever the commands did**;
`1` for its own failure (stdin read, stdout/stderr write, out of memory, no command
interpreter); and `2` for a usage error (unknown option, or any operand — there are
none).
