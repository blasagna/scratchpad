
# simple logger

A mini project from the little book of c.

Build a CLI program that appends messages to a log file with timestamps

Requirements:
1. accept an agument for a message, or read from stdin if no arg is provided
1. append the message to log file given as an argument
1. prepend a timestamp to each message, wrapped in square brackets, followed by a delimeter, default to space
1. keep all previous entries; append not overwrite
1. allow multiple entries in one run, with a default unix newline separator
   (the delimiter and separator were configurable at first and no longer are —
   see the entry format below for why)
1. allow multiple log levels for later filtering: debug, info, warning, error, also wrapped in square brackets

## Contract

The program is implemented three times — `c/` and `cpp/` (Bazel), `rust/` (cargo) —
and this section is what all three are checked against. `check_parity.sh` enforces it
directly; the per-port test suites enforce it piecewise.

```
simple_logger [options] <logfile> [message...]
```

Each message argument becomes one entry. With no message arguments, one entry is read
per line from stdin. The log file is opened for append and is created if missing.

### Entry format

```
[<timestamp>] [<LEVEL>] <message>\n
```

- The **space between fields and the newline after each entry are fixed.** They were
  options once (`-d`/`-s`), which is what the exercise asks for above. Being able to
  spell them on a command line meant every port carried an unescaper — a shell cannot
  portably pass a real newline, so `-s '\n'` had to mean one — and three
  implementations of the same four escapes had to be kept from drifting apart. Nothing
  was bought that a pipe through `sed` could not do, so the options went and the
  unescapers went with them.
- The **timestamp** is UTC ISO 8601 — `2026-07-30T18:22:05Z`. Always UTC: local time
  would need a timezone database in all three ports, which is a lot of machinery for a
  log line nobody greps by hour. It is read **once per run**, so every entry a single
  invocation writes shares it; otherwise a slow stdin pipe would spread one run's
  entries across several seconds.
- The **level** is given in lowercase (`debug`, `info`, `warning`, `error`) and
  written in uppercase (`[INFO]`). Uppercase is the syslog and log4j convention and
  makes `grep '\[ERROR\]'` unambiguous against ordinary prose.
- The **newline follows every entry, including the last**, so the next run starts on
  a fresh line.

### Options

| Option | Default | Meaning |
|---|---|---|
| `-l, --level LEVEL` | `info` | `debug`, `info`, `warning`, or `error` |
| `--no-timestamp` | off | omit the `[timestamp]` field and its trailing space |
| `--no-level` | off | omit the `[LEVEL]` field and its trailing space |
| `-h, --help` | — | show help |

`--level` takes those four spellings exactly: lowercase, no surrounding whitespace,
and never the numbers behind them. Each port gets that from its own parser — a
`strcmp` table in C, `CLI::IsMember` in C++, clap's `ValueEnum` in Rust — and the
three accept the identical set, which `bad_level` in `check_parity.sh` holds them to.

Options may appear anywhere, including after the logfile. `--` ends option parsing,
which is how a message beginning with `-` gets through.

### Examples

With the clock pinned to `2025-07-01T00:00:00Z`:

```
$ simple_logger app.txt "server started"
[2025-07-01T00:00:00Z] [INFO] server started

$ simple_logger --level error app.txt "disk full" "giving up"
[2025-07-01T00:00:00Z] [ERROR] disk full
[2025-07-01T00:00:00Z] [ERROR] giving up

$ printf 'a\nb\n' | simple_logger --level debug app.txt
[2025-07-01T00:00:00Z] [DEBUG] a
[2025-07-01T00:00:00Z] [DEBUG] b

$ simple_logger --no-timestamp --no-level app.txt bare
bare
```

### Edge cases

These are the decisions the ports would otherwise drift apart on, so each is settled
here and covered by `check_parity.sh`.

