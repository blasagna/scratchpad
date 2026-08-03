# matrix_ops — Rust port

The third port, after [`c/`](../c) and [`cpp/`](../cpp). The contract — shape
rules, output format, exit codes — is in [`../README.md`](../README.md).

```sh
cargo run -p matrix_ops -- add --values "1 2 3" --values "4 5 6"
cargo test -p matrix_ops
```

## What this port does not do

The C and C++ ports agree byte for byte on every stream, including stderr, and
[`../check_parity.sh`](../check_parity.sh) holds them there across 86 cases.
**This port is not in that script.** It uses `clap` the ordinary way and accepts
clap's behavior wherever it differs.

That is a deliberate trade, and the cost is worth writing down:

| | C and C++ | Rust |
|---|---|---|
| usage diagnostics | `error: unknown option '--x'`, hand-written | clap's wording |
| `--help` | a hand-written 40-line text | clap's rendering |
| dimensions | bind to the *next* operand, in the order typed | the Nth `--rows` describes the Nth operand |
| mixed operand sources | interleaved in the order typed | inline operands ordered before file ones |
| hex floats (`0x1p3`) | accepted, via `strtod` | rejected |
| out of memory | `matrix_ops: out of memory`, exit 1 | the process aborts |

Everything else matches, and the parts that matter to someone actually using the
tool match exactly: the four operations, the shape rules, and the rendering.
Spot-checking the two binaries against each other on the happy paths finds no
difference:

```sh
bazel build //matrix_ops/c:matrix_ops && cargo build -p matrix_ops
diff <(bazel-bin/matrix_ops/c/matrix_ops add --values "1 22" --values "333 4444") \
     <(target/debug/matrix_ops           add --values "1 22" --values "333 4444")
```

## The interesting divergences

**Dimensions pair by index.** In C, `--rows`/`--cols` bind to the next operand
and reset when it closes, so the command line is an ordered stream rather than a
set of flags. Reconstructing that under clap means going through
`ArgMatches::indices_of` — its indices come from one counter shared by every
argument, so sorting on them recovers the typed order — and then folding the
result. Pairing by index instead is a fraction of the code and reads like an
ordinary clap program.

The practical effect is smaller than it sounds: written the way C requires, with
each dimension before the operand it describes, both ports produce identical
output. The divergence only shows up in spellings C rejects outright, plus one
real case — `mul --file b.txt --values "…"` multiplies the *inline* operand
first, because sources are grouped rather than interleaved.

**Hex floats.** The contract says a value is "anything `strtod` accepts", which
includes C99 `0x1p3`. `f64::from_str` rejects those. Hand-rolling hex-float
parsing carries a rounding-correctness burden for a syntax nobody types into a
matrix, so this port rejects them and says so. This is the question
[`../CLAUDE.md`](../CLAUDE.md) left open for "the Rust port": use
`parse::<f64>()` with an `is_finite()` guard, which lands every other case right
— `+3`, `.5`, `4.`, `1e-400` → `0`, and `1e400`/`inf`/`nan` rejected.

**Out of memory.** The C port returns `MATRIX_ERR_NOMEM` and the C++ one catches
`std::bad_alloc`; Rust aborts. Matching would mean `try_reserve` at the read
buffer, the value `Vec`, and the matrix storage, and an infallible `format!` in
the renderer would still abort. Not worth threading through for a case
`check_parity.sh` reaches only under `ulimit -v`.

## Design

`matrix.rs` and `matrix_io.rs` mirror the C port's split. Two things differ:

- **`render` returns a `String`** rather than writing to a stream, following the
  C++ port's improvement over C. Most formatting tests become plain string
  comparisons.
- **`parse` moves its value `Vec` into the `Matrix`** instead of copying, since
  `resolve_shape` has already proved the count is exactly `rows * cols`. One
  fewer pass than C's `memcpy`.

`Matrix::mul` is the naive triple loop in the same `i`/`j`/`k` order as the C and
C++ ports. That is deliberate: [`../bench/rust`](../bench/rust) measures it
against faer and nalgebra, and reordering to the cache-friendlier `i`/`k`/`j`
would make the Rust column faster for reasons that have nothing to do with the
language.

`Error` is a plain enum with a hand-written `Display`, matching the rest of the
repo — no `anyhow` or `thiserror`. It carries the numbers behind each failure so
the CLI can report them without re-deriving anything.
