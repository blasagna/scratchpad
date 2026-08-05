# mini shell

An exercise from the little book of c. 

Build a a CLI program that implements a minimal, prototype shell program which takes user commands, runs them using C's system calls (`system()`), and shows the results. For example commands including `ls`, `date`, `echo hello world` are all valid.

Requirements:
1. display a prompt `$`
1. include a cool ascii art welcome message on initial start up
1. read a valid command from the user
1. run the command using the system
1. check `system()` return values and report any non zero exit codes
1. repeat until the user enters exit

### Departing from the exercise: `fork` + `exec`, not `system()`

The exercise says `system()`, and the first cut of all three ports used it. But
`system()` is `/bin/sh`, so the interpreter did the real work — splitting the line,
expanding globs and variables, building pipelines — and mini_shell was a front end to a
shell rather than a thing that runs programs. It now **forks and execs one program per
line**, and owns the splitting and the failure classification itself.

The trade is deliberate and total: pipes, redirection, globbing, quoting, and variable
expansion are all gone, because every one of them was `/bin/sh`'s. What is gained is
that everything left is mini_shell's own — including the answer to *why* a command did
not run, which `system()` could only ever report as somebody else's exit status of 127.

## Contract

The program is implemented three times — `c/` and `cpp/` (Bazel) and `rust/` (cargo
workspace member `mini_shell`) — and this section is what all three are checked against.
`check_parity.sh` builds them, feeds them the same scripted stdin, and diffs stdout,
stderr, and the exit status.

```
mini_shell [options]
```

There are no operands: commands come from stdin, one per line. Each iteration writes
the prompt `$ ` (with a trailing space) to stdout and flushes it, reads a line, splits
it into words, and runs the first word as a program with the rest as its arguments —
`fork` + `execvp` + `waitpid` in C and C++, and `Command::new(argv[0]).args(&argv[1..])
.status()` in Rust, which is the same three calls with the errno relay built in. The
loop ends at `exit` or at end of input.

**Splitting is the whole grammar.** A run of ASCII whitespace separates two words and
every other byte is literal. There is no quoting, no escaping, and no expansion of any
kind, so `echo a | wc` runs `echo` with the three arguments `a`, `|`, and `wc` and
prints `a | wc`. The program is looked up on `PATH` when it contains no `/`, which is
the one lookup `execvp` still does for us.