| Case | Behavior |
|---|---|
| Existing file with no trailing newline | Appended to as-is, joining the first entry onto that line. Probing would mean reading the file back, which `O_APPEND` alone does not require and a FIFO would not support. |
| CRLF on stdin | One trailing `\n` is stripped, then one trailing `\r`. A `\r` anywhere else is part of the message. |
| Blank stdin line | Logged as an entry with an empty message — N lines in, N entries out. |
| Empty stdin | No entries, exit 0, and the log file **is** created; the open happens before the read, like shell `>>`. |
| Message containing a newline | Written verbatim, so the entry spans physical lines. Not escaped and not split — escaping would need a matching unescaper on the reading side, and splitting would silently multiply the user's entry count. |
| Empty message argument | Logged, so the line ends with the space after `[LEVEL]`. Fields are unconditional; only `--no-*` removes them. |
| Embedded NUL and non-ASCII bytes | Preserved. |
| `-` as a logfile or message | Never special; it names a file called `-`. |
| Both a write and a close error | Reported as the write error, which names the earlier and more specific stage. |

### Known divergences

The rows above are guaranteed. These are **not**: each is a place where the three
parsers — `getopt_long`, CLI11, and clap — have different ideas about the same
spelling. None is covered by `check_parity.sh`, so treat the intersection of the
three as the supported surface and prefer the plain `--option value` form.

| Case | C | C++ | Rust |
|---|---|---|---|
| Non-UTF-8 **argv** | passes bytes through | passes bytes through | rejected (clap) |
| `-l=error` (`=` after a short option) | level is `=error`, **rejected** | level is `=error`, **rejected** | level is `error`, accepted |
| `--level error --level debug` (repeated option) | last wins | **rejected** | **rejected** |
| `--lev`, `--no-time` (abbreviated long option) | accepted | **rejected** | **rejected** |

Note also that **the exit code for a rejected argument is the parser's**, not the
contract's — `2` in C and Rust, one of CLI11's in C++ — which is why
`check_parity.sh` registers those cases with `run_case_parser_error` and requires
only that every port reject them.

Why each stands:

- **Non-UTF-8 argv** — clap hands arguments over as `String`. Getting the bytes
  through would mean taking `OsString` everywhere for a case that barely arises;
  stdin, the far likelier source of odd bytes, is byte-transparent in all three.
  The same kind of deliberate gap as `copy_file`'s `~user`.
- **`-l=error`** — clap strips a leading `=` after a short flag; `getopt_long`
  and CLI11 keep it, so those two see the level `=error` and reject it. (Attached
  values themselves — `--level=error`, `-lerror` — used to be on this list,
  rejected only by the hand-rolled C++ loop. CLI11 accepts them like the other
  two, so they are shared behavior now, with `attached_level` and
  `attached_short_level` in `check_parity.sh` to keep them that way.)
- **Repeated options** — clap 4's default action errors on a second occurrence
  and so does CLI11; only `getopt_long` overwrites. `#[arg(overrides_with_self)]`
  and `->take_last()` would restore last-wins on the two respectively, if this
  ever matters.
- **Abbreviations** — GNU `getopt_long` accepts any unambiguous prefix and offers
  no switch to turn that off, so removing it would mean hand-checking every token
  against the full option names before handing off.

### Exit codes

| Code | When |
|---|---|
| `0` | Success — and success is **silent**, unlike `copy_file`'s confirmation line. |
| `1` | An operation failed: opening, writing, or closing the log file, or reading stdin. |
| `2` | A usage error the program reports: missing or empty logfile, or a malformed `SIMPLE_LOGGER_FAKE_TIME`. |
| parser's own | A malformed command line — unknown option, bad `--level`. `2` in C and Rust; CLI11 picks its own in C++. |

### Testing the clock

Every port reads `SIMPLE_LOGGER_FAKE_TIME` — epoch seconds, matching `-?[0-9]+` — in
place of the system clock. It exists so `check_parity.sh` can compare three programs
whose output would otherwise depend on when they ran, and it is read only at each
port's CLI boundary; the libraries themselves take the time as an argument.

A malformed value is a hard error (exit 2), never a fallback to the real clock. The
fallback is the dangerous option: it would let the parity check pass while quietly
comparing three real clocks.
