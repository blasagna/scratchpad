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

## Contract

The program is implemented twice so far — `c/` and `cpp/`, both Bazel — and this
section is what both are checked against; Rust follows. `check_parity.sh` builds the
two, feeds them the same scripted stdin, and diffs stdout, stderr, and the exit status.

```
mini_shell [options]
```

There are no operands: commands come from stdin, one per line. Each iteration writes
the prompt `$ ` (with a trailing space) to stdout and flushes it, reads a line, and
hands it to the system command interpreter with `system()`. The loop ends at `exit` or
at end of input.

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

The raw value `system()` returns is a **wait status, not an exit code**. Reading it as
one would report a command killed by `SIGKILL` as "exited with status 0", so it is
decoded with `WIFSIGNALED`/`WTERMSIG`/`WIFEXITED`/`WEXITSTATUS` and the signal case is
checked first. `-1` means `system()` could not fork or wait at all — the command never
ran, and `errno` is about the shell rather than the command.

**A failed command never ends the loop and never changes mini_shell's own exit code.**
The shell did its job: it ran what it was asked to.

### Edge cases

| Case | Behavior |
|---|---|
| Blank or whitespace-only line | Skipped — prompted for, but not run. An interpreter would exit 0 for it anyway, and skipping saves a fork per stray Enter. |
| `exit` with surrounding whitespace | Ends the loop. `EXIT`, `exitx`, and `exit 3` do not; they are commands like any other and go to the interpreter, which is what a real shell does with them. |
| End of input (Ctrl-D) | Ends the loop, exit 0, and one newline is written so the cursor does not stop on the prompt's line. A line ended by `exit` gets no such newline. |
| Final line with no newline | Still run. |
| CRLF input | One trailing `\n` is stripped, then one trailing `\r`. A `\r` anywhere else belongs to the command. |
| Line containing a NUL byte | Refused with `mini_shell: command contains a NUL byte`, and the loop continues. `system()` takes a NUL-terminated string, so the alternative is silently running a truncated command — `echo a\0rm -rf /` would run as `echo a`. |
| Non-ASCII bytes | Passed through to the interpreter unchanged. |
| A command that reads stdin (`cat`, `read`, `ssh`) | Gets the input mini_shell has not consumed yet, so `printf 'cat\necho done\n' \| mini_shell` hands `cat` the `echo done` line. This requires reading the command input **unbuffered** — buffered, stdio pulls the whole pipe in before the first fork and the command sees an empty stdin. POSIX requires it of a shell, and `bash` honors it; **`dash` does not, so do not use `/bin/sh` as the reference** when checking a port against this row. |
| `cd` | Runs, and appears to do nothing. Every command gets a fresh subshell, so the working directory it sets dies with it. Making `cd` a builtin over `chdir()` was considered and left out: the exercise is about `system()`, and one builtin invites the rest of them (`export`, `pwd`, pipelines the interpreter already handles). It is documented rather than papered over. |
| Command not found | The interpreter prints its own `not found` message and exits 127, and mini_shell reports the 127 after it. Not special-cased: suppressing the status line would hide the one thing mini_shell actually knows. |
| No command interpreter | `system(NULL)` is checked once at startup. Without one, exit 1 immediately rather than one errno line per command. |

### Known divergences

Everything above is shared by both ports. These are not, and each is deliberate:

| Case | C | C++ |
|---|---|---|
| Unknown option, stray operand | own message, exit `2` | CLI11's message, exit `109` |
| `--help` text | hand-written | CLI11's rendering (exit `0` in both) |
| Abbreviated long option (`--no-ban`) | accepted — `getopt_long` matches unambiguous prefixes | rejected as an unknown option |
| Out of memory | `getline` fails → `mini_shell: out of memory`, exit 1 | `std::bad_alloc` caught in `main` → same message, same code |
| A read error on stdin | the error line, and nothing on stdout | one `\n` on stdout first, then the error line |

Why each stands:

- **Argument errors belong to the parser.** Each port takes its own — `getopt_long` in
  C, CLI11 in C++ — and neither the wording nor the exit code is worth hand-rolling
  back into agreement. This is the same call `simple_logger` and `matrix_ops` made.
  `check_parity.sh` registers these as `run_case_parser_error`, which requires only
  that both ports reject the same command line.
- **The read-error newline** falls out of how C++ sees the failure. `std::cin` is backed
  by a `stdio_sync_filebuf` that returns EOF on a read error without setting `badbit`,
  so the loop cannot tell a failed read from a clean end of input and writes its closing
  newline before `main` checks `ferror(stdin)` and reports the error. Making the two
  agree would mean reimplementing the loop against a `FILE *`, which is the seam the C++
  port exists to avoid.

### Exit codes

| Code | When |
|---|---|
| `0` | The loop ended at `exit` or at end of input — **regardless of what any command did**. |
| `1` | mini_shell's own failure: a read error on stdin, a write error on stdout or stderr, out of memory, or no command interpreter. |
| `2` | A usage error: an unknown option, or any operand (there are none). This is the **C port's** code; in C++ a bad command line is CLI11's to report and carries its code (`109`), per the divergences above. |