**The banner and the prompt are always printed**, whether or not stdin is a terminal.
Branching on `isatty` was the alternative and was rejected: it makes the output depend
on how the program was invoked, which every port would then have to reproduce
identically and every test would have to override. `--no-banner` covers the scripted
case without an environment-dependent branch.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--no-banner` | off | skip the startup banner; the prompt still prints |
| `-h, --help` | — | show help |

### Reporting

A command that exits 0 produces nothing beyond its own output. Anything else gets one
line on **stderr**:

```
mini_shell: command exited with status 3
mini_shell: command terminated by signal 9
mini_shell: failed to run command: <strerror>
```

```
mini_shell: nosuchcmd: command not found
mini_shell: /etc/passwd: permission denied
```

The value `waitpid` reports is a **wait status, not an exit code**. Reading it as one
would report a command killed by `SIGKILL` as "exited with status 0", so it is decoded
with `WIFSIGNALED`/`WTERMSIG`/`WIFEXITED`/`WEXITSTATUS` and the signal case is checked
first. A port whose standard library hands back a decoded status instead still owes the
same cases: Rust's `Command::status()` returns `io::Result<ExitStatus>`, and they come
out of `Err(_)`, `ExitStatus::signal()`, and `ExitStatus::code()`, checked in that
order.

**The last three lines are about a command that never started**, and telling them apart
is what the ports do that `system()` used to. `execvp` fails *in the child*, where the
parent cannot see its `errno`, so the C and C++ ports join the two with a close-on-exec
pipe: the child writes the errno and `_exit`s, a successful exec closes the pipe
instead, and the parent reads either four bytes or end of file. Rust's `Command` runs
that same pipe internally and reports it as `Err`. Inferring it from an exit status of
`127` was the alternative and was rejected — a command that really does exit 127 would
be indistinguishable from one that never ran.

**The first two of those three are written in mini_shell's own words rather than with
`strerror`.** That is load-bearing for parity, not decoration: Rust's `io::Error`
renders `ENOENT` as `No such file or directory (os error 2)` where C's `strerror` says
`No such file or directory`, so a shared message has to be one no port borrows from its
standard library. Every other errno falls through to `failed to run command:`, which
does carry the system's text — and which no parity case can reach, since it needs a
failed `fork` or an exhausted process table.

**A failed command never ends the loop and never changes mini_shell's own exit code.**
The shell did its job: it ran what it was asked to.

### Edge cases

| Case | Behavior |
|---|---|
| Blank or whitespace-only line | Skipped — prompted for, but not run. An interpreter would exit 0 for it anyway, and skipping saves a fork per stray Enter. |
| `exit` with surrounding whitespace | Ends the loop. `EXIT`, `exitx`, and `exit 3` do not; they are commands like any other and go to the interpreter, which is what a real shell does with them. |
| End of input (Ctrl-D) | Ends the loop, exit 0, and one newline is written so the cursor does not stop on the prompt's line. A line ended by `exit` gets no such newline. |
| Final line with no newline | Still run. |
| CRLF input | One trailing `\n` is stripped, then one trailing `\r`. A `\r` anywhere else is ASCII whitespace like any other and separates two words. |
| Line containing a NUL byte | Refused with `mini_shell: command contains a NUL byte`, and the loop continues. `execvp` takes NUL-terminated strings, so the alternative is silently running a truncated command — `echo a\0rm -rf /` would run as `echo a`. |
| Non-ASCII bytes | Passed through to the program unchanged: a word is bytes, and neither a program name nor an argument has to be UTF-8. |
| `\|`, `>`, `*`, `$HOME`, `"a b"` | **Ordinary bytes in ordinary words.** `echo a \| wc` prints `a \| wc`; `echo *` prints `*`. Every one of these was `/bin/sh`'s doing, and there is no `/bin/sh` here. Restoring any of them means writing a tokenizer, an expander, and a pipeline builder — which is to say writing a shell, and a different exercise. |
| An argument containing a space | Cannot be expressed. Whitespace separates words and nothing groups them back together. |
| A command that reads stdin (`cat`, `read`, `ssh`) | Gets the input mini_shell has not consumed yet, so `printf 'cat\necho done\n' \| mini_shell` hands `cat` the `echo done` line. This requires reading the command input **unbuffered** — buffered, stdio pulls the whole pipe in before the first fork and the command sees an empty stdin. POSIX requires it of a shell, and `bash` honors it; **`dash` does not, so do not use `/bin/sh` as the reference** when checking a port against this row. |
| `cd` | **Not found.** There is no `cd` program on Linux, and mini_shell has no builtins beyond `exit`. It would not have worked before either — every command got a fresh subshell, so the directory it set died with it — but it now says so out loud instead of appearing to do nothing. Making it a builtin over `chdir()` was considered and left out: one builtin invites the rest of them. |
| Command not found | `mini_shell: <program>: command not found`, and the loop continues. mini_shell reports this itself now: the interpreter that used to print its own message and exit 127 is gone. An exit status of 127 is accordingly no longer special in any way — it is whatever the command chose. |
| A program that exists but cannot be executed | `mini_shell: <program>: permission denied`. Covers a non-executable file and a directory alike, both of which `execvp` reports as `EACCES`. |
| A program with no `#!` line | Runs under `/bin/sh` in C and C++, and fails in Rust. This is `execvp`'s POSIX-mandated `ENOEXEC` retry, and it is the one place a shell still creeps in. See the divergences below. |

### Known divergences

Everything above is shared by all three ports. These are not, and each is deliberate:

