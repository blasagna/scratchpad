# copy_file (C)

A small `cp`-like utility that copies the contents of a source file to a
destination file. It takes the source and destination paths as command-line
arguments, creates or overwrites the destination, copies all bytes, and reports
any error clearly (naming the file and the underlying cause). See the top-level
`copy_file/README.md` for the full contract.

Path handling, matching `cp`:

- **Relative paths** are resolved against the current working directory.
- **`~` / `~user` prefixes** are expanded to the corresponding home directory.
  (An unquoted `~` on the command line is expanded by the shell before the
  program runs; this handles the quoted/literal case too.)
- **A destination that is an existing directory** copies the source into it
  under the source's base file name, e.g. `copy_file a.txt somedir` writes
  `somedir/a.txt`. The success message shows the resolved destination.

The logic is split into a `copyfile` library and a thin CLI:

- `copy_stream(src, dst)` copies every byte between two open `FILE*` streams,
  distinguishing read from write errors. It is stream-based so it can be unit
  tested with `fmemopen`.
- `copy_expand_tilde(path)` and `copy_resolve_dest(dst, src)` perform the path
  transformations above; both are pure and unit tested.
- `copy_path(src_path, dst_path)` applies those transformations, then opens,
  copies, and closes both files, returning a `CopyResult` that names the failing
  stage with `errno` preserved.

## Build & run

```sh
bazel run //copy_file/c:copy_file -- <source> <destination>
bazel test //copy_file/c:copyfile_test
```

The program prints a confirmation on success. On failure it writes a clear
message to stderr and exits nonzero: `2` for wrong argument count (usage), `1`
for any copy error.
