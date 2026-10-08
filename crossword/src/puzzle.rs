//! The puzzle model that every source converges on.
//!
//! A source builds a [`PuzzleData`] — the raw grid of answers and blocks plus
//! clue texts keyed by direction and number. That is also the form the cache
//! stores. [`Puzzle::new`] validates it and derives everything else: the
//! standard clue numbering, one [`Entry`] per across or down run, and the
//! lookup from a square to the entries that pass through it.

use std::collections::HashMap;
use std::fmt;

use serde::{Deserialize, Serialize};

/// The largest grid side accepted. Sunday-sized puzzles are 21 or 23 squares.
pub const MAX_SIDE: usize = 64;

/// The two directions an entry can run in.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Direction {
    Across,
    Down,
}

impl Direction {
    pub fn other(self) -> Self {
        match self {
            Direction::Across => Direction::Down,
            Direction::Down => Direction::Across,
        }
    }

    /// The suffix used in clue references such as `12A`.
    pub fn letter(self) -> char {
        match self {
            Direction::Across => 'A',
            Direction::Down => 'D',
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Direction::Across => "Across",
            Direction::Down => "Down",
        }
    }
}

/// Descriptive fields shown above the grid.
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct Meta {
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub author: String,
    #[serde(default)]
    pub copyright: String,
    /// Publication date as `YYYY-MM-DD`, when the source gives one.
    #[serde(default)]
    pub date: Option<String>,
}

/// One clue as a source supplies it, before it is matched to a grid run.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ClueData {
    pub direction: Direction,
    pub number: u32,
    pub text: String,
}

/// The raw, serializable puzzle. Sources produce it and the cache stores it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PuzzleData {
    pub meta: Meta,
    pub width: usize,
    pub height: usize,
    /// Row-major squares: `None` is a block, `Some` holds the answer, which is
    /// more than one letter for a rebus square.
    pub grid: Vec<Option<String>>,
    /// Row-major indices of circled or shaded squares.
    #[serde(default)]
    pub circled: Vec<usize>,
    pub clues: Vec<ClueData>,
}

/// Why a [`PuzzleData`] could not become a [`Puzzle`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PuzzleError(pub String);

impl fmt::Display for PuzzleError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for PuzzleError {}

/// One across or down run of two or more open squares, with its clue.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub direction: Direction,
    pub number: u32,
    /// Row-major indices of the squares, in reading order.
    pub cells: Vec<usize>,
    /// The clue text; empty when the source had no clue for this run.
    pub clue: String,
}

/// A validated puzzle with its derived numbering and entries.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Puzzle {
    data: PuzzleData,
    numbers: Vec<Option<u32>>,
    /// Across entries in number order, then down entries in number order —
    /// the order the clue lists use.
    entries: Vec<Entry>,
    across_of: Vec<Option<usize>>,
    down_of: Vec<Option<usize>>,
    circled: Vec<bool>,
}

/// Computes the standard clue numbering for a grid. A square gets the next
/// number when it starts an across or down run of two or more open squares.
pub fn numbering(width: usize, height: usize, grid: &[Option<String>]) -> Vec<Option<u32>> {
    let open = |row: usize, col: usize| grid[row * width + col].is_some();
    let mut numbers = vec![None; grid.len()];
    let mut next = 1;
    for row in 0..height {
        for col in 0..width {
            if !open(row, col) {
                continue;
            }
            let starts_across =
                (col == 0 || !open(row, col - 1)) && col + 1 < width && open(row, col + 1);
            let starts_down =
                (row == 0 || !open(row - 1, col)) && row + 1 < height && open(row + 1, col);
            if starts_across || starts_down {
                numbers[row * width + col] = Some(next);
                next += 1;
            }
        }
    }
    numbers
}

/// True when a player's letters count as the answer for a square. A rebus
/// square also accepts its first letter alone, the convention most solving
/// software follows, so that a rebus never blocks a solve.
pub fn answer_matches(solution: &str, guess: &str) -> bool {
    if guess.is_empty() {
        return false;
    }
    if solution.eq_ignore_ascii_case(guess) {
        return true;
    }
    let mut chars = solution.chars();
    let first = chars.next();
    chars.next().is_some()
        && guess.chars().count() == 1
        && first.is_some_and(|f| guess.chars().all(|g| g.eq_ignore_ascii_case(&f)))
}

