# matrix_ops (C++)

A CLI that adds, subtracts, multiplies, and scales 2D matrices of real numbers,
written in C++20 with the same semantics as the C port. See the top-level
`matrix_ops/README.md` for the full contract that both ports share, and
`../check_parity.sh` for the 85 cases that hold them to it.

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

All of this is about the numbers *inside* a matrix, which `matrix_io.cpp` still
parses itself. It no longer says anything about the numbers on the command
line: `--rows`, `--cols`, `--precision`, and `--scalar` are CLI11's to convert
now, and `from_chars` is not involved in this port at all — see
[The CLI](#the-cli).

This reverses the prediction in `matrix_ops/CLAUDE.md`, which had assumed
`from_chars` would be the C++ answer. The contract was right that the three
standard libraries disagree about `nan`/`inf`/signs; it was wrong about which
way to resolve it.

## The CLI

`main.cpp` declares its options to [CLI11](https://github.com/CLIUtils/CLI11)
and lets it do the parsing. Permutation, `--`, attached values (`--rows=2`,
`-r2`), and `--help` all come from the library; what the port keeps is the
contract around them.

**Dimensions bind to the next operand, in the order typed.** `--rows 2 --values
A --rows 3 --values B` shapes two operands differently, which means the port has
to know the order `--rows`, `--cols`, `--values`, and `--file` were interleaved
in — and a parser that hands back one result vector per option has thrown that
away. This is exactly where the Rust port gave up and switched to pairing by
index. CLI11 has the answer in `App::parse_order()`, which returns the options
in the order they were typed, one entry per occurrence; walking it with a cursor
into each option's `results()` replays the command line exactly, and the
existing pending-rows/pending-cols state machine goes on working unchanged.

**The option values are `CLI::Range`'s, grammar included.** `--rows`, `--cols`,
`--precision`, and `--scalar` are bound to `int`/`double` and checked with a
range; nothing here re-parses them. `parse_order()` then indexes the *converted*
vectors — `allow_extra_args(false)` gives each occurrence exactly one value, so
the Nth element of `rows_vals` is the Nth `--rows` typed.

`CLI::Range` is the right shape for this and `CLI::PositiveNumber` is not:
`PositiveNumber` is a `Range<double>`, so `--rows 2.5` would clear the check and
fail later in conversion, whereas a `Range<int>` is type-matched to the binding
and folds the `>= 1` and `<= INT_MAX` bounds into one test.

The cost is that CLI11's number grammar is wider than the contract's — base 0,
digit separators, surrounding whitespace — so this port accepts command lines
the C one refuses, which is
[tabulated in the area README](../README.md#known-divergence-argument-parsers)
because `check_parity.sh` can only assert agreement. **The one contract rule no
range can express is the exclusion of NaN**: `CLI::Range` tests
`val < min || val > max`, and both comparisons are false for a NaN, so a NaN
sits inside every range including `PositiveNumber`. `run()` rejects a NaN
`--scalar` by hand for that reason. Infinities need no help, being greater than
`DBL_MAX`.

**Diagnostics are CLI11's, and are no longer compared to the C port's.** That is
a deliberate loss. The two ports used to agree byte for byte on stderr, and
getting there had cost real work: `getopt_long` prefixes its own diagnostics
with `argv[0]`, which under Bazel is the full path to the binary, so
`matrix_ops --bogus` printed

```
/home/you/scratchpad/bazel-bin/matrix_ops/c/matrix_ops: unrecognized option '--bogus'
```

which can never match another port and puts a build path in a user-facing error.
The C port still sets `opterr = 0` and reports unknown options and missing
values itself, at exit 2; the C++ port now prints CLI11's wording and exits with
CLI11's codes. `check_parity.sh` compares stdout and exit status for everything
the *program* reports, and for the parser's own failures asserts only that both
ports reject the command line.

One case survives as a genuine disagreement rather than a wording difference:
`--help=x` is `error: option '--help' does not take a value` and exit 2 in C,
and a request for help and exit 0 under CLI11. It is recorded in
`../README.md` rather than in the script, which can only assert agreement.

## Build & run

```sh
bazel run  //matrix_ops/cpp:matrix_ops -- add --values "1 2 3" --values "4 5 6"
bazel test //matrix_ops/cpp:all
./matrix_ops/check_parity.sh          # both ports
```

`bazel run` executes from Bazel's runfiles directory, not your shell's, so pass
an absolute `--file` path (`"$PWD/a.txt"`) or run
`bazel-bin/matrix_ops/cpp/matrix_ops` directly.

Exit codes match the C port: `2` usage, `1` operational, `0` success.
