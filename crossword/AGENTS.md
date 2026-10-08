# crossword

An app to solve crosswords with vim-style keys, in Rust, with two frontends: a
terminal UI (ratatui + crossterm) and a desktop GUI (iced). Puzzles come in
three sizes, after the NYT's Mini, Midi and Crossword, from free sources or
from the NYT with the player's own cookie. The area is three workspace crates:
`core/` (`crossword_core`), `tui/` (`crossword_tui`, binary `crossword`) and
`gui/` (`crossword_gui`, binary `crossword-gui`). Usage, keys and sources are
in [`README.md`](README.md).

```sh
cargo run  -p crossword_tui -- mini         # or midi, crossword
cargo run  -p crossword_gui
cargo test -p crossword_core -p crossword_tui -p crossword_gui
cargo test -p crossword_core --test live -- --ignored   # needs the network
```

## Must-knows

- **All logic is in the core; the frontends only translate and draw.** The
  screens, the vim modes, the commands, the click actions (`click_cell`,
  `open_item`, `back` and the rest) and the saves are `App` methods in
  `core/src/app.rs`. A frontend converts its input into a core `Key` or one of
  those calls, and draws `App`'s state. Put new behaviour in the core and test
  it there, so both frontends get it.
- **The core does not depend on crossterm or iced.** Its `keys::Key` is the
  only input type. `tui/src/input.rs` and `gui/src/input.rs` convert to it.
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
- **Fixtures are synthetic.** The puzzles in `core/tests/fixtures/` are small
  grids written for the tests, in each publisher's format. Do not commit real
  puzzle data; it is copyrighted.
- **Only `Fetcher` touches the network, and it blocks.** The TUI runs it on
  one worker thread. The GUI runs each request on a thread of its own and
  awaits the answer, so the iced executor never blocks. `App` sends
  `Request`s and receives `Response`s, so tests answer by hand.
- **A panic in a fetch must not stop the TUI.** The worker thread catches
  it and sends `Request::failed`. The panic hook restores the terminal, so
  it ignores the thread named `fetch`. Without this, one bad puzzle leaves
  the terminal broken and every later request on "Loading…".
- **Alt+key in insert mode is Esc and then the key.** Terminals send Alt
  chords as an Esc prefix, and a fast Esc and key arrive the same way. Without
  this rule, `Esc` and `q` sent together type a `Q`.
- **The GUI needs iced's `smol` feature** for `iced::time::every`, which
  drives the timer. The default thread-pool executor has no timer.
- **The GUI saves before it closes.** `exit_on_close_request(false)` turns
  the close button into `Message::CloseRequested`, which saves and then exits.
- **GUI tests cannot see subscriptions.** `iced_test` drives widgets, but the
  keys arrive through `event::listen_with`. To test keys, send
  `Message::Key`, and test their conversion in `gui/src/input.rs`.
  `CROSSWORD_GUI_SNAPSHOTS=<dir>` with `ICED_TEST_BACKEND=tiny-skia` saves a
  PNG of each tested screen.
- **The TUI's mark colours are 256-colour indices.** Letters sit on light
  squares, and the 16 named colours follow the terminal theme, which makes
  them too pale there.
- **Some Princetonian clues have broken accents** (`protÈgÈ`, `1700?C`). The
  API serves them that way, so do not debug the decoder for them.

## Layout

- `core/src/puzzle.rs` — the model: clue numbers, entries and their clues.
- `core/src/game.rs` — one puzzle's state: letters, cursor, marks, undo, timer.
- `core/src/app.rs` — screens, vim modes and click actions as a state machine
  (unit tested).
- `core/src/keys.rs`, `core/src/help.rs` — the shared key type and key
  reference.
- `core/src/store.rs` — the puzzle cache and progress files.
- `core/src/sources/` — `princetonian`, `guardian`, `universal`, `nyt`, the
  HTTP client and `Fetcher`.
- `tui/src/ui.rs` — draws the boxed or compact grid, the clue lists and help.
- `tui/src/main.rs` — terminal setup, the fetch thread and the event loop.
- `gui/src/view.rs`, `gui/src/grid.rs` — the iced screens and the grid canvas.
- `gui/src/lib.rs` — `Gui`: the update loop, subscriptions and downloads.
- `core/tests/live.rs`, `tui/tests/render.rs`, `gui/tests/gui.rs` — live
  source tests, headless terminal tests and headless GUI tests.
