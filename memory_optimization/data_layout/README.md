# data layout

Reproduces two data-structure levers from **§6.2.1, "Optimizing Level 1 Data
Cache Access"**: hot/cold field splitting, and alignment. It also carries the
paper's `pahole` example.

## What the paper shows

**Hot/cold splitting.** The paper's `struct order { double price; bool paid;
const char *buyer[5]; long buyer_id; }` bundles a hot field a billing job reads
(`price`/`paid`) with cold fields it doesn't (`buyer`, `buyer_id`). Iterating an
array of these to total the outstanding payments drags the cold fields through
the cache, so each useful value costs most of a cache line — "up to 5 times worse
than it could be." Storing the hot fields in their own compact array packs
several per line.

**Alignment.** An access that straddles a cache-line boundary touches two lines
instead of one (Figure 6.4, "Overhead of Unaligned Accesses"). Naturally aligned
data keeps each access on one line. The paper measures roughly a 300% penalty for
L2-resident working sets and 20–30% for very large ones.

**pahole.** The `pahole` tool reads a binary's debug info and prints struct
layout with the holes and cache-line boundaries marked, so you can *see* where
padding lands and which fields share a line — and, with `--reorganize`, proposes a
field order that packs the holes away.

## This demo

- `total_due_fat` (over the paper's `Order`) vs `total_due_split` (over a packed
  `HotOrder{price,paid}`) in [`data_layout.hpp`](data_layout.hpp).
- `SlotBuffer` increments 64-bit slots laid out aligned inside a line
  (`offset 0`) or straddling the 64-byte boundary (`offset 60`).

```sh
bazel run -c opt //memory_optimization/data_layout:bench_data_layout
```

## Results on this machine (4M orders; fat = 256 MiB, hot = 64 MiB)

| | time | throughput |
|---|---|---|
| `total_due_fat`   | 5.70 ms | 0.70 G orders/s |
| `total_due_split` | 1.50 ms | 2.67 G orders/s |

The split layout is **~3.8×** faster — the paper's "up to 5×." For alignment, the
straddling walk is ~18% slower than the aligned one at this (large) working-set
size, consistent with the paper's 20–30% band for large sets; Zen 5 handles
unaligned accesses well, so the effect is at the low end.

## Using pahole

`pahole` is installed here (Debian/Ubuntu package `dwarves`; this box has v1.25).
It reads DWARF, so the binary has to carry real debug info: build `-c dbg`, since
the default `fastbuild` emits line tables only and pahole will not find the type
at all.

```sh
bazel build -c dbg //memory_optimization/data_layout:bench_data_layout
pahole -C Order bazel-out/k8-dbg/bin/memory_optimization/data_layout/bench_data_layout
```

**Use that `bazel-out/k8-dbg/...` path, not `bazel-bin/...`.** The `bazel-bin`
convenience symlink tracks whichever configuration Bazel built *last*, so the
`bazel run -c opt` above re-points it at `k8-opt` and any `bazel test` re-points
it at `k8-fastbuild` — after either, pahole reads a binary with no full debug
info, finds nothing, and prints nothing at all (exit 1, no message saying why). The
`bazel-out/k8-dbg/` path names the debug configuration outright and keeps
working whatever you built last. `bazel cquery -c dbg --output=files <target>`
prints it if you would rather not hardcode it.

**The type is `Order`, not the paper's `order`.** `-C` matches the C++ identifier
as written, and this repo spells structs in CamelCase; only the *layout* follows
the paper. `pahole -C order` prints `type 'order' not found`. The enclosing
`memory_optimization::data_layout` namespace does not need to be spelled out —
pahole matches on the bare name.

```
struct Order {
	double                     price;                /*     0     8 */
	bool                       paid;                 /*     8     1 */

	/* XXX 7 bytes hole, try to pack */

	const char  *              buyer[5];             /*    16    40 */
	long int                   buyer_id;             /*    56     8 */

	/* size: 64, cachelines: 1, members: 4 */
	/* sum members: 57, holes: 1, sum holes: 7 */
};
```

That is the paper's point made visible: 64 bytes — exactly one cache line — to
carry the 9 bytes of `price` + `paid` that the billing job actually reads. The
same command on the split struct shows the hot half at 16 bytes, ~4 per line:

```sh
pahole -C HotOrder bazel-out/k8-dbg/bin/memory_optimization/data_layout/bench_data_layout
```

**`--reorganize` finds nothing to save here, and that is the expected result:**

```sh
pahole --reorganize -C Order bazel-out/k8-dbg/bin/memory_optimization/data_layout/bench_data_layout
```

It reprints the struct unchanged with no `/* Saved N bytes! */` line. Moving
`paid` past the pointer array only relocates the padding — 8 + 40 + 8 + 1 rounds
back up to 64 either way, because `double` forces 8-byte alignment on the whole
struct. Reordering cannot help; that is precisely why the paper reaches for
*splitting* the struct instead, and why the benchmark above measures a split
rather than a reorder.

**Expect some stderr noise.** pahole v1.25 cannot model C++ templates and
complains once per offending DWARF DIE:

```
die__process_class: tag not supported 0x2f (template_type_parameter)!
tag__recode_dwarf_type: couldn't find 0xfa9f type for 0x54db (imported_declaration)!
```

These come from the libstdc++ and Google Benchmark debug info linked into the
binary, they go to stderr, and the struct output on stdout is unaffected —
append `2>/dev/null` if it is in the way. A wrong `-C` name is the case that
really floods, since pahole then scans every compilation unit instead of
stopping at the first match.

`test_data_layout.cpp` pins `sizeof(Order) == 64` (one cache line) and
`sizeof(HotOrder) == 16` (~4 per line), so the pahole output stays as the paper
describes.

## Modern C++ vs the paper

- `Order` is laid out exactly as the paper's `struct order` so its pahole output
  matches. Splitting produces a second array rather than a pointer-linked
  side-structure.
- The unaligned read-modify-write uses `std::memcpy` (defined in C++, lowered to a
  single unaligned `mov` on x86) where the paper dereferences a misaligned
  pointer directly. The paper's `posix_memalign` is not needed here — the
  straddle is created by an explicit byte offset, not by requesting alignment.
