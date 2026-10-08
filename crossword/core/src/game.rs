//! Solving state for one puzzle: the player's letters, the cursor and its
//! direction, check and reveal marks, undo history, and the solve timer.
//!
//! Nothing here knows about keys or terminals. `app` maps keys onto these
//! calls, which keeps every rule unit-testable.

use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

use crate::puzzle::{Direction, Puzzle, answer_matches};

/// The most undo steps kept.
const UNDO_LIMIT: usize = 500;

/// What a check or reveal has recorded about a square.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Mark {
    #[default]
    None,
    /// A check found the letter wrong. Changing the letter clears the mark.
    Wrong,
    /// A check confirmed the letter. The square is locked.
    Correct,
    /// The answer was revealed. The square is locked.
    Revealed,
}

impl Mark {
    pub fn is_locked(self) -> bool {
        matches!(self, Mark::Correct | Mark::Revealed)
    }
}

/// How much of the grid a check, reveal or clear covers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Scope {
    Cell,
    Entry,
    Puzzle,
}

/// The part of a game saved between sessions.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Progress {
    pub fill: Vec<String>,
    pub marks: Vec<Mark>,
    pub cursor: usize,
    pub direction: Direction,
    pub elapsed_secs: u64,
    pub solved: bool,
}

/// What a check found, for the status line.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CheckReport {
    pub checked: usize,
    pub wrong: usize,
}

#[derive(Debug, Clone)]
struct Snapshot {
    fill: Vec<String>,
    marks: Vec<Mark>,
    cursor: usize,
    direction: Direction,
}

/// One puzzle being solved.
#[derive(Debug, Clone)]
pub struct Game {
    puzzle: Puzzle,
    /// The player's letters per square; empty for a blank or a block.
    fill: Vec<String>,
    marks: Vec<Mark>,
    cursor: usize,
    direction: Direction,
    solved: bool,
    /// Time banked before the current run of the timer.
    elapsed: Duration,
    running_since: Option<Instant>,
    undo: Vec<Snapshot>,
    redo: Vec<Snapshot>,
}

impl Game {
    /// Starts a blank game with the cursor on the first entry.
    pub fn new(puzzle: Puzzle) -> Game {
        let first = &puzzle.entries()[0];
        let (cursor, direction) = (first.cells[0], first.direction);
        let len = puzzle.len();
        Game {
            puzzle,
            fill: vec![String::new(); len],
            marks: vec![Mark::None; len],
            cursor,
            direction,
            solved: false,
            elapsed: Duration::ZERO,
            running_since: None,
            undo: Vec::new(),
            redo: Vec::new(),
        }
    }

    /// Resumes saved progress. Progress that does not fit this grid — say,
    /// from a puzzle the publisher has since corrected — is discarded.
    pub fn with_progress(puzzle: Puzzle, progress: Progress) -> Game {
        let mut game = Game::new(puzzle);
        let len = game.puzzle.len();
        if progress.fill.len() != len || progress.marks.len() != len {
            return game;
        }
        for cell in 0..len {
            if game.puzzle.is_open(cell) {
                game.fill[cell] = progress.fill[cell].trim().to_uppercase();
                game.marks[cell] = progress.marks[cell];
            }
        }
        if game.puzzle.is_open(progress.cursor) {
            game.cursor = progress.cursor;
            game.direction = progress.direction;
            game.normalize_direction();
        }
        game.elapsed = Duration::from_secs(progress.elapsed_secs);
        game.solved = progress.solved && game.is_correct();
        game
    }

    pub fn progress(&self, now: Instant) -> Progress {
        Progress {
            fill: self.fill.clone(),
            marks: self.marks.clone(),
            cursor: self.cursor,
            direction: self.direction,
            elapsed_secs: self.elapsed(now).as_secs(),
            solved: self.solved,
        }
    }

    pub fn puzzle(&self) -> &Puzzle {
        &self.puzzle
    }

    pub fn cursor(&self) -> usize {
        self.cursor
    }

    pub fn direction(&self) -> Direction {
        self.direction
    }

