# simple_logger (Rust)

A CLI that appends timestamped, level-tagged messages to a log file, written in
idiomatic Rust with the same semantics as the C and C++ ports. See the top-level
`simple_logger/README.md` for the full contract.

## Design

The crate is a `simple_logger` library plus a thin `clap` derive CLI, with the same
two seams as the other ports:

- **A stream seam.** `format_entry(fmt, timestamp, message) -> Vec<u8>` and
  `write_entry`/`write_messages`/`write_lines` are generic over `Write`, so the unit
  tests write into a `Vec<u8>`. `append_messages`/`append_lines` are the only
  functions that open a file.
- **A clock seam.** `resolve_clock(fake, real_now)` is pure and takes the override as
  an argument, so tests never mutate the process environment — the same shape as
  `copy_file`'s `expand_tilde` / `expand_tilde_with`. `clock_now()` is the one impure
  function and is called only from `main.rs`.

The byte-level API (`&[u8]`, not `&str`) is what keeps stdin transparent: `write_lines`
uses `read_until(b'\n')` rather than `lines()`, so a line containing a NUL or invalid
UTF-8 survives, and the trailing `\r` is stripped deliberately rather than silently
(`lines()` would strip it, `getline` in C would not — hand-stripping is why the ports
agree on CRLF input).

### Dates come from `jiff`

`format_timestamp` adds the epoch seconds to a `jiff::civil::DateTime` and appends a
`Z`. Rust's std has no civil-date support at all — `std::time` offers `Instant`,
`SystemTime`, and `Duration`, and nothing that turns epoch seconds into a year, month,
and day — so a crate is the idiomatic answer rather than hand-rolled arithmetic. This
port used to carry Howard Hinnant's `civil_from_days` for exactly that reason;
delegating to `jiff` is what a Rust codebase would actually do.

**Not `jiff::Timestamp`, which cannot do this job.** `Timestamp` reserves headroom for
a timezone offset, so its ceiling is `9999-12-30T22:00:00Z` — *inside* the four-digit
year range, not past it. The first version of this code used it and silently rejected
the last 26 hours of year 9999, which C and C++ log without complaint. The civil types
carry no offset and span the full range, which is why the boundaries are now asserted
to the second in `accepts_exactly_the_four_digit_years_the_other_ports_do`.

**Two `jiff` properties make the output line up, and the tests treat them as `jiff`'s,
not ours.** `DateTime`'s `Display` zero-pads the year to four digits and omits the
fractional seconds when they are zero, so a whole number of seconds renders as
`YYYY-MM-DDTHH:MM:SS` — byte-for-byte what C and C++ assemble with `snprintf` and
`std::format`, once the `Z` is appended. `renders_a_whole_second_without_a_fractional_part`
asserts both directly, so a `jiff` upgrade that changed `Display` fails there rather
than surfacing as a mystery diff in `check_parity.sh`.

**Negative epoch seconds are supported down to year 0000 and rejected below it.**
`jiff` would happily render year -1 as `-000001-12-31T23:59:59`, which is neither the
documented shape nor something the other ports accept, so `format_timestamp` guards
`year < 0` explicitly. The upper end needs no guard: `checked_add` already refuses
year 10000. Both ends match the C port's `year < 0 || year > 9999` exactly.

The C and C++ ports keep doing the arithmetic by hand, since neither has an
equivalent to reach for. That asymmetry is the point of the note in `lib.rs`.

`--level` is a `clap::ValueEnum` on `Level`, so clap both validates the four spellings
and renders them in `--help` as `[possible values: ...]`. `Level`'s `Display` is the
lowercase command-line spelling, which is what lets `default_value_t = DEFAULT_LEVEL`
print `[default: info]`; `label()` is the uppercase one written to the log.

### Deliberate divergence

Arguments must be valid UTF-8 in this port: `clap` gives messages and the logfile path
as `String`, so a non-UTF-8 argument is rejected with exit 2, where the C and C++ ports
would pass the bytes through. Stdin — the far likelier source of odd bytes — is
byte-transparent in all three. This is the same kind of documented, deliberate gap as
`copy_file`'s `~user`.

## Build & run

```sh
cargo run  -p simple_logger -- <logfile> [message...]
cargo test -p simple_logger
```

Unlike `bazel run`, `cargo run` executes in your current working directory, so a
relative logfile path resolves normally.

Success is silent. On failure the program writes a message to stderr and exits `2`
for a usage error (clap handles most of these; a malformed `SIMPLE_LOGGER_FAKE_TIME`
is the one raised by hand) or `1` for an operational one.
