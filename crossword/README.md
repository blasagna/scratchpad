# crossword (Rust)

A terminal UI to solve crosswords with vim-style keys, built on ratatui and
crossterm. Puzzles come in three sizes, after the NYT's Mini, Midi and
Crossword. Free sources supply all three. NYT puzzles work too, with your own
subscription cookie.

## Build and run

```sh
cargo run -p crossword                 # start on the size menu
cargo run -p crossword -- mini         # open the Mini list (also: midi, crossword)
cargo run -p crossword -- --no-save    # keep no cache and save no progress
cargo test -p crossword                # offline tests
cargo test -p crossword --test live -- --ignored   # live tests against each source
```

## Puzzle sources

| Size | Free source | Grid | List | With an NYT cookie |
|------|-------------|------|---------|--------------------|
| Mini | Daily Princetonian Mini | 5×5 or 7×7 | the whole archive, about 280 | NYT Mini |
| Midi | Guardian Quick | 13×13 | the latest 20 | NYT Midi |
| Crossword | Universal | 15×15 | the last 60 days | NYT Crossword |
| Crossword | Daily Princetonian | 15×15 | the whole archive, about 240 | |

Each size shows its sources as tabs, free sources first.

- **Daily Princetonian** is the student paper at Princeton. Its API serves
  plain JSON. Some older clues show broken accents, such as `protÈgÈ`. The
  API serves them that way, so the app shows them as they are.
- **Guardian Quick** stands in for the Midi. No free series serves a 9×9 to
  11×11 grid as plain data. The Quick has plain definitions for clues and is
  fast to solve. Its grids are British, so many squares cross only one entry.
  Each clue ends with its letter count, such as `(5,3)`.
- **Universal** is the daily syndicated puzzle from Andrews McMeel, edited by
  David Steinberg.

The LA Times Midi and the Vulture 10×10 have the right size. Their platform,
AmuseLabs, obfuscates its puzzle data on purpose, so the app does not use it.

## NYT puzzles

The NYT Mini, Midi and Crossword are for subscribers. The app sends your own
`NYT-S` cookie with each NYT request. Without a cookie, the NYT tabs show a
message and the app sends no request.

To get the cookie:

1. Log in at nytimes.com in a browser.
2. Open the developer tools and find the cookies for `www.nytimes.com`.
3. Copy the value of the `NYT-S` cookie.

Give the cookie to the app in one of two ways. The environment variable wins
when both are set.

```sh
export NYT_S='<value>'
```

```sh
mkdir -p ~/.config/crossword
printf '%s' '<value>' > ~/.config/crossword/nyt-s
chmod 600 ~/.config/crossword/nyt-s
```

The cookie expires after a time. When the NYT refuses a request, the app says
so. Copy a new cookie then.

## How to use

The app has three screens:

1. **Sizes.** Choose Mini, Midi or Crossword.
2. **List.** Each source of the size has a tab. A `✓` marks a solved puzzle
   and a `◐` marks one that you started.
3. **Puzzle.** The grid, the Across and Down clues, the current clue, and a
   mode line as in vim.

Press `?` on any screen for the key reference.

### Keys in normal mode

| Keys | Action |
|------|--------|
| `h` `j` `k` `l`, arrows | Move one square. Blocks are skipped. Counts work, as in `3l`. |
| `w` `b` `e` | Go to the next entry, the start of this entry, or its end. |
| `0` `^` `$` | Go to the start or the end of this entry. |
| `gg` `G` | Go to the first or the last square. |
| `Tab` `Shift-Tab` | Go to the next or previous entry with a blank square. |
| `space` `Enter` | Switch between across and down. |
| `i` `a` `I` `A` | Insert here, on the next square, or at the start or end of the entry. |
| `x` | Clear the square. |
| `r{letter}` | Replace the square and stay in normal mode. |
| `R` | Put several letters in one square, for a rebus. `Enter` sets them. |
| `dd` `D` | Clear the entry, or clear from the cursor to its end. |
| `cc` `C` `s` | Clear the entry, the rest of it, or the square, then insert. |
| `u` `Ctrl-r` | Undo or redo. |
| `:` | Open the command line. |
| `q` | Go back to the list. The app saves your progress. |