    pub fn letter(&self, cell: usize) -> &str {
        &self.fill[cell]
    }

    pub fn mark(&self, cell: usize) -> Mark {
        self.marks[cell]
    }

    pub fn is_solved(&self) -> bool {
        self.solved
    }

    /// True when every open square holds a letter.
    pub fn is_full(&self) -> bool {
        self.puzzle.open_cells().all(|c| !self.fill[c].is_empty())
    }

    /// True when every open square holds its answer.
    pub fn is_correct(&self) -> bool {
        self.puzzle.open_cells().all(|c| self.cell_correct(c))
    }

    fn cell_correct(&self, cell: usize) -> bool {
        self.puzzle
            .solution(cell)
            .is_some_and(|s| answer_matches(s, &self.fill[cell]))
    }

    // ----- timer -----------------------------------------------------------

    /// Starts or restarts the clock. A solved puzzle's clock stays stopped.
    pub fn resume(&mut self, now: Instant) {
        if self.running_since.is_none() && !self.solved {
            self.running_since = Some(now);
        }
    }

    pub fn pause(&mut self, now: Instant) {
        if let Some(since) = self.running_since.take() {
            self.elapsed += now.saturating_duration_since(since);
        }
    }

    pub fn is_running(&self) -> bool {
        self.running_since.is_some()
    }

    pub fn elapsed(&self, now: Instant) -> Duration {
        match self.running_since {
            Some(since) => self.elapsed + now.saturating_duration_since(since),
            None => self.elapsed,
        }
    }

    // ----- entries ---------------------------------------------------------

    /// The entry under the cursor in the current direction.
    pub fn current_entry(&self) -> usize {
        self.puzzle
            .entry_at(self.cursor, self.direction)
            .or_else(|| self.puzzle.entry_at(self.cursor, self.direction.other()))
            .expect("every open square belongs to an entry")
    }

    /// The entry crossing the cursor, if the square is checked both ways.
    pub fn crossing_entry(&self) -> Option<usize> {
        self.puzzle.entry_at(self.cursor, self.direction.other())
    }

    pub fn entry_is_full(&self, entry: usize) -> bool {
        self.puzzle
            .entry(entry)
            .cells
            .iter()
            .all(|&c| !self.fill[c].is_empty())
    }

    fn first_empty(&self, entry: usize) -> Option<usize> {
        self.puzzle
            .entry(entry)
            .cells
            .iter()
            .copied()
            .find(|&c| self.fill[c].is_empty())
    }

    /// Where the cursor sits within the current entry.
    fn position_in_entry(&self) -> (usize, usize) {
        let entry = self.current_entry();
        let pos = self
            .puzzle
            .entry(entry)
            .cells
            .iter()
            .position(|&c| c == self.cursor)
            .expect("the cursor is inside its entry");
        (entry, pos)
    }

    // ----- motion ----------------------------------------------------------

    /// Keeps the direction pointing along an entry. A square checked only one
    /// way — common in British grids — forces that way.
    fn normalize_direction(&mut self) {
        if self.puzzle.entry_at(self.cursor, self.direction).is_none() {
            self.direction = self.direction.other();
        }
    }

    fn set_cursor(&mut self, cell: usize) {
        self.cursor = cell;
        self.normalize_direction();
    }

    /// Moves to the next open square in a compass direction, jumping over
    /// blocks. Stays put at the edge of the grid. Returns whether it moved.
    pub fn step(&mut self, d_row: isize, d_col: isize) -> bool {
        let (w, h) = (self.puzzle.width() as isize, self.puzzle.height() as isize);
        let (mut row, mut col) = (
            (self.cursor / self.puzzle.width()) as isize,
            (self.cursor % self.puzzle.width()) as isize,
        );
        loop {
            row += d_row;
            col += d_col;
            if row < 0 || row >= h || col < 0 || col >= w {
                return false;
            }
            let cell = (row * w + col) as usize;
            if self.puzzle.is_open(cell) {
                self.set_cursor(cell);
                return true;
            }
        }
    }

