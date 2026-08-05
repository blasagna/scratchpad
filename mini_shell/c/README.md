# mini_shell (C)

A prototype shell: it prints a `$` prompt, reads one command per line from stdin,
splits it into a program and its arguments, runs that program with `fork` + `execvp` +
`waitpid`, and reports the status of anything that did not exit 0. See the top-level
`mini_shell/README.md` for the full contract this port shares with `cpp/` and `rust/`,
including why it does not use `system()` as the exercise says.

## Design

The package is split into a `shell` library and a thin CLI, with two seams that keep
the command loop testable:

- **A runner seam.** `shell_run` never forks. It calls
  `opts->runner(argv, opts->runner_ctx)`, and `main` passes `shell_exec_runner`, the one
  impure function here. The tests pass a recording fake that returns canned wait
  statuses, so the whole loop — prompting, splitting, `exit` detection, status reporting
  — is exercised without forking a single process. Same shape as the clock seam in
  `simple_logger` (`log_clock_resolve` pure, `log_clock_now` impure).
- **A stream seam.** `shell_run(in, out, err, opts)` takes three `FILE *` rather than
  reaching for `stdin`/`stdout`/`stderr`, so tests drive it with `fmemopen`. `err` is
  separate from `out` on purpose: it is the contract that the shell's own complaints
  never mix into the command's output.

`shell_decode_status(raw, err)` is **pure and separate from the reporting**, which is
what makes the signal case testable at all — arranging for a real process to be killed
just to check one branch would be a fork per assertion. It takes the errno as an
argument rather than reading the global, so the "never started" branches are testable
too. `WIFSIGNALED` is checked before `WIFEXITED`, since reading a wait status as a code
reports a command killed by `SIGKILL` as "exited with status 0"; `-1` is checked first
of all and means the command never started.

## The errno pipe

`execvp` fails **in the child**, where the parent cannot see its `errno`, so
`shell_exec_runner` joins the two with a pipe whose write end is `FD_CLOEXEC`:

- exec succeeded → the kernel closes the write end → the parent's `read` returns `0`;
- exec failed → the child writes the errno and `_exit`s → the parent reads four bytes.

That single bit is the difference between `mini_shell: foo: command not found` and a
guess. The alternative — `_exit(127)` and let the parent infer it from the wait status
— makes a command that really exits 127 indistinguishable from one that never ran, and
is what the `system()` version was stuck with. Two details are load-bearing: the child
`_exit`s rather than `exit`s, because it shares the parent's stdio buffers and flushing
them would print the parent's pending output twice; and the pipe is `pipe` +
`fcntl(F_SETFD, FD_CLOEXEC)` rather than `pipe2`, which sits behind `__USE_GNU` in
glibc and would need `_GNU_SOURCE`.

The one test that really forks, `RealExec`, pins both halves: the assumption the rest of
the suite is built on (the `exited(code)`/`signaled(sig)` helpers in `test_shell.c`
construct wait statuses by hand, and if this libc laid them out differently every
hand-built status would be meaningless), and that a missing program and a
non-executable one arrive as `ENOENT` and `EACCES` through the pipe.

`shell_split` is the other pure function, and it is the whole grammar: a run of ASCII
whitespace separates two words, every other byte is literal. It splits the line **in
place**, overwriting the whitespace with NULs so the words are already the
NUL-terminated strings `execvp` wants, and allocates only the pointer array — counted in
a first pass so there is no growing `realloc`.

`shell_run` uses POSIX `getline`, which reports a byte count, so a line containing a
NUL arrives intact and can be **refused** rather than silently truncated — `execvp`
takes NUL-terminated strings, so `echo a\0rm -rf /` would otherwise run as `echo a`.
This is the one place the port is deliberately not byte-transparent.

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
`1` for its own failure (stdin read, stdout/stderr write, out of memory); and `2` for a
usage error (unknown option, or any operand — there are none). A command that could not
be found or could not be run is **not** one of those: the shell did its job by trying.
