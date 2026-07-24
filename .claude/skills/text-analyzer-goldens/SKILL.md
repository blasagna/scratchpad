---
name: text-analyzer-goldens
description: Regenerate the text_analyzer golden test outputs in testdata/ after an intentional behavior change to the analyzer. Use only when output has deliberately changed and all three ports (C / C++ / Rust) have been updated together; review the resulting diff before committing.
---

# Regenerate text_analyzer golden outputs

This wraps the existing scripts under `text_analyzer/testdata/` (their headers
document the same steps). Run only after a **deliberate** behavior change that you
have already applied to all three ports.

From the repo root (or from `text_analyzer/testdata/`):

```sh
# 1. Only if you added or changed a test *input* case:
./text_analyzer/testdata/make_inputs.sh   # rebuilds inputs from reviewable printf escapes; edit cases.txt too

# 2. Refresh the expected outputs:
./text_analyzer/testdata/regenerate.sh
```

`regenerate.sh`:

1. Builds all three ports (C and C++ via Bazel, Rust via `cargo build --release`).
2. Runs every case in `cases.txt` through all three ports under both configs
   (defaults and the alternate `--top-n 3 --max-word-len 5`), in text and JSON.
3. **Refuses to write anything unless all three ports agree** on every case, so a
   single-port bug can't be baked into the goldens. Only after all cases pass does it
   write the `.out` / `.json` / `.alt.out` / `.alt.json` files.

Afterward:

- **Review the git diff** — the outputs are committed and are the parity contract.
- The alternate config (`ALT_FLAGS` in `regenerate.sh`) must stay in sync with
  `kAltConfig` / `ALT_CONFIG` in the three golden tests (`//text_analyzer/{c,cpp}:test_golden`
  and `rust/tests/golden.rs`). If you change one, change all.

> Note: the steps above are copied from the `regenerate.sh` and `make_inputs.sh`
> headers, not synthesized — the scripts are the source of truth.