    /// Moves one square along the current entry. Returns whether it moved.
    pub fn step_in_entry(&mut self, forward: bool) -> bool {
        let (entry, pos) = self.position_in_entry();
        let cells = &self.puzzle.entry(entry).cells;
        let target = if forward {
            cells.get(pos + 1)
        } else {
            pos.checked_sub(1).and_then(|p| cells.get(p))
        };
        match target {
            Some(&cell) => {
                self.cursor = cell;
                true
            }
            None => false,
        }
    }

    /// Switches between across and down where the square allows it.
    pub fn toggle_direction(&mut self) {
        if self.crossing_entry().is_some() {
            self.direction = self.direction.other();
        }
    }

    /// Puts the cursor on `cell` of `entry`, facing along it.
    fn enter_entry(&mut self, entry: usize, cell: usize) {
        self.cursor = cell;
        self.direction = self.puzzle.entry(entry).direction;
    }

    fn wrap_entry(&self, entry: usize, forward: bool) -> usize {
        let n = self.puzzle.entries().len();
        if forward {
            (entry + 1) % n
        } else {
            (entry + n - 1) % n
        }
    }

    /// Vim `w`: the start of the next entry in clue order.
    pub fn next_entry_start(&mut self) {
        let next = self.wrap_entry(self.current_entry(), true);
        self.enter_entry(next, self.puzzle.entry(next).cells[0]);
    }

    /// Vim `b`: the start of this entry, or of the previous one when the
    /// cursor is already at the start.
    pub fn prev_entry_start(&mut self) {
        let (entry, pos) = self.position_in_entry();
        let target = if pos > 0 {
            entry
        } else {
            self.wrap_entry(entry, false)
        };
        self.enter_entry(target, self.puzzle.entry(target).cells[0]);
    }

    /// Vim `e`: the end of this entry, or of the next one when the cursor is
    /// already at the end.
    pub fn entry_end_motion(&mut self) {
        let (entry, pos) = self.position_in_entry();
        let last = self.puzzle.entry(entry).cells.len() - 1;
        let target = if pos < last {
            entry
        } else {
            self.wrap_entry(entry, true)
        };
        let cell = *self.puzzle.entry(target).cells.last().unwrap();
        self.enter_entry(target, cell);
    }

    /// Vim `0`: the first square of the current entry.
    pub fn goto_entry_start(&mut self) {
        let entry = self.current_entry();
        self.cursor = self.puzzle.entry(entry).cells[0];
    }

    /// Vim `$`: the last square of the current entry.
    pub fn goto_entry_end(&mut self) {
        let entry = self.current_entry();
        self.cursor = *self.puzzle.entry(entry).cells.last().unwrap();
    }

    /// Vim `gg`: the first open square of the grid.
    pub fn goto_first(&mut self) {
        let first = self.puzzle.open_cells().next();
        if let Some(cell) = first {
            self.set_cursor(cell);
        }
    }

    /// Vim `G`: the last open square of the grid.
    pub fn goto_last(&mut self) {
        let last = self.puzzle.open_cells().last();
        if let Some(cell) = last {
            self.set_cursor(cell);
        }
    }

    /// Tab: the first blank square of the next entry that has one. With
    /// nothing blank left, it moves to the next entry's start.
    pub fn next_open_entry(&mut self, forward: bool) {
        let start = self.current_entry();
        let mut entry = start;
        for _ in 0..self.puzzle.entries().len() {
            entry = self.wrap_entry(entry, forward);
            if let Some(cell) = self.first_empty(entry) {
                self.enter_entry(entry, cell);
                return;
            }
        }
        let next = self.wrap_entry(start, forward);
        self.enter_entry(next, self.puzzle.entry(next).cells[0]);
    }

    /// A click on a square, as in the NYT app: a click on the cursor's square
    /// switches direction, and a click elsewhere moves there. Returns false
    /// for a block.
    pub fn click(&mut self, cell: usize) -> bool {
        if !self.puzzle.is_open(cell) {
            return false;
        }
        if cell == self.cursor {
            self.toggle_direction();
        } else {
            self.set_cursor(cell);
        }
        true
    }

