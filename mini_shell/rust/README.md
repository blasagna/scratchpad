# mini_shell (Rust)

A prototype shell: it prints a `$` prompt, reads one command per line from stdin,
splits it into a program and its arguments, runs that program, and reports the status of
anything that did not exit 0. Written in idiomatic Rust with the same semantics as the C
and C++ ports. See the top-level `mini_shell/README.md` for the full contract, including
why none of the ports uses `system()` as the exercise says.

## Where Rust ends up ahead

The other two ports spell out `fork`, `execvp`, `waitpid`, `pipe`, and `fcntl`. This one
is a single expression:

```rust
Command::new(argv[0]).args(&argv[1..]).status()
```

and it is not a shortcut past the exercise — it is *the same program*. `Command` forks
(or `posix_spawn`s), execs, and waits; a program with no `/` is looked up on `PATH`
exactly as `execvp` does; and `.status()` rather than `.output()` means the child
inherits mini_shell's stdin, stdout, and stderr, which is the contract and what makes
`cat` work. No `arg0()` either: `Command` already passes the program as `argv[0]`, which
is what `execvp(argv[0], argv)` does with the word as typed.

**Including the errno pipe.** The interesting part of the C port is that `execvp` fails
in the *child*, so the parent needs a close-on-exec pipe to learn why — otherwise a
missing program and a command that exited 127 are the same event. Rust's standard
library runs that exact mechanism internally and surfaces the result as the `Err` arm of
`io::Result<ExitStatus>`. Twenty lines of C become a `match` on `err.kind()`:

```rust
ErrorKind::NotFound         => Status::NotFound,
ErrorKind::PermissionDenied => Status::NotExecutable,
_                           => Status::Unrunnable(err),
```

Two things still do not come for free:

- **The wait status is decoded, but the ordering is still ours.** `ExitStatus` has
  already done what `WIFSIGNALED`/`WTERMSIG`/`WIFEXITED`/`WEXITSTATUS` do, but
  `decode_status` still checks `signal()` before `code()`, for the same reason the C port
  checks the macros in that order: a status naming a signal has no exit code to report,
  and reading one as the other would call a command killed by `SIGKILL` "exited with
  status 0". It is still pure and still separate from `report_status`, which is what
  makes the signal case testable without arranging for a real process to be killed.
- **`ENOEXEC` behaves differently, and that is a real divergence.** See below.

The rejected alternatives:

- **`libc` through FFI** would match the C ports byte for byte, including the `ENOEXEC`
  case, and is C in Rust clothing: `CString`s, `unsafe` blocks, and a raw wait status to
  pick apart by hand. The point of this port is to show the same program in the language
  it is written in.
- **`popen`-alikes** capture the command's stdout into a pipe, which changes what the
  program does rather than how it is spelled.
- **A crate** (`duct`, `subprocess`) would be a dependency, and the ports here take only
  `clap`.

## Design

The port keeps both of the C port's seams:

- **A runner seam.** `run` never spawns anything. It calls `opts.runner.run(&argv)`
  through a `&mut dyn Runner`, and `main` passes `ExecRunner` — the one impure type
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

Lines are `Vec<u8>`, never `String`, and so are the words `split` cuts them into. The
contract passes non-ASCII bytes through unchanged and neither a program name nor an
argument is required to be UTF-8, so the bytes reach `Command::new` and `Command::arg`
through `OsStr::from_bytes` with no lossy conversion in between. `report_status` writes
the program name with `write_all` rather than `{}` for the same reason. A line containing
a NUL is still **refused** rather than run: Rust would return a spawn error instead of
truncating at the NUL the way `execvp` does, but refusing keeps the message and the loop
behavior identical across all three ports.

`split` is `slice::split` on the whitespace set, with the empty slices filtered out —
they are what a run of whitespace leaves behind, and passing them on would hand the
program blank arguments. That filter is the entire difference between this and
`str::split_whitespace`, which is unavailable here because a word is bytes.

`ShellError` has two variants where the C port's `ShellResult` has four. `getline`
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

The other thing not to undo is the whitespace set, `b" \t\n\x0b\x0c\r"`, spelled out
rather than `u8::is_ascii_whitespace`. That method omits `\v`, which C's `isspace` in the
"C" locale and the C++ port's `" \t\n\v\f\r"` both include — so `"\x0bexit\x0b"` would end
the loop in two ports and run as a command here. It is now the splitter's separator set
as well as `trim`'s, so it also decides where arguments begin and end.

`tests/real_exec.rs` is the one test that really forks. It is stronger than the C and
C++ ports' `RealExec.EncodingMatchesTheMacros`: because the unit suite builds statuses
with `ExitStatus::from_raw(3 << 8)`, it asserts that a hand-built status and a real
process's status decode *identically*, rather than only that the real one decodes as
expected. It reaches `/bin/sh -c "exit 3"` by handing the runner a three-word argv
directly — something the splitter could never produce, since it cannot quote.

## Deliberate divergences

`check_parity.sh` runs this port alongside the other two and only ever asserts that they
*agree*, so it cannot pin a difference. These live here and in the table in
`mini_shell/README.md`:

- **Out of memory aborts** instead of printing `mini_shell: out of memory` and exiting 1.
  `try_reserve` on the line buffer would not cover the `Vec::push` in `read_line` or the
  `Vec` `split` collects into, and the allocator aborts before any of it. Same call
  `matrix_ops/rust` made.
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
- **An executable file with no `#!` line fails here and runs there.** POSIX requires
  `execvp` to retry through a command interpreter on `ENOEXEC`, so glibc runs such a file
  as `execve("/bin/sh", ["/bin/sh", file, ...])` and the C and C++ ports inherit that.
  Rust's `Command` deliberately does not, and reports
  `failed to run command: Exec format error (os error 8)`. **This is the one behavior
  where the C ports still reach a shell and this one does not**, which is either the
  wart or the point depending on your reading. Closing it means resolving `PATH` by hand
  and calling `execv` in C and C++, and a second `PATH` implementation to keep in step
  with this one. No parity case may create such a file while it stands.
- **Error text carries Rust's suffix.** `io::Error`'s `Display` is
  `"<strerror> (os error N)"`. This is exactly why `command not found` and
  `permission denied` are written out in `report_status` rather than taken from the
  error: those two are the failures parity cases reach. What still carries the suffix is
  `mini_shell: failed to run command: …` and the two stream-error lines, and reaching any
  of them needs a fork failure, an unreadable stdin, or a full stdout.
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
`1` for its own failure (a read error on stdin, a write error on stdout or stderr) —
never for a command that could not be found or run, since the shell did its job by
trying; and `2` for a bad command line, which is clap's code and happens to
be the C port's too, where the C++ port exits `109`. See the divergence table in
`mini_shell/README.md`.
