# matrix_ops (C)

A CLI that adds, subtracts, multiplies, and scales 2D matrices of real numbers.
Values come from the command line or from a file, and the shape is inferred from
their layout unless it is stated. See the top-level `matrix_ops/README.md` for the
full contract that all ports share.

## Design

The package is two libraries and a thin CLI:

- **`matrix`** — the `Matrix` type and the arithmetic. It knows nothing about
  text, files, or the command line: no function in it takes a `FILE *` or a
  `char *`, so every operation is testable by building matrices in memory and
  comparing elements.
- **`matrix_io`** — parsing and rendering, the only part that deals in text.
- **`main.c`** — argument handling, which composes the two.

Elements are `double` and stored row-major in one allocation. `matrix_at` and
`matrix_set` spell the indexing out so no caller repeats `r * cols + c`.

### Errors

A single `MatrixResult` enum spans both libraries, in the shape of
`simple_logger`'s `LogResult`: a nonzero value names what went wrong, and
`matrix_result_str` gives it a label. Only `MATRIX_ERR_READ` and
`MATRIX_ERR_WRITE` leave `errno` in place — everything else describes the values
or their shapes, where `errno` would be meaningless.

Every function that produces a matrix allocates it and writes `*out` **only on
success**, so `main` can zero-initialize its operands once and free them
unconditionally at a single `cleanup:` label, whichever of a dozen paths got
there. The same discipline lets `matrix_create` refuse a zero dimension outright,
which means nothing downstream has to reason about a `NULL` `data` pointer.

### Shape resolution

The interesting part of the port. `matrix_parse_text` splits into two steps that
are worth keeping separate:

- `scan_values` reads the values and reports the shape the *text* implies —
  non-blank lines are rows — and rejects ragged input on the spot, before any
  override is considered.
- `resolve_shape` is a pure function of five numbers: the value count, the
  layout's shape, and the two requested dimensions. All four cases (neither
  dimension, both, rows only, cols only) live in that one function, so the later
  ports have a single thing to reproduce.

Both size checks multiply, so both guard first: `resolve_shape` checks
`want_rows > SIZE_MAX / want_cols` before comparing the product to the value
count, because a wrapped product could equal the count and yield a matrix of
entirely the wrong shape. `matrix_create` does the same before calling `calloc`.

### Parsing values

`strtod` is the parser but not the whole rule. It also accepts `nan`, `inf`, and
`infinity`, and the set it accepts is not the set C++'s `from_chars` or Rust's
`parse::<f64>` accept. `parse_number` therefore rejects the non-finite results
explicitly, along with trailing junk, so the accepted set is written down here
rather than inherited from whichever standard library a port happens to use.

`ERANGE` deliberately does **not** mean failure: `strtod` sets it both for a value
too large to represent, which yields an infinity and is rejected by the
`isfinite` check, and for one that underflows to zero, which is a perfectly good
answer for input like `1e-400`. Testing the result rather than `errno` gets both
right.

`matrix_read_stream` buffers the whole input instead of streaming it. Unlike
`text_analyzer`, where streaming was a requirement, the values have to be held in
memory anyway. It rejects an embedded NUL, since the parser is NUL-terminated and
would otherwise silently read only the prefix.

### Formatting

`matrix_write` renders every element into its own string before printing
anything, because the widest one sets the column width they are all right-
justified into. Elements use `"%.*f"` followed by trimming trailing zeros and a
bare trailing `.`, rather than `"%g"`: `%g` counts significant digits instead of
decimals and switches to scientific notation for large values, neither of which
suits a column of numbers.

The negative-zero check is on the rendered string, not on the value, because
`-0` reaches the output two ways — the double `-0.0`, and a small negative value
the precision rounded away.

## Build & run

```sh
bazel run  //matrix_ops/c:matrix_ops -- add --values "1 2 3" --values "4 5 6"
bazel test //matrix_ops/c:all
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so pass an
absolute `--file` path (`"$PWD/a.txt"`) or run `bazel-bin/matrix_ops/c/matrix_ops`
directly.

On failure the program writes a message to stderr and exits `2` for a usage error
(unknown operation or option, wrong operand count, bad number, bad or ragged
shape, dimensions with no operand to attach to) or `1` for an operational one (a
file that cannot be opened or read, a failed write, out of memory).