    /// Puts the cursor at the start of an entry, facing along it.
    pub fn goto_entry(&mut self, entry: usize) {
        if let Some(first) = self.puzzle.entries().get(entry).map(|e| e.cells[0]) {
            self.enter_entry(entry, first);
        }
    }

    /// How many open squares hold a letter, and how many there are.
    pub fn fill_counts(&self) -> (usize, usize) {
        self.puzzle.open_cells().fold((0, 0), |(filled, open), c| {
            (filled + usize::from(!self.fill[c].is_empty()), open + 1)
        })
    }

    /// Jumps to an entry by its number and direction, as `:12a` does.
    pub fn goto_clue(&mut self, direction: Direction, number: u32) -> bool {
        let found = self
            .puzzle
            .entries()
            .iter()
            .position(|e| e.direction == direction && e.number == number);
        if let Some(entry) = found {
            self.enter_entry(entry, self.puzzle.entry(entry).cells[0]);
        }
        found.is_some()
    }

    // ----- editing ---------------------------------------------------------

    /// True when the player may change `cell`.
    pub fn editable(&self, cell: usize) -> bool {
        !self.solved && self.puzzle.is_open(cell) && !self.marks[cell].is_locked()
    }

    /// Writes letters into a square. Returns whether anything changed.
    fn set(&mut self, cell: usize, letters: &str) -> bool {
        if !self.editable(cell) || self.fill[cell] == letters {
            return false;
        }
        self.fill[cell] = letters.to_string();
        if self.marks[cell] == Mark::Wrong {
            self.marks[cell] = Mark::None;
        }
        true
    }

    /// Insert-mode typing: writes letters at the cursor, then advances the way
    /// the NYT app does. It goes to the next blank square in the entry, then
    /// wraps to an earlier blank. A completed entry hands the cursor on to the
    /// next entry with a blank square. In an entry that was already full, the
    /// cursor steps one square so that the player can overwrite in place.
    pub fn type_letters(&mut self, letters: &str) {
        if self.solved {
            return;
        }
        let letters = letters.trim().to_uppercase();
        if letters.is_empty() {
            return;
        }
        let (entry, pos) = self.position_in_entry();
        let was_full = self.entry_is_full(entry);
        self.set(self.cursor, &letters);

        let cells = self.puzzle.entry(entry).cells.clone();
        let blank = |c: &usize| self.fill[*c].is_empty();
        if let Some(&next) = cells[pos + 1..]
            .iter()
            .find(|c| blank(c))
            .or_else(|| cells[..pos].iter().find(|c| blank(c)))
        {
            self.cursor = next;
        } else if (was_full || !self.jump_to_next_blank(entry)) && pos + 1 < cells.len() {
            // Overwriting a full entry, or nothing left blank anywhere.
            self.cursor = cells[pos + 1];
        }
        self.check_solved();
    }

    /// Moves to the first blank square after `entry` in clue order.
    fn jump_to_next_blank(&mut self, entry: usize) -> bool {
        let mut next = entry;
        for _ in 0..self.puzzle.entries().len() {
            next = self.wrap_entry(next, true);
            if let Some(cell) = self.first_empty(next) {
                self.enter_entry(next, cell);
                return true;
            }
        }
        false
    }

    /// Backspace: clears a filled square in place. On a blank square it steps
    /// back one square in the entry and clears that one.
    pub fn backspace(&mut self) {
        if self.solved {
            return;
        }
        if !self.fill[self.cursor].is_empty() && self.editable(self.cursor) {
            self.set(self.cursor, "");
            return;
        }
        let (entry, pos) = self.position_in_entry();
        if pos > 0 {
            self.cursor = self.puzzle.entry(entry).cells[pos - 1];
            self.set(self.cursor, "");
        }
    }

    /// Insert-mode space: clears the square and steps one square on.
    pub fn clear_and_advance(&mut self) {
        if self.solved {
            return;
        }
        self.set(self.cursor, "");
        let (entry, pos) = self.position_in_entry();
        if let Some(&next) = self.puzzle.entry(entry).cells.get(pos + 1) {
            self.cursor = next;
        }
    }

