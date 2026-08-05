# mini_shell

A prototype shell: it prints a `$` prompt, reads one command per line, hands it to the
system command interpreter with `system()`, and reports the status of anything that did
not exit 0. Two ports exist: `c/` and `cpp/`, both Bazel; Rust follows. The full
contract — the loop, the reporting format, edge cases, exit codes, and the handful of
places the two ports deliberately differ — is in [`README.md`](README.md), and each port
has its own design notes ([c](c/README.md), [cpp](cpp/README.md)).

## Commands

```sh
bazel run  //mini_shell/c:mini_shell
bazel run  //mini_shell/cpp:mini_shell

bazel test //mini_shell/c:all
bazel test //mini_shell/cpp:all

./mini_shell/check_parity.sh   # build both, run 21 scripted sessions, diff them

printf 'echo hi\nfalse\nexit\n' | bazel-bin/mini_shell/c/mini_shell --no-banner
```

**`check_parity.sh` compares stdout, stderr, *and* the exit status** — unlike
`matrix_ops`, which gave up on stderr. Here the per-command status lines are the
reporting contract and both ports write the same bytes, so a case that skipped stderr
would assert nothing at all about a failing command. Only the argument parser's own
diagnostics are exempt (`run_case_parser_error`, which requires just that both ports
reject the command line) and `--help` (`run_case_status_only`).

**One case in that script is not a comparison**, and must stay that way:
`check_unbuffered` asserts against a fixed expectation rather than against the reference
port, because both ports could regress into reading ahead together and every diff would
still come back clean. See the buffering bullet below.

## Shared behavior (keep the ports in sync)

- **`system()`'s return value is a wait status, not an exit code.** It is decoded with
  `WIFSIGNALED`/`WTERMSIG` before `WIFEXITED`/`WEXITSTATUS`, and `-1` (the command never
  ran) is checked before either. Reading the raw value as a code is the bug this
  ordering prevents: it reports a command killed by `SIGKILL` as "exited with status 0".
  Any port that gets a decoded status from its standard library for free still has to
  produce the same three cases.
- **The decoding is pure and separate from the reporting** (`shell_decode_status` /
  `decode_status` vs `shell_report_status` / `report_status`). That is what makes the
  signal branch testable without arranging for a real process to be killed, and it is
  why each suite forks exactly once — `RealSystem.EncodingMatchesTheMacros`, which pins
  the hand-built wait-status encoding the other tests use against the libc actually
  linked in.
- **`system()` is behind a runner seam**, so the loop is tested with a recording fake
  and never forks: a function pointer (`ShellRunner`) in C, a
  `std::function<int(const std::string &)>` in C++, a trait or `fn` in Rust. Without it
  the command loop is untestable.
- **The stream seam is three parameters, not three globals.** `shell_run` /
  `shell::run` take `(in, out, err)` — `FILE *` in C, `std::istream &`/`std::ostream &`
  in C++ — so the tests drive them with `fmemopen` and `std::istringstream`
  respectively. `err` is separate from `out` on purpose: it is the contract that the
  shell's own complaints never mix into the command's output.
- **The banner and prompt always print**, TTY or not. Do not add an `isatty` branch:
  it makes the output depend on how the program was invoked, which every port would
  then have to reproduce and every test override. `--no-banner` covers scripting.
- **The prompt is flushed before every read.** The command inherits stdout's file
  descriptor, so an unflushed prompt surfaces *after* the command's output.
- **The command input is read unbuffered**, so a command that reads stdin gets what
  mini_shell has not consumed yet. This is the easiest thing here to get wrong without
  noticing: buffered, stdio pulls an entire piped script in before the first fork, and
  `printf 'cat\necho done\n' | mini_shell` hands `cat` an empty stdin while the shell
  goes on to run `echo done` itself. Nothing in the unit suite can catch it — the tests
  drive `shell_run` with `fmemopen`, where buffering is invisible — so it is a
  `setvbuf(stdin, NULL, _IONBF, 0)` in `main` plus the end-to-end check above. POSIX
  requires it and `bash` honors it; **`dash` reads ahead**, so `/bin/sh` is not the
  reference to check a port against. Each port needs its own spelling: unbuffered
  reads, not a `BufReader`. In C++ the `setvbuf` reaches `std::cin` only because
  `sync_with_stdio` is true by default — **`std::ios::sync_with_stdio(false)` puts the
  bug straight back**, and it is the kind of line someone adds for speed without
  suspecting it does anything else. `check_parity.sh`'s `check_unbuffered` is the only
  guard either port has.
- **A failed command never ends the loop and never changes the exit code.** The shell
  ran what it was asked to; `0` means the loop ended cleanly, `1` is the shell's own
  I/O failure, `2` is a usage error. Making the exit code the last command's status was
  considered and rejected: it collides with `1` meaning "the shell itself broke".
- **Each port's argument parser writes its own diagnostics, and that is not shared
  behavior.** C uses `getopt_long` and reports an unknown option or a stray operand
  itself at exit `2`; C++ hands the job to CLI11 and takes CLI11's wording and CLI11's
  exit code (`109` for both). Do not try to reconcile them — the parity script covers
  these with `run_case_parser_error`, which asserts only that both reject the same
  command line, and the table is in [`README.md`](README.md#known-divergences). Which
  spellings are accepted differs too: `getopt_long` matches unambiguous long-option
  prefixes, so `--no-ban` works in C and is an error in C++.
- **Out of memory reports the same line and the same exit code in both**, by different
  routes: C gets a `getline` failure and returns `SHELL_ERR_NOMEM`, C++ catches
  `std::bad_alloc` around the loop in `main`. Any port that lets an allocation failure
  abort instead has diverged.
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

- **ASCII art in a string literal.** Every backslash in the banner must be doubled in
  both ports — the compiler reads `\_` as an unknown escape, and `-Werror` makes that
  fatal. The other two hazards are C-only: a `??` pair is a trigraph in C but not in
  C++17 and later, and the C banner is written with `fputs` rather than `printf` so a
  `%` is not a format specifier (the C++ one goes through `ostream::write`, where the
  question never arises). Check all three before changing the art, and keep the two
  copies identical — `check_parity.sh`'s `banner` case diffs them.
- **`bazel run` and relative paths.** `bazel run` executes from Bazel's runfiles
  directory, not your shell's cwd, so a command using a relative path resolves
  somewhere surprising. Run `bazel-bin/mini_shell/c/mini_shell` directly when that
  matters. (Same trap as `copy_file` and `simple_logger`.)
- **`valgrind` does not run on this machine.** Use sanitizers instead:
  ```sh
  bazel test //mini_shell/c:all //mini_shell/cpp:all --config=permissive \
    --copt=-fsanitize=address --copt=-g --linkopt=-fsanitize=address
  ```
- **`std::cin` cannot see a read error.** It is backed by a `stdio_sync_filebuf` whose
  `underflow()` returns EOF on failure without setting `badbit`, so the C++ loop reads
  an unreadable stdin as a clean end of input. `main.cpp` checks `std::ferror(stdin)`
  afterwards to recover it — the same fixup, for the same reason, as
  `simple_logger/cpp/main.cpp`. Deleting it makes the program exit 0 on a failed read.
- **Tests that assert on the prompt count are asserting on the loop shape.** `ls\nexit\n`
  produces `"$ $ "` — one prompt per line read, including the one `exit` answered. If a
  refactor changes when the prompt is written, those are the tests that will say so.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
