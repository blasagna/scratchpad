# mini_shell

A prototype shell: it prints a `$` prompt, reads one command per line, splits it into a
program and its arguments, runs that program, and reports the status of anything that
did not exit 0. Three ports exist: `c/` and `cpp/` (Bazel) and `rust/` (cargo workspace
member `mini_shell`), all three doing `fork` + `execvp` + `waitpid` — spelled out by
hand in the first two, and as `Command::new(argv[0]).args(..).status()` in the third.
**The exercise as written says `system()`, and this deliberately departs from it**; the
reasoning is at the top of [`README.md`](README.md). The full contract — the loop, the reporting format, edge cases, exit codes,
and the handful of places the ports deliberately differ — is in [`README.md`](README.md),
and each port has its own design notes ([c](c/README.md), [cpp](cpp/README.md),
[rust](rust/README.md)).

## Commands

```sh
bazel run  //mini_shell/c:mini_shell
bazel run  //mini_shell/cpp:mini_shell
cargo run  -p mini_shell

bazel test //mini_shell/c:all
bazel test //mini_shell/cpp:all
cargo test -p mini_shell

./mini_shell/check_parity.sh   # build all three, run 21 scripted sessions, diff them

printf 'echo hi\nfalse\nexit\n' | bazel-bin/mini_shell/c/mini_shell --no-banner
```

**`check_parity.sh` compares stdout, stderr, *and* the exit status** — unlike
`matrix_ops`, which gave up on stderr. Here the per-command status lines are the
reporting contract and every port writes the same bytes, so a case that skipped stderr
would assert nothing at all about a failing command. Only the argument parser's own
diagnostics are exempt (`run_case_parser_error`, which requires just that every port
rejects the command line) and `--help` (`run_case_status_only`).

**All three ports are in that script**, unlike `matrix_ops`, where the Rust port had to
stay out because it accepts a different set of command lines. Nothing here diverges on
what the *loop* does, so `binaries()` lists Rust alongside the other two and `build()`
runs `cargo build` after `bazel build`.

**One case in that script is not a comparison**, and must stay that way:
`check_unbuffered` asserts against a fixed expectation rather than against the reference
port, because every port could regress into reading ahead together and every diff would
still come back clean. See the buffering bullet below.

## Shared behavior (keep the ports in sync)

- **`waitpid`'s status is a wait status, not an exit code.** It is decoded with
  `WIFSIGNALED`/`WTERMSIG` before `WIFEXITED`/`WEXITSTATUS`, and `-1` (the command never
  started) is checked before either. Reading the raw value as a code is the bug this
  ordering prevents: it reports a command killed by `SIGKILL` as "exited with status 0".
  Any port that gets a decoded status from its standard library for free still has to
  produce the same cases: Rust's `Command::status()` gives back an
  `io::Result<ExitStatus>`, and `decode_status` maps `Err(_)`, `ExitStatus::signal()`,
  and `ExitStatus::code()` onto them — in the same order, since a status that names a
  signal has no exit code to report.
