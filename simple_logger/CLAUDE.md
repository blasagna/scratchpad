# simple_logger

A CLI that appends timestamped, level-tagged messages to a log file, implemented three
times with matching semantics: `c/` and `cpp/` (Bazel), `rust/` (cargo workspace member
`simple_logger`). The full contract — entry format, options, edge cases, exit codes —
is in [`README.md`](README.md); each port has its own README with design notes
([c](c/README.md), [cpp](cpp/README.md), [rust](rust/README.md)).

## Commands

```sh
bazel run  //simple_logger/c:simple_logger   -- <logfile> [message...]
bazel run  //simple_logger/cpp:simple_logger -- <logfile> [message...]
cargo run  -p simple_logger                  -- <logfile> [message...]

bazel test //simple_logger/c:test_logger
bazel test //simple_logger/cpp:test_logger
cargo test -p simple_logger

./simple_logger/check_parity.sh   # build all three, pin the clock, diff their log files
```

## Shared behavior (keep the ports in sync)

- **Entry format** is `[<timestamp>]<delim>[<LEVEL>]<delim><message><separator>`, with
  `--no-timestamp` / `--no-level` dropping a field *and its trailing delimiter*.
- **Timestamps are UTC ISO 8601 only** (`2026-07-30T18:22:05Z`) and the clock is read
  **once per run**, so every entry from one invocation shares it. Local time was
  considered and rejected: it would need a timezone database in all three ports for a
  log line nobody greps by hour.
- **All three accept exactly the four-digit years 0000-9999** and report a bad time
  outside them. The C port's `year < 0 || year > 9999` is the reference; the Rust port
  reproduces both ends, and
  `accepts_exactly_the_four_digit_years_the_other_ports_do` asserts them to the second.
- **The Rust port gets its dates from `jiff`; C and C++ do the arithmetic by hand.**
  Use `jiff::civil::DateTime`, **not `jiff::Timestamp`** — `Timestamp` reserves
  headroom for a timezone offset, so it stops at `9999-12-30T22:00:00Z`, *inside* the
  four-digit range, and silently loses the last 26 hours of year 9999 that the other
  two ports render fine. That bug shipped once already.
  `DateTime`'s `Display` zero-padding the year and omitting a zero fraction is what
  keeps the three in step; treat both as properties of `jiff` rather than guarantees.
  `renders_a_whole_second_without_a_fractional_part` in `rust/src/lib.rs` pins them,
  and it is the first test to check if a `jiff` upgrade makes `check_parity.sh` go red.
- **Levels** are lowercase on input, uppercase in the log (`[INFO]`).
- **The separator follows every entry, including the last**, so the next run appends
  onto a fresh line.
- **`--delimiter` / `--separator` accept exactly `\n`, `\t`, `\r`, `\\`.** Anything
  else, including a trailing lone backslash, is a usage error rather than a
  pass-through — that is what stops the accepted set from drifting between ports.
- **Options permute**: `simple_logger log.txt --level error msg` works everywhere.
  Every port gets this from its parser — `getopt_long` in C, CLI11 in C++, clap
  in Rust — and `--` ends option parsing in all three. Attached values
  (`--level=error`, `-lerror`) now work everywhere too; abbreviated long options
  still only work in C. See the divergences below.
- **stdin**: one entry per line; strip one `\n` then one `\r`; a blank line is an empty
  entry; a final line without a newline still logs; empty stdin writes nothing but
  still creates the file.
- **Bytes are preserved** — embedded NULs, non-ASCII, newlines inside a message. This
  is why the primitives take `(const char *, size_t)` / `std::string_view` / `&[u8]`
  rather than NUL-terminated strings. The exception is **argv in the Rust port**,
  which clap requires to be UTF-8 (exit 2); C and C++ pass those bytes through.
- **The ports do not agree on every argument spelling.** A repeated option and
  abbreviated long options still behave differently in at least one port, and
  `check_parity.sh` does not cover either — see the **Known divergences** table
  in [`README.md`](README.md). The intersection is the supported surface: plain
  `--option value`, given once. Do not add a shared-behavior claim about argument
  parsing without a parity case to back it.
- **`--level` is validated through `logger::parse_level`, not through
  `CLI::CheckedTransformer`.** The transformer is the obvious fit for mapping
  four names onto an enum and is wrong here: it also accepts the enum's
  underlying integers, so `--level 3` meant "error" in the C++ port and stayed a
  usage error in the other two. Same reasoning keeps `--delimiter`/`--separator`
  on `logger::unescape` rather than `CLI::EscapedString`, which additionally
  takes `\xNN`, `\uNNNN`, and octal.
- **Exit codes**: `2` usage, `1` operational, `0` success, for what the *program*
  reports — `missing <logfile>`, an empty logfile, a bad `SIMPLE_LOGGER_FAKE_TIME`.
  A bad *argument* is the parser's to report and carries its code: `2` in C and
  Rust, one of CLI11's in C++. `check_parity.sh` registers those cases with
  `run_case_parser_error`, which requires only that every port reject them.
  Success is **silent** —
  unlike `copy_file`, nothing is printed on the happy path, which is also what lets the
  parity script assert stdout is empty everywhere.

## Testing the clock

Every port honors `SIMPLE_LOGGER_FAKE_TIME` (epoch seconds, `-?[0-9]+`) instead of the
system clock. It exists so `check_parity.sh` can compare programs whose output would
otherwise depend on when they ran.

Two properties matter and are easy to break:

- **It is read only at the CLI boundary** — `log_clock_now` / `clock_now`. The pure
  `log_clock_resolve` / `resolve_clock` takes the override as an argument, so the
  libraries never read the environment and the tests never mutate it. Same shape as
  `copy_file`'s `expand_tilde` → `expand_tilde_with`.
- **A malformed value is a hard error (exit 2), never a fallback to the real clock.**
  A silent fallback would let `check_parity.sh` pass while comparing three real clocks.

## Gotchas

- **`bazel run` and relative paths.** `bazel run` executes from Bazel's runfiles
  directory, not your shell's cwd, so a relative logfile lands somewhere surprising.
  Pass an absolute path or run `bazel-bin/simple_logger/{c,cpp}/simple_logger`
  directly. `cargo run` is unaffected. (Same trap as `copy_file`.)
- **`.gitignore` swallows `*.log`.** Every example path, fixture, and file the parity
  script creates uses `.txt` so a committed one does not silently vanish.
- **Timestamp formatting is `snprintf`/`std::format`, not `strftime`.** glibc's `%Y`
  does not zero-pad a year below 1000; `jiff` pads unconditionally. Spelling the
  padding out in C and C++ is what keeps such dates in agreement.
- **The fake-time value is validated by hand as `-?[0-9]+`.** `strtoll` would also take
  leading whitespace and Rust's `parse` would take a `+` sign; C++'s `from_chars`
  happens to implement exactly the intended rule.
- **clap's `--separator` uses `default_value`, not `default_value_t`**, so `--help`
  shows `[default: \n]` rather than breaking its layout with a real newline.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
