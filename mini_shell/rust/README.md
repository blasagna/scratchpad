# mini_shell (Rust)

A prototype shell: it prints a `$` prompt, reads one command per line from stdin, runs
it through the system command interpreter, and reports the status of anything that did
not exit 0. Written in idiomatic Rust with the same semantics as the C and C++ ports.
See the top-level `mini_shell/README.md` for the full contract.

## What replaces `system()`

Rust has no `system()`, deliberately — the standard library exposes process creation
and nothing that hands a string to a shell. The equivalent is one `Command`:

```rust
Command::new("/bin/sh").arg0("sh").arg("-c").arg(command).status()
```

which is what glibc's `system()` does internally (`execl("/bin/sh", "sh", "-c", line,
NULL)`), so `ls | wc -l`, `echo *`, and `exit 3` keep working. The path is hardcoded
rather than searched for on `$PATH`, and `arg0` is set, for the same reason: to be that
call rather than something near it. `.status()` and not `.output()` — the command
inherits mini_shell's stdin, stdout, and stderr, which is the contract and what makes
`cat` work.

The alternatives were the same three the C++ port weighed, plus one:

- **`libc::system` through FFI** would be exact parity for free, and is C in Rust
  clothing: a `CString` to build, an `unsafe` block, and the raw wait status back again.
  The exercise is about what `system()` *does*, and this port has to show that in the
  language it is written in.
- **`popen`-alikes** capture the command's stdout into a pipe, which changes what the
  program does rather than how it is spelled.
- **`fork` + `exec`** is what a real shell does, and it skips the interpreter — a
  different exercise.
- **A crate** (`duct`, `subprocess`) would be a dependency, and the ports here take only
  `clap`.

Three things `system()` gives away for free do not come with `Command`:

- **The status is already decoded.** `system()` returns a raw wait status that the C and
  C++ ports pick apart with `WIFSIGNALED`/`WTERMSIG`/`WIFEXITED`/`WEXITSTATUS`, checking
  signals first so a command killed by `SIGKILL` is not reported as "exited with status
  0". `Command::status()` returns `io::Result<ExitStatus>`, so the same three outcomes
  come out of `Err(_)`, `ExitStatus::signal()`, and `ExitStatus::code()` — in that order,
  for the same reason, since a status naming a signal has no exit code to report.
  `decode_status` is still pure and still separate from `report_status`, which is what
  makes the signal case testable without arranging for a real process to be killed.
- **`system(NULL)` has no counterpart.** glibc implements it as `do_system("exit 0")`, so
  `interpreter_available()` runs exactly that once at startup.
- **The `SIGINT`/`SIGQUIT` blocking is gone.** See the divergences below.

## Design

The port keeps both of the C port's seams:

- **A runner seam.** `run` never spawns anything. It calls `opts.runner.run(command)`
  through a `&mut dyn Runner`, and `main` passes `SystemRunner` — the one impure type
  here. A trait object rather than a `Box<dyn FnMut>`: it is the literal translation of
  C's `(ShellRunner, void *ctx)` pair, with the vtable for the function pointer and the
  data pointer for the context, and it lets the test fake be a plain struct whose
  `commands` field the test reads after `run` returns. A closure would need
  `Rc<RefCell<_>>` and a `.borrow()` at every assertion.
- **A stream seam.** `run(input, out, err, opts)` is generic over `Read`/`Write` rather
  than reaching for `io::stdin()`/`stdout()`/`stderr()`, so the tests drive it with
  `&[u8]` and `Vec<u8>` and the C suite's `fmemopen` scaffolding collapses to nothing.
  `err` is separate from `out` on purpose: it is the contract that the shell's own
  complaints never mix into the command's output.

Lines are `Vec<u8>`, never `String`. The contract passes non-ASCII bytes through
unchanged and a command line is not required to be UTF-8, so the bytes reach
`Command::arg` through `OsStr::from_bytes` with no lossy conversion in between. A line
containing a NUL is still **refused** rather than run: Rust would return a spawn error
instead of truncating at the NUL the way `system()` does, but refusing keeps the message
and the loop behavior identical across all three ports.

`ShellError` has three variants where the C port's `ShellResult` has five. `getline`
returns `-1` for end of input, a read error, and an allocation failure alike, which is
the only reason `shell_run` consults `ferror`/`feof` and the only reason
`SHELL_ERR_NOMEM` exists; `Read::read` separates them in the type. `Display` composes the
label with the underlying `io::Error`, so `main` reproduces both of the C port's shapes
from one match arm.

### Reading unbuffered is the subtle part

`io::stdin()` is always a `BufReader`, and there is no way to turn that off — the C
ports' `setvbuf(stdin, NULL, _IONBF, 0)` has no counterpart. Buffered, it pulls the whole
piped script in before the first fork, and `printf 'cat\necho done\n' | mini_shell` hands
`cat` an empty stdin while mini_shell goes on to run `echo done` itself. So `main` takes
a duplicate of fd 0 as a plain `File`:

```rust
let mut input = File::from(io::stdin().as_fd().try_clone_to_owned()?);
```

`dup(2)` shares the file description and therefore the offset, so the child still sees
exactly what mini_shell has not consumed. No `unsafe` is needed; `File::from_raw_fd(0)`
would be, and would close fd 0 when the `File` dropped.

`read_line` then reads **one byte at a time**. **Reaching for `BufRead::read_until` —
which is how `simple_logger/rust` reads stdin — puts the bug straight back**, and it is
the exact Rust counterpart of `std::ios::sync_with_stdio(false)` in the C++ port: a line
someone adds for speed without suspecting it does anything else. No unit test can catch
it, since they all drive `run` with an in-memory stream where buffering is invisible;
`check_parity.sh`'s `check_unbuffered` and `tests/cli.rs` are the guards.

One consequence of the bare `read(2)`: it must retry `ErrorKind::Interrupted` by hand.
stdio restarts an interrupted read under `SA_RESTART`, so the C ports never see EINTR;
without that arm here, a `SIGWINCH` mid-read would be reported as
`mini_shell: error reading command input`. `write_all` and `flush` already retry
internally.

The other thing not to undo is `trim`'s whitespace set, `b" \t\n\x0b\x0c\r"`, spelled out
rather than `u8::is_ascii_whitespace`. That method omits `\v`, which C's `isspace` in the
"C" locale and the C++ port's `" \t\n\v\f\r"` both include — so `"\x0bexit\x0b"` would end
the loop in two ports and run as a command here.

`tests/real_system.rs` is the one test that really forks. It is stronger than the C and
C++ ports' `RealSystem.EncodingMatchesTheMacros`: because the unit suite builds statuses
with `ExitStatus::from_raw(3 << 8)`, it asserts that a hand-built status and a real
`/bin/sh` status decode *identically*, rather than only that the real one decodes as
expected.

## Deliberate divergences

`check_parity.sh` runs this port alongside the other two and only ever asserts that they
*agree*, so it cannot pin a difference. These live here and in the table in
`mini_shell/README.md`:

- **Out of memory aborts** instead of printing `mini_shell: out of memory` and exiting 1.
  `try_reserve` on the line buffer would not cover the `Vec::push` in `read_line`, and
  the allocator aborts before any of it. Same call `matrix_ops/rust` made.
- **Argument errors are clap's, at exit 2.** The wording is clap's, not the C port's
  `usage:` pair nor CLI11's. The *code* agrees with C and differs from C++'s `109` —
  the first time in this repo the Rust port lands on C's side of that row. `--help` is
  likewise clap's rendering, at exit 0 like the other two.
- **`--no-ban` is rejected.** `getopt_long` matches unambiguous long-option prefixes;
  clap does not infer them, and `infer_long_args(true)` is deliberately not set. So this
  port matches C++ and diverges from C.
- **A read error on stdin behaves like C, not C++.** `Read::read` reports the failure
  directly, so there is no `stdio_sync_filebuf` blind spot: no closing `\n` on stdout
  before the diagnostic, and no `ferror(stdin)` fixup in `main`.
- **`SIGINT` during a command kills mini_shell too.** POSIX requires `system()` to set
  `SIGINT` and `SIGQUIT` to `SIG_IGN` in the caller while the command runs, so Ctrl-C at
  a terminal kills only the child and the C and C++ ports report
  `command terminated by signal 2` and prompt again. `Command::status()` does no such
  thing, so the parent takes the signal as well and dies. Restoring it means `signal(2)`
  in the parent, which means a `libc` dependency and an `unsafe` block, and the ports
  here take only `clap`. Interactive only — no parity case reaches it.
- **Error text carries Rust's suffix.** `io::Error`'s `Display` is
  `"<strerror> (os error N)"`, so `mini_shell: failed to run command: …` and the two
  stream-error lines gain ` (os error 11)` where C prints a bare `strerror`. No parity
  case exercises any of the three — they need a fork failure, an unreadable stdin, or a
  full stdout — but it is a real byte difference.
- **`mini_shell: cannot duplicate stdin: …`** replaces the C ports'
  `cannot unbuffer stdin: …`. Different mechanism, same exit 1, effectively unreachable.

## Build & run

```sh
cargo run  -p mini_shell
cargo test -p mini_shell

printf 'echo hi\nfalse\nexit\n' | cargo run -p mini_shell -- --no-banner
```

Unlike `bazel run`, `cargo run` executes in your current working directory, so relative
paths in a command resolve normally.

The program exits `0` whenever the loop ended cleanly, **whatever the commands did**;
`1` for its own failure (a read error on stdin, a write error on stdout or stderr, no
command interpreter); and `2` for a bad command line, which is clap's code and happens to
be the C port's too, where the C++ port exits `109`. See the divergence table in
`mini_shell/README.md`.