impl Puzzle {
    /// Validates `data` and derives the numbering and entries. Clue texts are
    /// matched to runs by direction and number. A run with no clue keeps an
    /// empty text, and a clue that matches no run is dropped.
    pub fn new(mut data: PuzzleData) -> Result<Puzzle, PuzzleError> {
        let (width, height) = (data.width, data.height);
        if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE {
            return Err(PuzzleError(format!(
                "grid size {width}x{height} is out of range"
            )));
        }
        if data.grid.len() != width * height {
            return Err(PuzzleError(format!(
                "grid has {} squares, expected {}",
                data.grid.len(),
                width * height
            )));
        }
        for answer in data.grid.iter_mut().flatten() {
            let normalized = answer.trim().to_uppercase();
            if normalized.is_empty() {
                return Err(PuzzleError("an open square has no answer".into()));
            }
            *answer = normalized;
        }
        let len = data.grid.len();
        if let Some(bad) = data.circled.iter().find(|&&i| i >= len) {
            return Err(PuzzleError(format!("circled square {bad} is off the grid")));
        }

        let numbers = numbering(width, height, &data.grid);
        let texts: HashMap<(Direction, u32), &str> = data
            .clues
            .iter()
            .map(|c| ((c.direction, c.number), c.text.trim()))
            .collect();

        let open = |i: usize| data.grid[i].is_some();
        let mut across = Vec::new();
        let mut down = Vec::new();
        for (i, number) in numbers.iter().enumerate() {
            let Some(number) = *number else { continue };
            let (row, col) = (i / width, i % width);
            if (col == 0 || !open(i - 1)) && col + 1 < width && open(i + 1) {
                let cells: Vec<usize> = (i..row * width + width).take_while(|&j| open(j)).collect();
                across.push((number, cells));
            }
            if (row == 0 || !open(i - width)) && row + 1 < height && open(i + width) {
                let cells: Vec<usize> = (i..len).step_by(width).take_while(|&j| open(j)).collect();
                down.push((number, cells));
            }
        }
        if across.is_empty() && down.is_empty() {
            return Err(PuzzleError("the grid has no entries".into()));
        }

        let mut entries = Vec::with_capacity(across.len() + down.len());
        let mut across_of = vec![None; len];
        let mut down_of = vec![None; len];
        for (direction, runs, owner) in [
            (Direction::Across, across, &mut across_of),
            (Direction::Down, down, &mut down_of),
        ] {
            for (number, cells) in runs {
                for &cell in &cells {
                    owner[cell] = Some(entries.len());
                }
                let clue = texts
                    .get(&(direction, number))
                    .map(|t| t.to_string())
                    .unwrap_or_default();
                entries.push(Entry {
                    direction,
                    number,
                    cells,
                    clue,
                });
            }
        }

        let mut circled = vec![false; len];
        for &i in &data.circled {
            circled[i] = true;
        }

        Ok(Puzzle {
            data,
            numbers,
            entries,
            across_of,
            down_of,
            circled,
        })
    }

    /// The validated raw form, with answers normalized to uppercase.
    pub fn data(&self) -> &PuzzleData {
        &self.data
    }

    pub fn meta(&self) -> &Meta {
        &self.data.meta
    }

    pub fn width(&self) -> usize {
        self.data.width
    }

    pub fn height(&self) -> usize {
        self.data.height
    }

    /// The number of squares, blocks included.
    pub fn len(&self) -> usize {
        self.data.grid.len()
    }

    pub fn is_empty(&self) -> bool {
        self.data.grid.is_empty()
    }

    pub fn is_open(&self, cell: usize) -> bool {
        self.data.grid.get(cell).is_some_and(Option::is_some)
    }

    /// The answer for a square, or `None` for a block.
    pub fn solution(&self, cell: usize) -> Option<&str> {
        self.data.grid.get(cell).and_then(|s| s.as_deref())
    }

    pub fn number(&self, cell: usize) -> Option<u32> {
        self.numbers.get(cell).copied().flatten()
    }

    pub fn is_circled(&self, cell: usize) -> bool {
        self.circled.get(cell).copied().unwrap_or(false)
    }

    /// All entries: across in number order, then down in number order.
    pub fn entries(&self) -> &[Entry] {
        &self.entries
    }

    pub fn entry(&self, index: usize) -> &Entry {
        &self.entries[index]
    }

    /// The index of the entry that runs through `cell` in `direction`.
    pub fn entry_at(&self, cell: usize, direction: Direction) -> Option<usize> {
        let owners = match direction {
            Direction::Across => &self.across_of,
            Direction::Down => &self.down_of,
        };
        owners.get(cell).copied().flatten()
    }

