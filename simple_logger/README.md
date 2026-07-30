
# simple logger

A mini project from the little book of c.

Build a CLI program that appends messages to a log file with timestamps

Requirements:
1. accept an agument for a message, or read from stdin if no arg is provided
1. append the message to log file given as an argument
1. prepend a timestamp to each message, wrapped in square brackets, followed by a delimeter, default to space
1. keep all previous entries; append not overwrite
1. allow multiple entries in one run, with a default unix newline separator
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
[<timestamp>]<delim>[<LEVEL>]<delim><message><separator>
```

- The **timestamp** is UTC ISO 8601 — `2026-07-30T18:22:05Z`. Always UTC: local time
  would need a timezone database that Rust's std does not have, and adding a date
  dependency to one port was not worth a friendlier log line. It is read **once per
  run**, so every entry a single invocation writes shares it; otherwise a slow stdin
  pipe would spread one run's entries across several seconds.
- The **level** is given in lowercase (`debug`, `info`, `warning`, `error`) and
  written in uppercase (`[INFO]`). Uppercase is the syslog and log4j convention and
  makes `grep '\[ERROR\]'` unambiguous against ordinary prose.
- The **separator follows every entry, including the last**, so the next run starts on
  a fresh line.

### Options

| Option | Default | Meaning |
|---|---|---|
| `-l, --level LEVEL` | `info` | `debug`, `info`, `warning`, or `error` |
| `-d, --delimiter STR` | `" "` | text between fields |
| `-s, --separator STR` | `"\n"` | text after each entry |
| `--no-timestamp` | off | omit the `[timestamp]` field and its delimiter |
| `--no-level` | off | omit the `[LEVEL]` field and its delimiter |
| `-h, --help` | — | show help |

`--delimiter` and `--separator` interpret exactly four escapes: `\n`, `\t`, `\r`, and
`\\`. A shell cannot portably hand a program a real newline, so `-s '\n'` has to mean
one. Any other escape, including a trailing lone backslash, is an error rather than a
pass-through — that way the accepted set cannot quietly drift between ports.

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

$ simple_logger -d ' | ' app.txt hi
[2025-07-01T00:00:00Z] | [INFO] | hi

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
| Empty message argument | Logged, so the line ends with a trailing delimiter. Fields are unconditional; only `--no-*` removes them. |
| Embedded NUL and non-ASCII bytes | Preserved. |
| `-` as a logfile or message | Never special; it names a file called `-`. |
| Both a write and a close error | Reported as the write error, which names the earlier and more specific stage. |

### Known divergences

The rows above are guaranteed. These are **not**: each is a place where the three
ports disagree, inherited from `getopt_long`, the hand-rolled C++ loop, and clap
having different ideas about the same spelling. None is covered by
`check_parity.sh` — which is exactly why they survived — so treat the intersection
of the three as the supported surface and prefer the plain `--option value` form.

| Case | C | C++ | Rust |
|---|---|---|---|
| Non-UTF-8 **argv** | passes bytes through | passes bytes through | exit 2 (clap) |
| `--level=error`, `-lerror` (attached value) | accepted | **exit 2** | accepted |
| `-d=` (empty attached value) | delimiter is `=` | **exit 2** | delimiter is empty |
| `--level error --level debug` (repeated option) | last wins | last wins | **exit 2** |
| `--lev`, `--no-time` (abbreviated long option) | accepted | **exit 2** | **exit 2** |

Why each stands:

- **Non-UTF-8 argv** — clap hands arguments over as `String`. Getting the bytes
  through would mean taking `OsString` everywhere for a case that barely arises;
  stdin, the far likelier source of odd bytes, is byte-transparent in all three.
  The same kind of deliberate gap as `copy_file`'s `~user`.
- **Attached values** — `getopt_long` and clap both accept them; the C++ port
  matches option tokens exactly, so it does not. `-d=` differs between C and Rust
  besides, because clap strips a leading `=` after a short flag and getopt does
  not.
- **Repeated options** — clap 4's default action errors on a second occurrence,
  where both C ports simply overwrite. `#[arg(overrides_with_self)]` would restore
  last-wins if this ever matters.
- **Abbreviations** — GNU `getopt_long` accepts any unambiguous prefix and offers
  no switch to turn that off, so removing it would mean hand-checking every token
  against the full option names before handing off.

### Exit codes

| Code | When |
|---|---|
| `0` | Success — and success is **silent**, unlike `copy_file`'s confirmation line. |
| `1` | An operation failed: opening, writing, or closing the log file, or reading stdin. |
| `2` | A usage error: missing or empty logfile, unknown option, bad `--level`, bad escape, or a malformed `SIMPLE_LOGGER_FAKE_TIME`. |

### Testing the clock

Every port reads `SIMPLE_LOGGER_FAKE_TIME` — epoch seconds, matching `-?[0-9]+` — in
place of the system clock. It exists so `check_parity.sh` can compare three programs
whose output would otherwise depend on when they ran, and it is read only at each
port's CLI boundary; the libraries themselves take the time as an argument.

A malformed value is a hard error (exit 2), never a fallback to the real clock. The
fallback is the dangerous option: it would let the parity check pass while quietly
comparing three real clocks.
