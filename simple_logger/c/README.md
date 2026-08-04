# simple_logger (C)

A CLI that appends timestamped, level-tagged messages to a log file. Messages come
from the command line, or one per line from stdin when none are given. See the
top-level `simple_logger/README.md` for the full contract that all three ports share.

## Design

The package is split into a `logger` library and a thin CLI, with two seams that keep
the library testable:

- **A stream seam.** `log_write_entry`, `log_write_messages`, and `log_write_lines`
  all take a `FILE *` rather than a path, so the tests exercise them through
  `fmemopen` with no filesystem involved. `log_open_append` and `log_close` are the
  only functions that touch a real file.
- **A clock seam.** `log_clock_resolve(fake, real_now, out)` is pure — it decides
  between the `SIMPLE_LOGGER_FAKE_TIME` override and the system clock without reading
  either. `log_clock_now` is the one impure function, called only from `main`.

There is deliberately no single `log_append_path()` covering the whole operation the
way `copy_file`'s `copy_path` does: argument messages and stdin lines have to share
one open handle, so the seam is `open → write* → close` and `main` composes it,
reporting a write failure in preference to a close failure.

`log_write_entry` takes `(const char *message, size_t message_len)` rather than a
NUL-terminated string. That is what makes the port byte-transparent — an embedded NUL
arriving on stdin is written out verbatim, matching the C++ `string_view` and Rust
`&[u8]` signatures.

Two details exist purely to keep the three ports in step:

- `log_format_timestamp` builds the timestamp with `snprintf("%04d-…")`, not
  `strftime("%Y-…")`. glibc's `%Y` does not zero-pad a year below 1000, and the Rust
  port pads unconditionally.
- `log_clock_resolve` checks the value matches `-?[0-9]+` by hand before calling
  `strtoll`, which would otherwise also accept leading whitespace and a `+` sign that
  the other ports reject.

Errors are a `LogResult` enum naming the stage that failed, with `errno` left in place
by the stages backed by a libc call (preserved across `fclose`/`free` with the usual
`int saved = errno` idiom) so `main` can pair the stage with `strerror`.
`log_write_lines` uses POSIX `getline`, which reports a byte count and so survives a
line containing a NUL.

## Build & run

```sh
bazel run  //simple_logger/c:simple_logger -- <logfile> [message...]
bazel test //simple_logger/c:test_logger
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so pass an
absolute logfile path (`"$PWD/app.txt"`) or run `bazel-bin/simple_logger/c/simple_logger`
directly.

Success is silent. On failure the program writes a message to stderr and exits `2`
for a usage error (missing or empty logfile, unknown option, bad `--level`,
malformed `SIMPLE_LOGGER_FAKE_TIME`) or `1` for an operational one (open, write,
close, or stdin read).