    /// Open squares in reading order.
    pub fn open_cells(&self) -> impl Iterator<Item = usize> + '_ {
        (0..self.len()).filter(|&i| self.is_open(i))
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// Builds grid data from rows where `#` is a block and any other
    /// character is a one-letter answer.
    pub(crate) fn data_from_rows(rows: &[&str], clues: &[(Direction, u32, &str)]) -> PuzzleData {
        let width = rows[0].chars().count();
        let grid = rows
            .iter()
            .flat_map(|r| r.chars())
            .map(|c| (c != '#').then(|| c.to_string()))
            .collect();
        PuzzleData {
            meta: Meta {
                title: "Test".into(),
                author: "Tester".into(),
                ..Meta::default()
            },
            width,
            height: rows.len(),
            grid,
            circled: Vec::new(),
            clues: clues
                .iter()
                .map(|&(direction, number, text)| ClueData {
                    direction,
                    number,
                    text: text.into(),
                })
                .collect(),
        }
    }

    /// A 3x3 with one corner block:
    ///
    /// ```text
    /// 1C 2A 3T
    /// 4A  R  E
    /// ##  5E E
    /// ```
    pub(crate) fn small() -> Puzzle {
        use Direction::*;
        Puzzle::new(data_from_rows(
            &["CAT", "ARE", "#EE"],
            &[
                (Across, 1, "Feline"),
                (Across, 4, "Exist"),
                (Across, 5, "Letter pair"),
                (Down, 1, "Taxi"),
                (Down, 2, "Painting, e.g."),
                (Down, 3, "Golf peg"),
            ],
        ))
        .unwrap()
    }

    #[test]
    fn numbers_follow_the_standard_rule() {
        let p = small();
        let numbers: Vec<Option<u32>> = (0..9).map(|i| p.number(i)).collect();
        assert_eq!(
            numbers,
            [
                Some(1),
                Some(2),
                Some(3),
                Some(4),
                None,
                None,
                None,
                Some(5),
                None
            ]
        );
    }

    #[test]
    fn entries_are_across_then_down_with_clues() {
        let p = small();
        let summary: Vec<(char, u32, Vec<usize>, &str)> = p
            .entries()
            .iter()
            .map(|e| {
                (
                    e.direction.letter(),
                    e.number,
                    e.cells.clone(),
                    e.clue.as_str(),
                )
            })
            .collect();
        assert_eq!(
            summary,
            [
                ('A', 1, vec![0, 1, 2], "Feline"),
                ('A', 4, vec![3, 4, 5], "Exist"),
                ('A', 5, vec![7, 8], "Letter pair"),
                ('D', 1, vec![0, 3], "Taxi"),
                ('D', 2, vec![1, 4, 7], "Painting, e.g."),
                ('D', 3, vec![2, 5, 8], "Golf peg"),
            ]
        );
    }

    #[test]
    fn entry_lookup_by_square() {
        let p = small();
        assert_eq!(p.entry_at(4, Direction::Across), Some(1));
        assert_eq!(p.entry_at(4, Direction::Down), Some(4));
        assert_eq!(p.entry_at(6, Direction::Across), None);
    }

    #[test]
    fn unchecked_square_belongs_to_one_entry_only() {
        // The middle column of row 1 is open but has blocks on both sides.
        let p = Puzzle::new(data_from_rows(&["ABC", "#D#", "EFG"], &[])).unwrap();
        assert_eq!(p.entry_at(4, Direction::Across), None);
        assert!(p.entry_at(4, Direction::Down).is_some());
        // Missing clues stay empty rather than failing the puzzle.
        assert!(p.entries().iter().all(|e| e.clue.is_empty()));
    }

    #[test]
    fn answers_are_normalized_to_uppercase() {
        let mut data = data_from_rows(&["ab", "cd"], &[]);
        data.grid[0] = Some(" star ".into());
        let p = Puzzle::new(data).unwrap();
        assert_eq!(p.solution(0), Some("STAR"));
        assert_eq!(p.solution(1), Some("B"));
    }

    #[test]
    fn invalid_data_is_rejected() {
        let mut short = data_from_rows(&["AB", "CD"], &[]);
        short.grid.pop();
        assert!(Puzzle::new(short).is_err());

        let all_blocks = data_from_rows(&["##", "##"], &[]);
        assert!(Puzzle::new(all_blocks).is_err());

        let mut bad_circle = data_from_rows(&["AB", "CD"], &[]);
        bad_circle.circled.push(4);
        assert!(Puzzle::new(bad_circle).is_err());

        let mut blank = data_from_rows(&["AB", "CD"], &[]);
        blank.grid[0] = Some("  ".into());
        assert!(Puzzle::new(blank).is_err());
    }

    #[test]
    fn rebus_accepts_full_answer_or_first_letter() {
        assert!(answer_matches("STAR", "star"));
        assert!(answer_matches("STAR", "S"));
        assert!(!answer_matches("STAR", "ST"));
        assert!(!answer_matches("STAR", "T"));
        assert!(answer_matches("A", "a"));
        assert!(!answer_matches("A", ""));
        assert!(!answer_matches("A", "AA"));
    }
}
