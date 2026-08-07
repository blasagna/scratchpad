# scratchpad

A personal monorepo of small programs and exercises to practice programming. Explore tools and patterns for different programming languages.

## Prerequisites

`valgrind` (used via `bazel test --config=valgrind`, see the per-area CLAUDE.md files)
requires the system package `libc6-dbg`: `sudo apt install libc6-dbg` on
Debian/Ubuntu. Without it, valgrind fails at startup ("a function redirection which is
mandatory for this platform-tool combination cannot be set up") because the system's
`ld-linux-x86-64.so.2` doesn't export `memcmp` in its dynamic symbol table, and
valgrind falls back to glibc debug info to find it.

The same `--config=valgrind` also works with `bazel run` for a `cc_binary`, since
`--run_under` applies to both commands:

```
bazel run //simple_logger/c:simple_logger --config=valgrind -- --help
```
