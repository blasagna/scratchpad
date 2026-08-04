# simple_logger (C++)

A CLI that appends timestamped, level-tagged messages to a log file, written in
idiomatic C++20 with the same semantics as the C and Rust ports. See the top-level
`simple_logger/README.md` for the full contract.

## Design

Everything lives in `namespace logger`, with file-local helpers in an unnamed
namespace. **No exceptions**: failures are returned as values, either a
`std::optional` for the pure functions or a `LogResult { LogStage stage;
std::error_code ec; }` for the ones that touch the filesystem.

The port has the same two seams as the C one, but a purer version of the first:

- **`format_entry(fmt, timestamp, message) -> std::string`** renders an entry with no
  stream at all, so most of the tests are plain string comparisons.  `write_entry` is
  that call plus an `ostream::write` and a `good()` check, and `write_lines` /
  `write_messages` build on it.
- **`resolve_clock(fake, real_now)`** is pure; `clock_now()` is the one function that
  reads the environment and the system clock, and it is called only from `main.cpp`.

Idiom differences from the C port, beyond the namespace and `enum class` spellings:
`std::filesystem::path` and `std::string_view` instead of `char *`, RAII `ofstream`
instead of `fopen`/`fclose`, `std::from_chars` instead of `strtoll`, and
`std::span<char *>` over `argv` instead of index arithmetic. Messages are
`std::string_view`, which carries its own length and so keeps embedded NULs intact.

`main.cpp` parses options with CLI11, which permutes the way GNU `getopt_long` does —
an option is recognized wherever it appears, so `simple_logger log.txt --level error
msg` behaves identically in all three ports — and writes `--help` and rejects unknown
options on its own. `--` ends option parsing.

**`--level` is checked with `CLI::IsMember`, and that is the whole validation.** It is
the one constrained option here, and CLI11 can own its grammar outright: the option is
bound to a `std::string`, so no conversion runs on the value, and `IsMember` compares
the bytes as typed — no case folding, no trimming — which is exactly the C port's
`strcmp` against the same four names. The set itself comes from `logger::kLevelNames`
rather than being respelled here, and `--help` renders it as
`TEXT:{debug,info,warning,error}`.

The one CLI11 spelling that would *not* have matched is the obvious one:
`CLI::CheckedTransformer` maps names onto an enum in a single step, but it also accepts
the enum's underlying integers, so `--level 3` would mean `error` here and stay a usage
error in the other two ports. Elsewhere in the repo — `text_analyzer`, `simple_logger`
before this — that kind of gap is what the hand-written validators behind `->check()`
exist to close; `--level` no longer needs one.

Two details keep the ports in step: `format_timestamp` composes the string with
`std::format("{:04}-…")` rather than `strftime("%Y-…")`, since glibc's `%Y` does not
zero-pad a year below 1000; and `std::from_chars` happens to accept exactly the
`-?[0-9]+` the other two ports check for by hand, rejecting the leading whitespace and
`+` sign that `strtoll` and Rust's `parse` would each let through.

## Build & run

```sh
bazel run  //simple_logger/cpp:simple_logger -- <logfile> [message...]
bazel test //simple_logger/cpp:test_logger
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so pass an
absolute logfile path or run `bazel-bin/simple_logger/cpp/simple_logger` directly.

Success is silent. On failure the program writes a message to stderr and exits `2`
for a usage error or `1` for an operational one.
