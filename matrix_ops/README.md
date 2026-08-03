# matrix operations

A mini project from the little book of c.

Build a library, tests, and a CLI program that implements basic operations on 2D matrices.

Requirements:
1. take arguments for the matrix dimensions (rows and columns)
1. read matrix values from arguments or from a file
1. perform the requested matrix operation: addition, subtraction, multiplication, scalar multiplication
1. display results neatly formatted to stdout

## Contract

The behavior all ports implement. Only the C port (`c/`) exists so far; C++ and
Rust follow, and this is what they get checked against.

### CLI

```
matrix_ops <add|sub|mul|scale> [operand...] [options]
```

Each `--values` or `--file` introduces one operand, and any `--rows`/`--cols`
written *before* it describes that operand. That is what lets the two operands of
a product have different shapes. `add`, `sub`, and `mul` take two operands;
`scale` takes one plus `--scalar`.

| Flag | Meaning |
|---|---|
| `-v, --values "..."` | values separated by whitespace or newlines; closes an operand |
| `-f, --file PATH` | read values from a file, `-` for stdin; closes an operand |
| `-r, --rows N` | rows for the next operand (optional, `N >= 1`) |
| `-c, --cols N` | columns for the next operand (optional, `N >= 1`) |
| `-k, --scalar X` | the multiplier for `scale`; a finite number |
| `-p, --precision N` | decimal places in the output, `N >= 0` (default `4`) |
| `-h, --help` | show help, exit `0` |

The operation is the only positional argument and there must be exactly one;
option parsing permutes, so it may appear anywhere.

### Shape

Dimensions are optional and inferred by default:

1. A single non-blank line of values is a `1 x N` **row vector**; several
   non-blank lines are rows. Blank lines and surrounding whitespace are ignored,
   and a trailing newline is optional.
2. **Rows of differing length are always an error**, even when `--rows`/`--cols`
   would make the layout irrelevant.
3. Then the requested dimensions, if any, are applied:
   - neither given → the layout's own shape
   - **both** given → the values are reshaped row-major, and their count must be
     exactly `rows * cols`
   - **one** given → the other is derived, and it must divide the count evenly

So `--values "1 2 3 4 5 6"` is a `1x6` vector, `--rows 2 --cols 3` over the same
values is a `2x3` matrix, and `--rows 2` alone gets there too.

### Values

A value is anything the platform's `strtod` accepts — sign, decimal point,
exponent — **except** `nan`, `inf`, `infinity`, and anything that overflows to
infinity, all of which are errors. A value that underflows to zero is accepted.
An embedded NUL byte in a file is an error rather than a silent truncation.

### Output

One row per line, each line ending in a newline including the last. Every element
is rendered with a fixed number of decimals and then stripped of trailing zeros
and a bare trailing `.`, so integral values print as integers. The widest
rendering sets a common column width; all elements are right-justified into it
and separated by two spaces, so decimal points line up.

- Scientific notation is never used, at any magnitude. The column width follows
  the data, so a matrix holding `1e300` prints a very wide column.
- A negative zero always prints as `0`, whether it is the double `-0.0` or a
  small negative value the precision rounded away.
- Rounding is the ties-to-even rule `printf("%.*f")` and Rust's formatting share:
  `0.25` at one decimal is `0.2`, not `0.3`.

### Exit codes

`0` success, `2` usage error (unknown operation or option, wrong operand count,
bad number, bad or ragged shape, dimensions with no operand to attach to), `1`
operational error (a file that cannot be opened or read, a failed write, out of
memory).