- **`execvp` fails in the child, so its errno crosses a close-on-exec pipe.** The child
  writes the errno and `_exit`s; a successful exec closes the pipe instead, so the
  parent reads either `sizeof(int)` bytes or end of file. That is the whole mechanism,
  and Rust gets it for free because `Command` does exactly this internally — which is
  why all three ports can report a missing program identically. **Do not replace it with
  `_exit(127)`/`_exit(126)` and a status check in the parent**: a command that really
  exits 127 then becomes indistinguishable from one that never ran, and Rust cannot
  reproduce the ambiguity anyway, since `Command::status()` returns `Err`. Two details
  in the child are load-bearing: `_exit` and not `exit` (it shares the parent's stdio
  buffers, and flushing them would duplicate the parent's pending output), and `pipe` +
  `fcntl(F_SETFD, FD_CLOEXEC)` rather than `pipe2`, which sits behind `__USE_GNU`. A
  third is the order at the end: **what the child sent is reported in preference to a
  `waitpid` failure**, since the child's errno is the one that says why the command
  never ran. Whoever exec'd mini_shell may have left `SIGCHLD` at `SIG_IGN`, which is
  inherited and makes every `waitpid` here fail `ECHILD`; reading that first turns
  "command not found" into "failed to run command: No child processes". Nothing
  automated covers it — reproduce it with
  `perl -e '$SIG{CHLD}="IGNORE"; exec(@ARGV)' <port> --no-banner`, where all three ports
  still agree (Rust reports the same `ECHILD` from inside `Command::status`).
- **`ENOENT` and `EACCES` are reported in mini_shell's own words**, not with `strerror`.
  This is parity infrastructure, not style: Rust's `io::Error` renders `ENOENT` as
  `No such file or directory (os error 2)` and C's `strerror` as `No such file or
  directory`, and "command not found" is now the most ordinary failure there is, with a
  parity case on it. Every other errno falls through to `failed to run command:`, which
  keeps the system's text precisely because nothing can reach it.
- **Splitting is the whole grammar, and it is one pure function per port**
  (`shell_split` / `split`). Whitespace separates, every other byte is literal. There is
  no quoting and no expansion, so `echo a | wc` prints `a | wc`. Do not "fix" this by
  adding quote handling to one port — it is a documented non-feature in all three, with
  a parity case (`metacharacters`) pinning it.
- **The decoding is pure and separate from the reporting** (`shell_decode_status` /
  `decode_status` vs `shell_report_status` / `report_status`). That is what makes the
  signal branch testable without arranging for a real process to be killed, and it is
  why each suite forks exactly once — `RealSystem.EncodingMatchesTheMacros`, which pins
  the hand-built wait-status encoding the other tests use against the libc actually
  linked in.
- **The fork is behind a runner seam**, so the loop is tested with a recording fake and
  never forks: a function pointer (`ShellRunner`, taking `char *const argv[]`) in C, a
  `std::function<int(const std::vector<std::string> &)>` in C++, and a `&mut dyn Runner`
  taking `&[&[u8]]` in Rust — a trait object rather than a boxed closure, so the fake is
  a plain struct the test reads afterwards instead of an `Rc<RefCell<_>>` it has to
  borrow at every assertion. The C and C++ convention is **a wait status, or `-1` with
  `errno` set**, which is what `decode_status(raw, err)` takes; the loop clears `errno`
  before the call so the fake can set it. Without the seam the command loop is
  untestable.
- **The stream seam is three parameters, not three globals.** `shell_run` /
  `shell::run` take `(in, out, err)` — `FILE *` in C, `std::istream &`/`std::ostream &`
  in C++, generic `Read`/`Write` in Rust — so the tests drive them with `fmemopen`,
  `std::istringstream`, and plain `&[u8]`/`Vec<u8>` respectively. `err` is separate from
  `out` on purpose: it is the contract that the
  shell's own complaints never mix into the command's output.
- **The banner and prompt always print**, TTY or not. Do not add an `isatty` branch:
  it makes the output depend on how the program was invoked, which every port would
  then have to reproduce and every test override. `--no-banner` covers scripting.
- **The prompt is flushed before every read.** The command inherits stdout's file
  descriptor, so an unflushed prompt surfaces *after* the command's output. In C and C++
  the child must also `_exit` rather than `exit`, for the mirror-image reason.
- **The command input is read unbuffered**, so a command that reads stdin gets what
  mini_shell has not consumed yet. This is the easiest thing here to get wrong without
  noticing: buffered, stdio pulls an entire piped script in before the first fork, and
  `printf 'cat\necho done\n' | mini_shell` hands `cat` an empty stdin while the shell
  goes on to run `echo done` itself. Nothing in the unit suite can catch it — the tests
  drive `shell_run` with `fmemopen`, where buffering is invisible — so it is a
  `setvbuf(stdin, NULL, _IONBF, 0)` in `main` plus the end-to-end check above. POSIX
  requires it and `bash` honors it; **`dash` reads ahead**, so `/bin/sh` is not the
  reference to check a port against. Each port needs its own spelling. In Rust there is
  no `setvbuf` to reach for at all — `io::stdin()` is always a `BufReader` and cannot be
  told otherwise — so `main` takes a `dup` of fd 0 as a plain `File`
  (`File::from(io::stdin().as_fd().try_clone_to_owned()?)`) and `read_line` reads **one
  byte at a time**. **`BufRead::read_until`, which is how `simple_logger/rust` reads
  stdin, puts the bug straight back**, and it is that port's exact counterpart of the
  `sync_with_stdio(false)` hazard below. In C++ the `setvbuf` reaches `std::cin` only because
  `sync_with_stdio` is true by default — **`std::ios::sync_with_stdio(false)` puts the
  bug straight back**, and it is the kind of line someone adds for speed without
  suspecting it does anything else. `check_parity.sh`'s `check_unbuffered` is the only
  guard the C and C++ ports have; the Rust one additionally pins it from `cargo test`,
  in `rust/tests/cli.rs`, by spawning the binary.
- **A failed command never ends the loop and never changes the exit code.** The shell
  ran what it was asked to; `0` means the loop ended cleanly, `1` is the shell's own
  I/O failure, `2` is a usage error. Making the exit code the last command's status was
  considered and rejected: it collides with `1` meaning "the shell itself broke".
- **Each port's argument parser writes its own diagnostics, and that is not shared
  behavior.** C uses `getopt_long` and reports an unknown option or a stray operand
  itself at exit `2`; C++ hands the job to CLI11 and takes CLI11's wording and CLI11's
  exit code (`109` for both); Rust hands it to clap, which happens to exit `2` as well —
  a coincidence, not something to rely on. Do not try to reconcile them — the parity
  script covers these with `run_case_parser_error`, which asserts only that every port
  rejects the same command line, and the table is in
  [`README.md`](README.md#known-divergences). Which spellings are accepted differs too:
  `getopt_long` matches unambiguous long-option prefixes, so `--no-ban` works in C and
  is an error in C++ and Rust.
- **Out of memory reports the same line and the same exit code in C and C++**, by
  different routes: C gets a `getline` failure or a failed `malloc` of the argv array
  and returns `SHELL_ERR_NOMEM`, C++ catches `std::bad_alloc` around the loop in `main`. **The Rust port aborts instead**,
  which this file used to call a divergence in advance and now records as one: Rust's
  allocator aborts before any `try_reserve` dance could see it, and the same call is
  already made and documented in `matrix_ops/rust`. It is in the divergence table in
  [`README.md`](README.md#known-divergences); `ShellError` accordingly has two
  variants where C's `ShellResult` has four, since `Read::read` also separates end of
  input from a read error in the type where `getline` needs `ferror`/`feof` to.
- **`exit` is the only builtin**, matched as the bare word after trimming ASCII
  whitespace. `EXIT`, `exitx`, and `exit 3` are looked up as programs like anything
  else, and none of them is one, so all three now report "command not found". **`cd` is
  deliberately absent** and reports the same way — it would not have worked before
  either, since every command was a fresh process, but it now says so rather than
  appearing to do nothing. One builtin invites the rest of them.
- **A line containing a NUL is refused, not truncated.** `execvp` takes NUL-terminated
  strings, so running `echo a\0rm -rf /` would run `echo a` and silently drop the rest.
  This is the one place the port is not byte-transparent, unlike `simple_logger`, and
  the `execvp` signature is the reason — the same reason `system()` gave before it.
- **End of input writes one closing newline; `exit` does not.** The prompt just written
  is the last thing on its line, so Ctrl-D leaves the cursor somewhere sane.
- **Status 127 is not special-cased**, and there is no longer anything special about
  it: the interpreter that used to exit 127 for a missing command is gone, and
  mini_shell reports a missing program itself, before any wait status exists. A command
  that exits 127 chose to.

## Gotchas

- **ASCII art in a string literal.** Every backslash in the banner must be doubled in
  all three ports — every one of these compilers reads `\_` as an unknown escape, and
  `-Werror` makes that fatal in the two Bazel ones. Two more hazards are C-only: a `??`
  pair is a trigraph in C but not in C++17 and later, and the C banner is written with
  `fputs` rather than `printf` so a `%` is not a format specifier (the C++ one goes
  through `ostream::write`, where the question never arises). Rust has the same shape of
  hazard with a different character: the banner goes out through `write_all`, **never
  `write!`**, so a `{` in future art is not a format hole. Check all of them before
  changing the art, and keep the three copies identical — `check_parity.sh`'s `banner`
  case diffs them.
- **The whitespace set is written out, not delegated.** C uses `isspace` in the "C"
  locale and C++ spells `" \t\n\v\f\r"`; Rust must spell it too, because
  `u8::is_ascii_whitespace` **omits `\v`**. Reaching for that method makes `"\x0bexit"`
  end the loop in two ports and run as a command in the third — a divergence no parity
  case would catch, since none feeds a vertical tab. The same set now does double duty
  as the splitter's separators, so it also decides argument boundaries: `\r` is in it,
  which is why `echo a\rb` is two arguments and not one.
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
- **`SIGINT` is no longer a divergence, because nothing blocks it any more.** `system()`
  was required by POSIX to set `SIGINT`/`SIGQUIT` to `SIG_IGN` in the caller while the
  command ran, so Ctrl-C at a terminal used to kill only the child in C and C++ while
  the Rust port died with it. With `fork` + `exec` no port does that, so all three now
  die together. Restoring it properly means `signal(2)` in the parent, which in Rust
  means a `libc` dependency and an `unsafe` block, and the ports here take only `clap`.
  Interactive-only, so nothing in `check_parity.sh` or any suite will tell you about it.
- **`execvp` still reaches `/bin/sh` in exactly one case, and Rust does not.** POSIX
  requires `execvp` to retry through a command interpreter on `ENOEXEC` — an executable
  file with no `#!` line and no recognized format — so glibc runs it as
  `execve("/bin/sh", ["/bin/sh", file, ...])`. Rust's `Command` returns `ENOEXEC`
  instead. It is the one shell left in the C and C++ ports and it is a documented
  divergence; closing it means resolving `PATH` by hand and calling `execv`. **No parity
  case may create an executable file without a shebang** while that stands —
  `check_parity.sh`'s `suicide.sh` fixture has one for this reason.
- **A bare `read(2)` must retry `EINTR` by hand.** `impl Read for File` surfaces
  `ErrorKind::Interrupted` where stdio restarts the read under `SA_RESTART`, so the Rust
  `read_line` has a retry arm the C and C++ loops need no counterpart to. Delete it and
  a `SIGWINCH` during a read is reported as `mini_shell: error reading command input`.
  `write_all` and `flush` already retry internally.
- **The forking test can build an argv the splitter never could.** `RealExec` passes
  `{"/bin/sh", "-c", "exit 3"}` as three words, which is how a wait status of 3 is still
  arranged without a grammar that can quote. Do not read that as the ports shelling out:
  it is a test fixture, and `/bin/sh` there is just a convenient program.
- **Tests that assert on the prompt count are asserting on the loop shape.** `ls\nexit\n`
  produces `"$ $ "` — one prompt per line read, including the one `exit` answered. If a
  refactor changes when the prompt is written, those are the tests that will say so.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
