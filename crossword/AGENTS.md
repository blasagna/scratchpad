# crossword

A terminal UI to solve crosswords with vim-style keys (ratatui + crossterm), in
Rust. It is a cargo workspace member. Puzzles come in three sizes, after the
NYT's Mini, Midi and Crossword, from free sources or from the NYT with the
player's own cookie. Usage, keys and sources are in [`README.md`](README.md).

```sh
cargo run  -p crossword
cargo run  -p crossword -- mini         # or midi, crossword
cargo test -p crossword
cargo test -p crossword --test live -- --ignored   # needs the network
```

## Must-knows

- **Every source converges on `PuzzleData`.** `Puzzle::new` derives the clue
  numbers and the entries from the grid. It matches clues to entries by
  direction and number. A source that places clues by position converts them
  with `numbering()` (see `grid_from_answers` in `sources/mod.rs`).
- **The NYT sources need the player's cookie.** `Fetcher` returns
  `SourceError::NeedsCookie` before it sends any request. Keep that gate. The
  NYT endpoints answered without a cookie in October 2026, but the Midi and
  the Crossword are subscriber puzzles.
- **AmuseLabs sources are out on purpose.** The LA Times Midi and the Vulture
  10×10 have the right size, but AmuseLabs obfuscates its data and computes
  tokens against scrapers. Do not reverse-engineer it.
- **Fixtures are synthetic.** The puzzles in `tests/fixtures/` are small grids
  written for the tests, in each publisher's format. Do not commit real puzzle
  data; it is copyrighted.
- **Only `Fetcher` touches the network.** It runs on a worker thread. `App`
  sends it `Request`s and receives `Response`s, so the tests drive `App` with
  key events and canned responses. The live tests in `tests/live.rs` check
  each source against its real endpoint.
- **Alt+key in insert mode is Esc and then the key.** Terminals send Alt
  chords as an Esc prefix, and a fast Esc and key arrive the same way. Without
  this rule, `Esc` and `q` sent together type a `Q`.
- **Marks use 256-colour indices.** Letters sit on light squares, and the 16
  named colours follow the terminal theme, which makes them too pale there.
- **Some Princetonian clues have broken accents** (`protÈgÈ`, `1700?C`). The
  API serves them that way, so do not debug the decoder for them.

## Layout

- `src/puzzle.rs` — the model: clue numbers, entries and their clues.
- `src/game.rs` — one puzzle's state: letters, cursor, marks, undo, timer.
- `src/app.rs` — screens and vim modes as a state machine (unit tested).
- `src/ui.rs` — draws the boxed or compact grid, the clue lists and the help.
- `src/store.rs` — the puzzle cache and progress files.
- `src/sources/` — `princetonian`, `guardian`, `universal`, `nyt`, the HTTP
  client and `Fetcher`.
- `src/main.rs` — CLI, terminal setup, the fetch thread and the event loop.
- `tests/render.rs` — draws each screen on a headless `TestBackend`.
- `tests/live.rs` — network tests, ignored by default.
