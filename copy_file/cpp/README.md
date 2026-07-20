# copy_file (C++)

A `cp`-like utility that copies the contents of a source file to a destination,
written in modern C++20. It takes the source and destination paths as command-line
arguments, creates or overwrites the destination, copies all bytes, and reports any
error clearly (naming the file and the underlying cause). See the top-level
`copy_file/README.md` for the full contract.

Path handling, matching `cp`:

- **Relative paths** are resolved against the current working directory.
- **`~` / `~user` prefixes** are expanded to the corresponding home directory. (An
  unquoted `~` on the command line is expanded by the shell before the program runs;
  this handles the quoted/literal case too.)
- **A destination that is an existing directory** copies the source into it under the
  source's base file name, e.g. `copy_file a.txt somedir` writes `somedir/a.txt`. The
  success message shows the resolved destination.

## Design

The copy itself is an explicit stream-based byte loop (`copy_stream`) rather than
`std::filesystem::copy_file`, to satisfy the exercise's open/read/write/close contract.
`<filesystem>` is used for the path work it does well — directory detection, base-name
extraction, and path joining. Errors are reported via a `CopyResult` value (no
exceptions), carrying the failing `CopyStage`, an OS `std::error_code` for open
failures, and the fully resolved destination path.

The logic is split into a `copyfile` library and a thin CLI:

- `copy_stream(src, dst)` copies every byte between two `std::iostream`s,
  distinguishing read from write errors. It is stream-based so it can be unit tested
  with `std::stringstream`.
- `expand_tilde(path)` and `resolve_destination(dst, src)` perform the path
  transformations above; both are pure and unit tested.
- `copy(source, dest)` applies those transformations, then opens, copies, and closes
  both files, returning a `CopyResult`.

### Alternative: `copy_fs`

`copy_fs(source, dest)` is an alternative that delegates the byte transfer to
`std::filesystem::copy_file` (with `overwrite_existing`). It reuses the same
`expand_tilde` and `resolve_destination`, so relative paths, `~` expansion, and
directory destinations behave identically — only the copy step differs. Because
`copy_file` returns a single `error_code` without a stage, a failure is attributed to
the source when it is not a regular file and to the destination otherwise (it never
reports a distinct read/write error). The CLI selects it with the `--fs` flag.

## Build & run

```sh
bazel run //copy_file/cpp:copy_file -- <source> <destination>
bazel run //copy_file/cpp:copy_file -- --fs <source> <destination>   # std::filesystem::copy_file
bazel test //copy_file/cpp:test_copyfile
```

`bazel run` executes the binary from Bazel's runfiles directory, not your shell's
working directory, so **relative** path arguments are resolved from the wrong place.
Pass absolute paths (e.g. `"$PWD/copy_file/README.md"`) with `bazel run`, or run the
built binary directly (`bazel-bin/copy_file/cpp/copy_file <src> <dst>`) to use
relative paths.

The program prints a confirmation on success. On failure it writes a clear message to
stderr and exits nonzero: `2` for wrong argument count (usage), `1` for any copy error.
