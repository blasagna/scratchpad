# copy_file

A `cp`-like utility that copies a source file to a destination, implemented three
times with matching semantics: `c/` and `cpp/` (Bazel), `rust/` (cargo workspace
member `copy_file`). The full contract is in [`README.md`](README.md); each port
has its own README with design notes ([c](c/README.md), [cpp](cpp/README.md),
[rust](rust/README.md)).

## Commands

```sh
bazel run  //copy_file/c:copy_file   -- <source> <destination>
bazel run  //copy_file/cpp:copy_file -- <source> <destination>   # --fs uses std::filesystem::copy_file
cargo run  -p copy_file              -- <source> <destination>   # --fs uses std::fs::copy

bazel test //copy_file/c:copyfile_test
bazel test //copy_file/cpp:test_copyfile
cargo test -p copy_file
```

## Shared behavior (keep the ports in sync)

- **Relative paths** resolve against the current working directory.
- **`~` / `~user`** expand to the corresponding home directory — **except the Rust
  port intentionally does not expand `~user`** (no password-db access, no extra
  deps): it passes it through so the open fails with a clear error. A bare `~` /
  `~/…` is expanded via `$HOME` in all ports.
- **A destination that is an existing directory** copies the source into it under
  the source's base name (`copy_file a.txt somedir` → `somedir/a.txt`); the success
  message shows the resolved destination.
- **Exit codes**: `2` for a usage error (wrong arg count / clap), `1` for any copy
  error; a confirmation is printed on success.
- The byte transfer is an explicit stream loop (`copy_stream`) so it is unit-testable
  with in-memory streams (`fmemopen` / `std::stringstream` / `Cursor`). C++ and Rust
  also offer a `--fs` method delegating to the stdlib one-shot copy. Rust refuses a
  file-onto-itself copy (`SameFile`, by device+inode) **before** truncating the dest.

## Gotcha: `bazel run` and relative paths

`bazel run` executes from Bazel's runfiles dir, not your shell's cwd, so **relative**
path arguments resolve from the wrong place. Pass absolute paths (e.g.
`"$PWD/copy_file/README.md"`) or run the built binary directly
(`bazel-bin/copy_file/cpp/copy_file <src> <dst>`). `cargo run` runs in your cwd and is
unaffected.

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`) is the
repo-wide convention from the root [`CLAUDE.md`](../CLAUDE.md).
