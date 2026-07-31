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

`format_timestamp` is `jiff::Timestamp::from_second(s)?.to_string()`. Rust's std has
no civil-date support at all — `std::time` offers `Instant`, `SystemTime`, and
`Duration`, and nothing that turns epoch seconds into a year, month, and day — so a
crate is the idiomatic answer rather than hand-rolled arithmetic. This port used to
carry Howard Hinnant's `civil_from_days` for exactly that reason; `jiff` does the same
job in one line, and delegating it is what a Rust codebase would actually do.

**The format lines up by luck, and the tests treat it as luck.** `Timestamp`'s
`Display` omits the fractional part when it is zero, so a whole number of seconds
renders as `YYYY-MM-DDTHH:MM:SSZ` — byte-for-byte what C and C++ assemble with
`snprintf` and `std::format`. That is a property of `jiff`, not of anything in
`lib.rs`, so `renders_a_whole_second_without_a_fractional_part` asserts it directly
and the known-date vectors pin the rest. A `jiff` upgrade that changed `Display` would
fail there rather than surfacing as a mystery diff in `check_parity.sh`.

**Negative epoch seconds are supported, not rejected** — `jiff` handles them and so
does glibc's `gmtime_r` in the other two ports, so parity holds with no guard on
either side. A time `jiff` cannot represent returns `None`; its range stops just
short of the year 10000, comfortably outside the four digits the other ports accept.

The C and C++ ports keep doing the arithmetic by hand, since neither has an
equivalent to reach for. That asymmetry is the point of the note in `lib.rs`.

### Deliberate divergence

Arguments must be valid UTF-8 in this port: `clap` gives messages and the logfile path
as `String`, so a non-UTF-8 argument is rejected with exit 2, where the C and C++ ports
would pass the bytes through. Stdin — the far likelier source of odd bytes — is
byte-transparent in all three. This is the same kind of documented, deliberate gap as
`copy_file`'s `~user`.

One clap detail worth keeping: `--separator` uses `default_value = "\\n"` with a
`value_parser`, **not** `default_value_t`. Routing the two-character `\n` through the
same unescaper as a user-supplied value makes `--help` print `[default: \n]` instead of
breaking its own layout with a real newline.

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
