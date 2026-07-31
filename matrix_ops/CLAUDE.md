# matrix_ops

A CLI that adds, subtracts, multiplies, and scales 2D matrices of real numbers.
Only the C port (`c/`, Bazel) exists so far; C++ and Rust follow. The full
contract — CLI surface, shape rules, output format, exit codes — is in
[`README.md`](README.md), and the C port has its own design notes in
[`c/README.md`](c/README.md).

## Commands

```sh
bazel run  //matrix_ops/c:matrix_ops -- <add|sub|mul|scale> [operand...]

bazel test //matrix_ops/c:test_matrix
bazel test //matrix_ops/c:test_matrix_io
bazel test //matrix_ops/c:all
```

## Shared behavior (keep the ports in sync)

- **Dimensions are optional, and the layout is the default.** One line of values
  is a `1 x N` row vector; several lines are rows. `--rows`/`--cols` override, and
  a single one of them derives the other by division. The full rule set is in
  [`README.md`](README.md#shape) and lives in one function, `resolve_shape`, so a
  port has one thing to reproduce rather than a scattering of special cases.
- **Ragged input is an error even when `--rows`/`--cols` are given**, though the
  layout is otherwise ignored in that case. It is a deliberate asymmetry: a file
  whose rows differ in length is far more often a typo than a request to reshape.
- **`--rows`/`--cols` bind to the *next* operand, not the previous one**, and
  reset once it closes. Dimensions left over at the end of the command line are a
  usage error rather than silently ignored — otherwise a misplaced `--rows` would
  quietly produce a differently-shaped answer.
- **The accepted number set is written down, not inherited from `strtod`.**
  `nan`, `inf`, `infinity`, and an overflow to infinity are rejected; an underflow
  to zero is accepted. Pinning it here is what will keep C++'s `from_chars` and
  Rust's `parse::<f64>` — which disagree with `strtod` and with each other about
  those spellings — on one rule. Same reasoning as `simple_logger`'s hand-checked
  `-?[0-9]+`.
- **Output is `printf("%.*f")` with trailing zeros trimmed, never `%g`.** `%g`
  counts significant digits rather than decimals and switches to scientific
  notation for large values, which reads badly in a column. A port reaching for
  its own "shortest representation" formatter will diverge here.
- **Rounding ties go to the even digit** (`0.25` at one decimal is `0.2`).
  `printf` and Rust's formatter both do this;
  `BreaksARoundingTieTowardsTheEvenDigit` in `c/test_matrix_io.c` pins it, and it
  is the first test to check if a port's output starts drifting in the last
  place.
- **A negative zero always prints as `0`.** It arrives two ways — the double
  `-0.0` from `scale --scalar 0`, and a small negative rounded away by the
  precision — so the check is on the *rendering*, not on the value.
- **Every line of output ends in a newline, including the last.**
- **Exit codes**: `2` usage, `1` operational, `0` success. A dimension mismatch is
  a usage error (`2`), since it always traces back to what was typed.

## Gotchas

- **`bazel run` and relative paths.** `bazel run` executes from Bazel's runfiles
  directory, not your shell's cwd, so a relative `--file` path resolves somewhere
  surprising. Pass an absolute path or run `bazel-bin/matrix_ops/c/matrix_ops`
  directly. (Same trap as `copy_file` and `simple_logger`.)
- **`valgrind` does not run on this machine.** The pixi-provided build fails at
  startup against the system's stripped `ld.so` ("a function redirection which is
  mandatory for this platform-tool combination cannot be set up"). Use the
  sanitizers instead:

  ```sh
  bazel test //matrix_ops/c:all --config=permissive \
    --copt=-fsanitize=address --copt=-g --linkopt=-fsanitize=address
  ```

C-test-with-GoogleTest wrapping (`extern "C"` + `copts = ["-x", "c++"]`), strict
warnings, and formatting are repo-wide conventions from the root
[`CLAUDE.md`](../CLAUDE.md).