`Ctrl-c` in normal mode does not quit, as in vim. It tells you to use `:q`.

### Keys in insert mode

| Keys | Action |
|------|--------|
| letters | Type a letter and advance. |
| `Backspace` | Clear the square. On a blank square, step back and clear that one. |
| `space` | Clear the square and step on. |
| arrows, `Tab` | Move, or go to the next entry with a blank square. |
| `Enter` | Switch between across and down. |
| `Esc`, `Ctrl-c` | Go back to normal mode. |

The cursor advances as in the NYT app. It goes to the next blank square in the
entry, and then wraps to a blank square earlier in it. When the entry is
complete, the cursor goes on to the next entry with a blank square. In an
entry that was already full, the cursor steps one square, so you can correct
letters in place.

Terminals send `Alt+x` as `Esc` and then `x`. A fast `Esc` and `x` can arrive
in the same form. The app reads both as `Esc` followed by `x`, as vim does.

### Commands

| Command | Action |
|---------|--------|
| `:check [scope]` | Mark wrong letters in red, struck through. Right letters turn green and lock. |
| `:reveal [scope]` | Show the answers in magenta and lock them. |
| `:clear [scope]` | Erase the letters that are not locked. |
| `:12a` `:12d` `:12` | Go to clue 12 Across, 12 Down, or 12 in the current direction. |
| `:reset` | Erase everything and set the timer to zero. |
| `:w` `:q` `:wq` `:qa` | Save, go back to the list, or quit the app. |

A scope is `cell`, `word` or `puzzle`. The default is `word`.

### Keys in lists

| Keys | Action |
|------|--------|
| `j` `k`, `g` `G`, `Ctrl-d` `Ctrl-u` | Move. |
| `h` `l`, `Tab` | Switch the source tab. |
| `Enter` | Open the puzzle. |
| `r` | Reload the list. |
| `q`, `Esc` | Go back. |

### Rules

- A rebus square accepts the full answer or its first letter alone.
- A solved puzzle is read-only. `:reset` starts it again.
- The timer stops when you solve the puzzle. It also pauses when the terminal
  window loses focus, if the terminal reports focus changes.

## Files

| What | Where on Linux |
|------|----------------|
| Downloaded puzzles | `~/.cache/crossword/puzzles/<source>/<id>.json` |
| Progress | `~/.local/share/crossword/progress/<source>/<id>.json` |
| NYT cookie | `~/.config/crossword/nyt-s` |

The app saves your progress at these times:

- when you leave insert mode, and every 10 seconds in insert mode
- after each change in normal mode
- when the terminal window loses focus
- on `:w`, and when you leave the puzzle or quit

## Grid layout

When the terminal has space, each square is a 3×2 box with its clue number in
the top line. A 15×15 grid then needs 61 columns and 31 rows. In a smaller
terminal, the grid changes to one row for each square with no lines or
numbers, and a 15×15 grid needs 45 columns and 15 rows. The clue lists stand
beside the grid when they have 24 columns or more.

## Layout

- `src/puzzle.rs` — the puzzle model. It derives the clue numbers and the
  entries from the grid.
- `src/game.rs` — the state of one puzzle: the letters, the cursor, marks,
  undo and the timer.
- `src/app.rs` — the screens and the vim modes, as a state machine that the
  tests drive with key events.
- `src/ui.rs` — draws the screens with ratatui.
- `src/store.rs` — the puzzle cache and the progress files.
- `src/sources/` — one module for each publisher, the HTTP client, and the
  `Fetcher` that runs on a separate thread.
- `src/main.rs` — the CLI, the terminal setup and the event loop.
- `tests/render.rs` — draws each screen on a headless terminal.
- `tests/live.rs` — network tests, ignored by default.
- `tests/fixtures/` — small synthetic puzzles in each source's format.

## Add a source

1. Add a variant to `SourceId`, with a name and a slug.
2. Add the variant to `Size::sources`.
3. Write a module in `src/sources/` with a pure `parse_puzzle` function that
   returns a `PuzzleData`.
4. Add a synthetic fixture in `tests/fixtures/` and a unit test for the parser.
5. Connect the list and the download in `Fetcher::list` and
   `Fetcher::download`.
6. Add a live test to `tests/live.rs`.