    /// Vim `x`: clears the square under the cursor.
    pub fn clear_cell(&mut self) {
        self.set(self.cursor, "");
    }

    /// Vim `r`: replaces the square under the cursor without moving.
    pub fn replace_cell(&mut self, letters: &str) {
        let letters = letters.trim().to_uppercase();
        if !letters.is_empty() {
            self.set(self.cursor, &letters);
            self.check_solved();
        }
    }

    /// Vim `D`: clears from the cursor to the end of the entry.
    pub fn clear_to_entry_end(&mut self) {
        let (entry, pos) = self.position_in_entry();
        let cells = self.puzzle.entry(entry).cells.clone();
        for &cell in &cells[pos..] {
            self.set(cell, "");
        }
    }

    /// Vim `dd`: clears the entry and puts the cursor at its start.
    pub fn clear_entry(&mut self) {
        self.goto_entry_start();
        self.clear_to_entry_end();
    }

    fn scope_cells(&self, scope: Scope) -> Vec<usize> {
        match scope {
            Scope::Cell => vec![self.cursor],
            Scope::Entry => self.puzzle.entry(self.current_entry()).cells.clone(),
            Scope::Puzzle => self.puzzle.open_cells().collect(),
        }
    }

    /// Marks each filled square in `scope` right (locking it) or wrong.
    pub fn check(&mut self, scope: Scope) -> CheckReport {
        let mut report = CheckReport::default();
        if self.solved {
            return report;
        }
        for cell in self.scope_cells(scope) {
            if self.fill[cell].is_empty() || self.marks[cell] == Mark::Revealed {
                continue;
            }
            report.checked += 1;
            if self.cell_correct(cell) {
                self.marks[cell] = Mark::Correct;
            } else {
                self.marks[cell] = Mark::Wrong;
                report.wrong += 1;
            }
        }
        report
    }

    /// Fills `scope` with its answers. Squares that were already right are
    /// marked correct, and the others are marked revealed. Returns how many
    /// squares were revealed.
    pub fn reveal(&mut self, scope: Scope) -> usize {
        if self.solved {
            return 0;
        }
        let mut revealed = 0;
        for cell in self.scope_cells(scope) {
            if self.marks[cell].is_locked() {
                continue;
            }
            if self.cell_correct(cell) {
                self.marks[cell] = Mark::Correct;
            } else {
                self.fill[cell] = self.puzzle.solution(cell).unwrap_or_default().to_string();
                self.marks[cell] = Mark::Revealed;
                revealed += 1;
            }
        }
        self.check_solved();
        revealed
    }

    /// Clears the unlocked squares in `scope`.
    pub fn clear(&mut self, scope: Scope) {
        for cell in self.scope_cells(scope) {
            self.set(cell, "");
        }
    }

    /// Starts over: every letter, mark, the timer and the undo history.
    pub fn reset(&mut self, now: Instant) {
        let len = self.puzzle.len();
        self.fill = vec![String::new(); len];
        self.marks = vec![Mark::None; len];
        self.solved = false;
        self.elapsed = Duration::ZERO;
        self.running_since = Some(now);
        self.undo.clear();
        self.redo.clear();
        self.goto_first();
    }

    fn check_solved(&mut self) {
        if !self.solved && self.is_correct() {
            self.solved = true;
            self.pause(Instant::now());
        }
    }

    // ----- undo ------------------------------------------------------------

    fn snapshot(&self) -> Snapshot {
        Snapshot {
            fill: self.fill.clone(),
            marks: self.marks.clone(),
            cursor: self.cursor,
            direction: self.direction,
        }
    }

    fn restore(&mut self, snapshot: Snapshot) {
        self.fill = snapshot.fill;
        self.marks = snapshot.marks;
        self.cursor = snapshot.cursor;
        self.direction = snapshot.direction;
    }

    /// Records the current state as an undo step. The caller makes it before
    /// a change; an insert-mode session counts as one change, as in vim.
    pub fn checkpoint(&mut self) {
        if self.solved {
            return;
        }
        if self.undo.len() == UNDO_LIMIT {
            self.undo.remove(0);
        }
        self.undo.push(self.snapshot());
        self.redo.clear();
    }

