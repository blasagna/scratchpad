# morse_trainer (Rust)

A terminal UI for practicing Morse code. Following the model of the
[`~/code/morse`](https://github.com/) project, you key each character with **two
distinct inputs** — a dot key and a dash key — rather than press-and-hold
durations. A pause longer than the timeout ends the character. This removes most
of Morse's timing specifications and makes the code easier to learn and type.

## Build & run

```sh
cargo run -p morse_trainer
cargo run -p morse_trainer -- --dot a --dash s --timeout 400
cargo run -p morse_trainer -- --mouse          # left button = dot, right = dash
cargo test -p morse_trainer
```

Defaults: dot = `j`, dash = `k`, quit = `q`, timeout = `300` ms.

## How to use

The app opens on a menu (navigate with `↑`/`↓`, select with `⏎`, quit with `q`):

- **Free text entry** — key freely; each decoded character is appended to a
  buffer. Great for warming up.
- **Prompted practice** — reproduce a given prompt one character at a time.
  Choose either a **mixed phrases** track or one of the **progressive lessons**.
  Correct characters turn green, mistakes red, and the current position is
  underlined. Press `Tab` to reveal the Morse for the next expected character.

Press `Esc` to return to the menu.

### Keying

- A **dot** and a **dash** are separate keys; a run of them forms one character.
- After the timeout of quiet, the character is decoded and applied.
- Two extensions to standard Morse make text entry fully keyboard-driven:
  - **space** = `..--` (dot-dot-dash-dash)
  - **backspace** = `----` (dash-dash-dash-dash)

## Learning Morse code

There are many resources for learning Morse code; one good one is
[Hello Morse](https://experiments.withgoogle.com/collection/morse).

The letter table and the progressive lessons are ordered by Morse's **dichotomic
(binary-tree) structure** — the shortest, most common characters first
(E, T → I, A, N, M → S, U, R, W, D, K, G, O → …). Working through the lessons in
order introduces letters the way the code itself is built up, similar in spirit
to the Koch method.

## Layout

- `src/morse.rs` — the dot/dash input model and the decode/encode table
  (a Rust port of the `~/code/morse` tables), with the space/backspace extensions.
- `src/lessons.rs` — progressive lessons and mixed phrases for prompted mode.
- `src/app.rs` — the terminal-independent state machine (fully unit tested).
- `src/ui.rs` — ratatui rendering of the app state.
- `src/main.rs` — CLI parsing and the crossterm event loop with timeout-based
  character segmentation.
