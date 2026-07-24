---
name: text-analyzer-bench
description: Benchmark the three text_analyzer ports (C / C++ / Rust) against each other and check they still agree byte-for-byte. Use when comparing port performance, verifying cross-port parity after a change, or reproducing the timing table in text_analyzer/README.md.
---

# Benchmark and parity-check the text_analyzer ports

This wraps the existing `text_analyzer/bench/run.sh` script (its header documents
the same steps). From the repo root:

```sh
./text_analyzer/bench/run.sh            # build, benchmark, print a markdown table
./text_analyzer/bench/run.sh --check    # parity check only, no timings
```

What it does:

1. Builds all three ports **optimized** — `bazel build --config=opt` for C and C++,
   `cargo build --release` for Rust. The default Bazel build is `fastbuild` (`-O0`),
   so timings taken without `--config=opt` are not comparable to `cargo --release`;
   the script handles this for you (don't add the flag again).
2. Generates two corpora (same size and word count, differing only in vocabulary
   size) under `${TMPDIR:-/tmp}/text_analyzer_bench` via `bench/gen_corpus.py`,
   skipping any that already exist.
3. Checks byte parity across all three ports in **both** text and JSON, diffing on
   failure.
4. Without `--check`, times each port (best of 5 runs) and prints the results table.

If parity fails, the ports have diverged — fix the offending port before trusting
any timings, and regenerate goldens with the **text-analyzer-goldens** skill.

> Note: the steps above are copied from the `bench/run.sh` header and body, not
> synthesized — the script is the source of truth.