    /// Drops the latest undo step if nothing changed since it was made.
    /// Returns whether it dropped one.
    pub fn drop_unchanged_checkpoint(&mut self) -> bool {
        let unchanged = self
            .undo
            .last()
            .is_some_and(|s| s.fill == self.fill && s.marks == self.marks);
        if unchanged {
            self.undo.pop();
        }
        unchanged
    }

    /// Steps back one change. A solved puzzle stays solved.
    pub fn undo(&mut self) -> bool {
        if self.solved {
            return false;
        }
        let Some(snapshot) = self.undo.pop() else {
            return false;
        };
        self.redo.push(self.snapshot());
        self.restore(snapshot);
        true
    }

    pub fn redo(&mut self) -> bool {
        if self.solved {
            return false;
        }
        let Some(snapshot) = self.redo.pop() else {
            return false;
        };
        self.undo.push(self.snapshot());
        self.restore(snapshot);
        self.check_solved();
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::puzzle::tests::{data_from_rows, small};

    // `small()` is:
    //
    //   1C 2A 3T
    //   4A  R  E
    //   ##  5E E
    //
    // Cells are numbered 0..9 in reading order; cell 6 is the block.

    fn game() -> Game {
        Game::new(small())
    }

    fn type_word(g: &mut Game, word: &str) {
        for c in word.chars() {
            g.type_letters(&c.to_string());
        }
    }

    fn letters(g: &Game) -> String {
        (0..g.puzzle().len())
            .map(|c| match g.letter(c) {
                _ if !g.puzzle().is_open(c) => "#".to_string(),
                "" => ".".to_string(),
                s => s.to_string(),
            })
            .collect()
    }

    #[test]
    fn starts_on_one_across() {
        let g = game();
        assert_eq!((g.cursor(), g.direction()), (0, Direction::Across));
    }

    #[test]
    fn steps_jump_blocks_and_stop_at_edges() {
        let mut g = game();
        assert!(!g.step(-1, 0)); // top edge
        assert!(g.step(1, 0)); // 0 -> 3
        assert_eq!(g.cursor(), 3);
        assert!(!g.step(1, 0)); // below 3 is the block, then the edge
        assert_eq!(g.cursor(), 3);

        let mut g = Game::new(Puzzle::new(data_from_rows(&["AB#CD", "EF#GH"], &[])).unwrap());
        g.step(0, 1);
        g.step(0, 1); // jumps the block from 1 to 3
        assert_eq!(g.cursor(), 3);
    }

    #[test]
    fn direction_follows_one_way_squares() {
        // Cell 4 is open but only part of a down entry.
        let mut g = Game::new(Puzzle::new(data_from_rows(&["ABC", "#D#", "EFG"], &[])).unwrap());
        g.step(0, 1); // 1, across
        g.step(1, 0); // 4: no across entry here
        assert_eq!((g.cursor(), g.direction()), (4, Direction::Down));
        g.toggle_direction(); // nothing to toggle to
        assert_eq!(g.direction(), Direction::Down);
    }

    #[test]
    fn typing_fills_the_entry_then_moves_to_the_next_blank() {
        let mut g = game();
        type_word(&mut g, "CAT");
        assert_eq!(letters(&g), "CAT...#..");
        // 1A is full, so the cursor hands on to 4A.
        assert_eq!((g.cursor(), g.direction()), (3, Direction::Across));
    }

    #[test]
    fn typing_skips_filled_squares_and_wraps_within_the_entry() {
        let mut g = game();
        g.step(0, 1); // cell 1
        g.type_letters("A"); // -> 2
        assert_eq!(g.cursor(), 2);
        g.type_letters("T"); // nothing later; wraps to the blank at 0
        assert_eq!(g.cursor(), 0);
    }

    #[test]
    fn overwriting_a_full_entry_steps_one_square() {
        let mut g = game();
        type_word(&mut g, "CAT"); // the cursor moves on to 4A
        g.prev_entry_start(); // back to the start of 1A
        g.type_letters("B");
        assert_eq!(g.cursor(), 1);
        assert_eq!(g.letter(0), "B");
    }

    #[test]
    fn backspace_clears_in_place_then_steps_back() {
        let mut g = game();
        type_word(&mut g, "CA"); // cursor on 2
        g.backspace(); // 2 is blank: step back to 1 and clear it
        assert_eq!((g.cursor(), letters(&g).as_str()), (1, "C.....#.."));
        g.step(0, -1);
        g.backspace(); // 0 is filled: clear in place
        assert_eq!((g.cursor(), letters(&g).as_str()), (0, "......#.."));
        g.backspace(); // at the start of the entry: nothing to do
        assert_eq!(g.cursor(), 0);
    }

    #[test]
    fn entry_motions_follow_vim() {
        let mut g = game();
        g.next_entry_start(); // w: 4A
        assert_eq!(g.cursor(), 3);
        g.step(0, 1);
        g.prev_entry_start(); // b from the middle: start of 4A
        assert_eq!(g.cursor(), 3);
        g.prev_entry_start(); // b at the start: 1A
        assert_eq!(g.cursor(), 0);
        g.entry_end_motion(); // e: end of 1A
        assert_eq!(g.cursor(), 2);
        g.entry_end_motion(); // e at the end: end of 4A
        assert_eq!(g.cursor(), 5);
        g.goto_entry_start(); // 0
        assert_eq!(g.cursor(), 3);
        g.goto_entry_end(); // $
        assert_eq!(g.cursor(), 5);
        g.goto_last(); // G
        assert_eq!(g.cursor(), 8);
        g.goto_first(); // gg
        assert_eq!(g.cursor(), 0);
    }

    #[test]
    fn w_wraps_from_the_last_across_to_the_first_down() {
        let mut g = game();
        g.goto_clue(Direction::Across, 5);
        g.next_entry_start();
        assert_eq!((g.cursor(), g.direction()), (0, Direction::Down));
        g.goto_clue(Direction::Down, 3);
        g.next_entry_start(); // wraps to 1A
        assert_eq!((g.cursor(), g.direction()), (0, Direction::Across));
    }

    #[test]
    fn tab_skips_full_entries() {
        let mut g = game();
        type_word(&mut g, "CATARE"); // fills 1A and 4A
        g.goto_first();
        g.next_open_entry(true);
        assert_eq!((g.cursor(), g.direction()), (7, Direction::Across)); // 5A
        g.next_open_entry(false);
        // Backward from 5A, wrapping through the downs: 3D has a blank at 8.
        assert_eq!((g.cursor(), g.direction()), (8, Direction::Down));
    }

    #[test]
    fn check_marks_and_locks() {
        let mut g = game();
        type_word(&mut g, "COT");
        g.goto_first();
        let report = g.check(Scope::Entry);
        assert_eq!(
            report,
            CheckReport {
                checked: 3,
                wrong: 1
            }
        );
        assert_eq!(g.mark(0), Mark::Correct);
        assert_eq!(g.mark(1), Mark::Wrong);
        // A correct square is locked; a wrong one clears its mark on change.
        g.replace_cell("X");
        assert_eq!(g.letter(0), "C");
        g.step(0, 1);
        g.replace_cell("A");
        assert_eq!((g.letter(1), g.mark(1)), ("A", Mark::None));
    }

    #[test]
    fn reveal_and_solve() {
        let mut g = game();
        g.resume(Instant::now());
        type_word(&mut g, "CAT");
        assert_eq!(g.reveal(Scope::Cell), 1); // cursor moved on to cell 3
        assert_eq!((g.letter(3), g.mark(3)), ("A", Mark::Revealed));
        assert!(!g.is_solved());
        g.reveal(Scope::Puzzle);
        assert!(g.is_solved());
        assert!(!g.is_running());
        // A solved grid is read-only.
        g.clear(Scope::Puzzle);
        assert!(g.is_full());
        assert!(!g.undo());
    }

    #[test]
    fn typing_the_last_answer_solves() {
        let mut g = game();
        type_word(&mut g, "CATAREEE");
        assert!(g.is_solved());
    }

    #[test]
    fn full_but_wrong_is_not_solved() {
        let mut g = game();
        type_word(&mut g, "CATAREEX");
        assert!(g.is_full());
        assert!(!g.is_solved());
    }

    #[test]
    fn undo_and_redo_restore_letters_and_cursor() {
        let mut g = game();
        g.checkpoint();
        type_word(&mut g, "CAT"); // the cursor moves on to 4A
        g.goto_first();
        g.checkpoint();
        g.clear_entry();
        assert_eq!(letters(&g), "......#..");
        assert!(g.undo());
        assert_eq!((letters(&g).as_str(), g.cursor()), ("CAT...#..", 0));
        assert!(g.undo());
        assert_eq!((letters(&g).as_str(), g.cursor()), ("......#..", 0));
        assert!(!g.undo());
        assert!(g.redo());
        assert_eq!((letters(&g).as_str(), g.cursor()), ("CAT...#..", 0));
    }

    #[test]
    fn unchanged_checkpoints_are_dropped() {
        let mut g = game();
        g.checkpoint();
        assert!(g.drop_unchanged_checkpoint());
        assert!(!g.undo());
        g.checkpoint();
        g.type_letters("C");
        assert!(!g.drop_unchanged_checkpoint());
        assert!(g.undo());
    }

    #[test]
    fn clicks_move_or_switch_direction() {
        let mut g = game();
        assert!(g.click(4));
        assert_eq!((g.cursor(), g.direction()), (4, Direction::Across));
        assert!(g.click(4)); // the same square again switches direction
        assert_eq!(g.direction(), Direction::Down);
        assert!(!g.click(6)); // a block
        assert_eq!(g.cursor(), 4);
        g.goto_entry(2); // 5A
        assert_eq!((g.cursor(), g.direction()), (7, Direction::Across));
    }

    #[test]
    fn fill_counts_count_open_squares() {
        let mut g = game();
        assert_eq!(g.fill_counts(), (0, 8));
        type_word(&mut g, "CA");
        assert_eq!(g.fill_counts(), (2, 8));
    }

    #[test]
    fn step_in_entry_stays_inside_the_entry() {
        let mut g = game();
        assert!(!g.step_in_entry(false));
        assert!(g.step_in_entry(true));
        assert!(g.step_in_entry(true));
        assert_eq!(g.cursor(), 2);
        assert!(!g.step_in_entry(true));
    }

    #[test]
    fn dd_and_d_clear_parts_of_an_entry() {
        let mut g = game();
        type_word(&mut g, "CAT");
        g.goto_first();
        g.step(0, 1);
        g.clear_to_entry_end(); // D
        assert_eq!(letters(&g), "C.....#..");
        g.clear_entry(); // dd
        assert_eq!((letters(&g).as_str(), g.cursor()), ("......#..", 0));
    }

    #[test]
    fn timer_banks_time_across_pauses() {
        let mut g = game();
        let t0 = Instant::now();
        g.resume(t0);
        g.pause(t0 + Duration::from_secs(5));
        assert_eq!(
            g.elapsed(t0 + Duration::from_secs(50)),
            Duration::from_secs(5)
        );
        g.resume(t0 + Duration::from_secs(60));
        assert_eq!(
            g.elapsed(t0 + Duration::from_secs(62)),
            Duration::from_secs(7)
        );
    }

    #[test]
    fn progress_round_trips() {
        let mut g = game();
        type_word(&mut g, "CAT");
        g.goto_first();
        g.check(Scope::Cell);
        g.step(0, 1);
        let saved = g.progress(Instant::now());
        let restored = Game::with_progress(small(), saved.clone());
        assert_eq!(letters(&restored), "CAT...#..");
        assert_eq!(restored.cursor(), 1);
        assert_eq!(restored.mark(0), Mark::Correct);

        // Progress for a different grid is ignored.
        let mut wrong = saved;
        wrong.fill.pop();
        assert_eq!(letters(&Game::with_progress(small(), wrong)), "......#..");
    }
}
