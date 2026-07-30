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

### No date dependency

Timestamps are computed in-crate rather than with `chrono` or `time`.
`civil_from_days` is Howard Hinnant's `civil_from_days`, the inverse of
`days_from_civil`: it shifts the epoch to 0000-03-01 so a leap day always lands at the
end of a 400-year era, which removes every month-length and leap-year branch and fits
in about ten lines. `format_timestamp` splits the day with `div_euclid`/`rem_euclid`
rather than `/` and `%`, which truncate toward zero and would put a negative timestamp
on the wrong day.

**Negative epoch seconds are supported, not rejected** — Euclidean division makes it
free, and glibc's `gmtime_r` handles them in the other two ports, so parity holds with
no guard on either side. A year outside four digits has no agreed rendering across the
ports and returns `None`.

Known-date vectors pin the common cases, but the test that would actually catch an
off-by-one in the era arithmetic is `walks_every_day_from_1970_to_2070_without_gaps`,
which steps a century one day at a time against an independent `is_leap` /
`days_in_month` oracle defined in the test module.

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
