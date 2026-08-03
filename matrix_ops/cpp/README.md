# matrix_ops (C++)

A CLI that adds, subtracts, multiplies, and scales 2D matrices of real numbers,
written in C++20 with the same semantics as the C port. See the top-level
`matrix_ops/README.md` for the full contract that both ports share, and
`../check_parity.sh` for the 86 cases that hold them to it.

## Design

Everything lives in `namespace matrix_ops`, with file-local helpers in an
unnamed namespace. **Failures are returned as values, not thrown** — but the
allocations underneath are still the standard library's, so `Error::kNoMem`
exists to carry a `std::bad_alloc` back out as one of those values. It is
caught where the C port returns `MATRIX_ERR_NOMEM`: in `parse_text`,
`read_stream`, and `write` (function try-blocks), plus one backstop around
`main` for the result matrix. Without it, running out of memory left the
contract's exit-1-with-a-message as a `std::terminate` and a SIGABRT.

The package keeps the C port's two libraries and its seams, with the return
types adapted to what each operation can actually do wrong:

- **`add`, `sub`, `mul` return `std::optional<Matrix>`.** They fail exactly one
  way — a shape mismatch — so there is no reason to carry an error code the
  caller already knows.
- **`scale` returns a plain `Matrix`.** It cannot fail at all, which the C
  port's `MatrixResult` signature could not express.
- **`parse_text` / `read_stream` return a `ParseResult`**, shaped like
  `simple_logger`'s `LogResult`, because parsing fails in six distinct ways and
  the CLI has to name which one.

`Matrix` owns a `std::vector<double>`, which subsumes both the C port's
`calloc` and its hand-written `rows > SIZE_MAX / cols` overflow guard —
`max_size()` is the tighter bound. `create()` still rejects a zero dimension, so
nothing downstream reasons about a degenerate shape.

`render()` returns a `std::string` rather than writing to a stream, following
the improvement `simple_logger/cpp` made over its C sibling with
`format_entry`: most of the formatting tests become plain string comparisons
with no stream involved. `write()` is that call plus an `ostream::write`.

## Why `std::strtod` and not `std::from_chars`

`from_chars` is the natural C++ choice and is what `simple_logger/cpp` uses for
integers. For `double` it turned out to be the wrong tool here, because it
diverges from this area's contract in two places the C port's tests already
pin:

| Input | Contract (and C) | `std::from_chars` |
|---|---|---|
| `+3` | accepted, `3` | **rejected** — a sign is recognized only in the exponent |
| `1e-400` | accepted, `0` | **rejected** — `result_out_of_range` covers underflow too |

The contract wants `1e400` refused (it overflows to infinity) but `1e-400`
accepted (flushing to zero is a fine answer). `from_chars` reports the same
`result_out_of_range` for both and cannot tell them apart.

So this port calls `std::strtod` and applies the same guards the C one does:
reject trailing junk, then reject the result with `std::isfinite`. Testing the
*result* rather than `errno` is what keeps the underflow case accepted, since
`strtod` sets `ERANGE` for that too.

`from_chars` **is** still what parses `--rows`, `--cols`, and `--precision`,
where the values are integers and the two quirks above do not apply the same
way — but the accepted spelling is written down there too, for the same reason
in reverse. `strtol` skips leading whitespace and `from_chars` rejects a
leading `+`, so each port accepted something the other refused (`--rows " 2"`
in C, `--rows +2` in C++) until both were pinned to `+?[0-9]+`.

This reverses the prediction in `matrix_ops/CLAUDE.md`, which had assumed
`from_chars` would be the C++ answer. The contract was right that the three
standard libraries disagree about `nan`/`inf`/signs; it was wrong about which
way to resolve it.

## The CLI

`main.cpp` parses options with a hand-rolled loop rather than `getopt_long`,
but it **permutes the way GNU `getopt_long` does** — an option is recognized
wherever it appears, so the operation name may come before or after the
operands in both ports. `--` ends option parsing.

Writing this port also fixed a wart in the C one. `getopt_long`'s own
diagnostics are prefixed with `argv[0]`, which under Bazel is the full path to
the binary, so `matrix_ops --bogus` printed something like

```
/home/you/scratchpad/bazel-bin/matrix_ops/c/matrix_ops: unrecognized option '--bogus'
```

That can never match another port's wording, and a build path in a user-facing
error is worse than useless. The C port now sets `opterr = 0` and reports both
unknown options and missing values by hand, so both ports print

```
error: unknown option '--bogus'
```

The parity script is what surfaced this; it was the only case of the original
53 that failed.

Reproducing `getopt_long`'s *classification* of a bad option took a second
pass. An option written with an attached value is not automatically a known
option being misused: `--help=x` is, and gets `error: option '--help' does not
take a value`, but `--bogus=1` is simply unknown and `getopt_long` names the
whole argument, `=1` included. Testing for the attached value before checking
the name — which is what this port did first — described every unknown long
option as one that does not take a value. The check now lives inside the
`--help` branch, the only option that takes no value.

## Build & run

```sh
bazel run  //matrix_ops/cpp:matrix_ops -- add --values "1 2 3" --values "4 5 6"
bazel test //matrix_ops/cpp:all
./matrix_ops/check_parity.sh          # both ports, byte for byte
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so pass
an absolute `--file` path (`"$PWD/a.txt"`) or run
`bazel-bin/matrix_ops/cpp/matrix_ops` directly.

Exit codes match the C port: `2` usage, `1` operational, `0` success.
