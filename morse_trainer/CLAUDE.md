# morse_trainer

A terminal UI (ratatui + crossterm) for practicing Morse code, in Rust. It's a
cargo workspace member. Full usage — menu, practice modes, screenshots — is in
[`README.md`](README.md).

```sh
cargo run  -p morse_trainer
cargo run  -p morse_trainer -- --dot a --dash s --timeout 400
cargo run  -p morse_trainer -- --mouse      # left button = dot, right = dash
cargo test -p morse_trainer
```

Defaults: dot = `j`, dash = `k`, quit = `q`, timeout = `300` ms.

## Must-knows

- **Two-key model, not press-and-hold.** Each character is keyed with two distinct
  inputs — a dot key and a dash key; a run of them forms one character, and a pause
  longer than the timeout ends it. This deliberately drops most of Morse's timing
  spec.
- **Two extensions** make text entry fully keyboard-driven: **space = `..--`**
  (dot-dot-dash-dash) and **backspace = `----`** (dash-dash-dash-dash). Keep these in
  the encode/decode table.
- **Letter table and lessons are ordered by Morse's dichotomic (binary-tree)
  structure** (E, T → I, A, N, M → …), not alphabetically — preserve that ordering
  when editing `lessons.rs` / the table in `morse.rs`.

## Layout

- `src/morse.rs` — dot/dash input model and the decode/encode table (with the
  space/backspace extensions).
- `src/lessons.rs` — progressive lessons and mixed phrases for prompted mode.
- `src/app.rs` — the terminal-independent state machine (fully unit tested).
- `src/ui.rs` — ratatui rendering of the app state.
- `src/main.rs` — CLI parsing and the crossterm event loop with timeout-based
  character segmentation.
- `tests/render.rs` — rendering tests.