| Case | C | C++ | Rust |
|---|---|---|---|
| Unknown option, stray operand | own message, exit `2` | CLI11's message, exit `109` | clap's message, exit `2` |
| `--help` text | hand-written | CLI11's rendering | clap's rendering (exit `0` in all three) |
| Abbreviated long option (`--no-ban`) | accepted — `getopt_long` matches unambiguous prefixes | rejected as an unknown option | rejected as an unknown option |
| Out of memory | `getline` or the argv array fails → `mini_shell: out of memory`, exit 1 | `std::bad_alloc` caught in `main` → same message, same code | **aborts** |
| A read error on stdin | the error line, and nothing on stdout | one `\n` on stdout first, then the error line | as C: the error line, nothing on stdout |
| An executable file with no `#!` line | **runs it under `/bin/sh`** | as C | **`failed to run command: Exec format error`** |
| Text of an errno in a diagnostic | bare `strerror` | bare `std::error_code::message()` | `strerror` plus ` (os error N)` |
| Unbuffering stdin fails | `mini_shell: cannot unbuffer stdin: …` | as C | `mini_shell: cannot duplicate stdin: …` |

Why each stands:

- **Argument errors belong to the parser.** Each port takes its own — `getopt_long` in
  C, CLI11 in C++, clap in Rust — and neither the wording nor the exit code is worth
  hand-rolling back into agreement. This is the same call `simple_logger` and
  `matrix_ops` made. `check_parity.sh` registers these as `run_case_parser_error`, which
  requires only that every port reject the same command line. That clap lands on C's
  exit `2` is a coincidence, not a contract.
- **The read-error newline** falls out of how C++ sees the failure. `std::cin` is backed
  by a `stdio_sync_filebuf` that returns EOF on a read error without setting `badbit`,
  so the loop cannot tell a failed read from a clean end of input and writes its closing
  newline before `main` checks `ferror(stdin)` and reports the error. Making the two
  agree would mean reimplementing the loop against a `FILE *`, which is the seam the C++
  port exists to avoid. Rust's `Read::read` reports the failure directly, so it has no
  such blind spot and lands on C's behavior.
- **Out of memory aborts in Rust.** `try_reserve` on the line buffer would not cover the
  `Vec::push` that reads each byte, and the allocator aborts before any of it. The same
  gap, for the same reason, as [`matrix_ops`](../matrix_ops/README.md#known-divergence-the-rust-port).
- **The `ENOEXEC` retry is `execvp`'s, and Rust does not have it.** POSIX requires
  `execvp` to run a command interpreter when the file is executable but is not in a
  format the kernel recognizes — no `#!` line, no ELF header — so glibc retries as
  `execve("/bin/sh", ["/bin/sh", file, args...])`. Rust's `Command` deliberately does
  not, and returns `ENOEXEC` instead. This is the one place a shell still creeps into
  the C and C++ ports after `system()` was taken out, and it is a genuine wart: closing
  it means resolving `PATH` by hand and calling `execv`, which is another thirty lines
  per port and a second `PATH` implementation to keep in step with Rust's. Left as a
  divergence for now, and **no parity case may create such a file** while it stands.
- **The errno suffix is `io::Error`'s `Display`.** Rust names the numeric code as well as
  the text. This is exactly why `command not found` and `permission denied` are written
  in mini_shell's own words rather than borrowed from `strerror` — those two are now the
  most ordinary failures there are, and a parity case reaches each of them. What is left
  borrowing the system's wording is `failed to run command:` and the two stream-error
  lines, and reaching any of them takes a fork failure, an unreadable stdin, or a full
  stdout.

`check_parity.sh` only ever asserts that the ports *agree*, so it cannot pin a
difference; every row above lives here instead. Same treatment
[`simple_logger/README.md`](../simple_logger/README.md) gives its own.

### Exit codes

| Code | When |
|---|---|
| `0` | The loop ended at `exit` or at end of input — **regardless of what any command did**. |
| `1` | mini_shell's own failure: a read error on stdin, a write error on stdout or stderr, or out of memory. **Not** a command that failed, however it failed — including one that was never found. |
| `2` | A usage error: an unknown option, or any operand (there are none). This is the code the **C** port reports itself and the one **clap** happens to use in Rust; in C++ a bad command line is CLI11's to report and carries its code (`109`), per the divergences above. |
