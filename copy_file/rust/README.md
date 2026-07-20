# copy_file (Rust)

A `cp`-like utility that copies the contents of a source file to a destination,
written in idiomatic Rust. It takes the source and destination paths as command-line
arguments, creates or overwrites the destination, copies all bytes, and reports any
error clearly (naming the file and the underlying cause). See the top-level
`copy_file/README.md` for the full contract.

Path handling, matching the C and C++ ports:

- **Relative paths** are resolved against the current working directory.
- **A leading `~` / `~/…`** is expanded via `$HOME`. A `~user` prefix is intentionally
  **not** expanded (Rust's std has no password-database access and this crate takes no
  extra dependencies for it) — it is passed through unchanged so the open fails with a
  clear error. An unquoted `~` on the command line is expanded by the shell before the
  program runs anyway; this covers the quoted/literal case for a bare `~`.
- **A destination that is an existing directory** copies the source into it under the
  source's base file name, e.g. `copy_file a.txt somedir` writes `somedir/a.txt`. The
  success message shows the resolved destination.

## Design

The crate is split into a `copy_file` library and a thin CLI (`clap` derive):

- `copy_stream(reader, writer)` wraps `std::io::copy` over any `Read`/`Write`, so the
  core copy is unit-testable with in-memory `Cursor`s.
- `expand_tilde(path)` and `resolve_destination(dest, source)` perform the path
  transformations above; both are pure and unit tested.
- `copy(source, dest, method)` applies those transformations and copies, returning the
  resolved destination path. Errors are a `CopyError` enum (`OpenSource`, `OpenDest`,
  `Copy`, `SameFile`) implementing `std::error::Error`.

Two copy methods are available:

- **`CopyMethod::Stream`** (default) streams bytes with `std::io::copy` between open
  `File` handles — the idiomatic explicit open/read/write/close path.
- **`CopyMethod::Fs`** (the `--fs` flag) uses the one-shot `std::fs::copy`, mirroring the
  C++ port's `--fs`. Since `fs::copy` reports a single error without a stage, a failure is
  attributed to the source when it cannot be opened and to the destination otherwise.

Copying a file onto itself is refused (`SameFile`) before the destination is truncated —
both copy methods truncate first, which would otherwise destroy the source. The check
compares device + inode, so hard links and `./x` vs `x` are caught too.

## Build & run

```sh
cargo run -p copy_file -- <source> <destination>
cargo run -p copy_file -- --fs <source> <destination>   # use std::fs::copy
cargo test -p copy_file
```

Unlike `bazel run`, `cargo run` executes in your current working directory, so relative
path arguments resolve normally.

The program prints a confirmation on success. On failure it writes a clear message to
stderr and exits nonzero: `2` for a usage error (handled by `clap`), `1` for any copy
error.
