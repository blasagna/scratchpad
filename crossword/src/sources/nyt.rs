//! The New York Times Mini, Midi and Crossword, for subscribers.
//!
//! These are subscriber puzzles, so every request carries the player's own
//! `NYT-S` cookie, and without one [`super::Fetcher`] makes no request at all.
//! The JSON is the v6 format the NYT web app reads. Each square is either an
//! empty object (a block) or carries its `answer`. A `type` other than 1 marks
//! a circled or shaded square, and `moreAnswers.valid` lists rebus spellings.

use chrono::NaiveDate;
use serde::Deserialize;

use super::{SourceError, SourceId, clean_text, join_names};
use crate::puzzle::{ClueData, Direction, Meta, PuzzleData, numbering};

/// The value of the `Cookie` header for a token. A token pasted with its
/// `NYT-S=` name is accepted too.
pub fn cookie_header(token: &str) -> String {
    let token = token.trim();
    let value = token.strip_prefix("NYT-S=").unwrap_or(token);
    format!("NYT-S={value}")
}

pub fn puzzle_url(source: SourceId, date: NaiveDate) -> String {
    let kind = match source {
        SourceId::NytMini => "mini",
        SourceId::NytMidi => "midi",
        _ => "daily",
    };
    format!(
        "https://www.nytimes.com/svc/crosswords/v6/puzzle/{kind}/{}.json",
        date.format("%Y-%m-%d")
    )
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Root {
    body: Vec<Body>,
    #[serde(default)]
    constructors: Vec<String>,
    #[serde(default)]
    editor: Option<String>,
    #[serde(default)]
    copyright: Option<String>,
    #[serde(default)]
    publication_date: Option<String>,
    #[serde(default)]
    title: Option<String>,
}

#[derive(Deserialize)]
struct Body {
    dimensions: Dimensions,
    cells: Vec<Cell>,
    clues: Vec<Clue>,
}

#[derive(Deserialize)]
struct Dimensions {
    width: usize,
    height: usize,
}

#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct Cell {
    #[serde(default)]
    answer: Option<String>,
    #[serde(default, rename = "type")]
    kind: Option<u32>,
    #[serde(default)]
    more_answers: Option<MoreAnswers>,
}

#[derive(Deserialize, Default)]
struct MoreAnswers {
    #[serde(default)]
    valid: Vec<String>,
}

#[derive(Deserialize)]
struct Clue {
    cells: Vec<usize>,
    direction: String,
    #[serde(default)]
    text: Vec<ClueText>,
}

#[derive(Deserialize)]
struct ClueText {
    #[serde(default)]
    plain: Option<String>,
}

pub fn parse_puzzle(body: &str) -> Result<PuzzleData, SourceError> {
    let root: Root = serde_json::from_str(body)?;
    let Some(board) = root.body.into_iter().next() else {
        return Err(SourceError::Parse("the response has no puzzle body".into()));
    };
    let (width, height) = (board.dimensions.width, board.dimensions.height);
    if board.cells.len() != width * height {
        return Err(SourceError::Parse(format!(
            "{} squares for a {width}x{height} grid",
            board.cells.len()
        )));
    }

    let mut grid = Vec::with_capacity(board.cells.len());
    let mut circled = Vec::new();
    for (i, cell) in board.cells.iter().enumerate() {
        let answer = cell
            .answer
            .clone()
            .filter(|a| !a.trim().is_empty())
            .or_else(|| {
                cell.more_answers
                    .as_ref()
                    .and_then(|m| m.valid.first().cloned())
            });
        if answer.is_some() && cell.kind.is_some_and(|k| k != 1) {
            circled.push(i);
        }
        grid.push(answer);
    }

    let numbers = numbering(width, height, &grid);
    let clues = board
        .clues
        .iter()
        .filter_map(|c| {
            let direction = match c.direction.as_str() {
                "Across" => Direction::Across,
                "Down" => Direction::Down,
                _ => return None,
            };
            let number = numbers.get(*c.cells.first()?).copied().flatten()?;
            let text = c.text.iter().find_map(|t| t.plain.as_deref())?;
            Some(ClueData {
                direction,
                number,
                text: clean_text(text),
            })
        })
        .collect();

    let mut author = join_names(&root.constructors);
    if let Some(editor) = root
        .editor
        .as_deref()
        .map(str::trim)
        .filter(|e| !e.is_empty())
    {
        author = format!("{author}, edited by {editor}");
    }
    let copyright = match root.copyright.as_deref().map(str::trim) {
        Some(year) if !year.is_empty() => format!("© {year} The New York Times"),
        _ => "The New York Times".to_string(),
    };
    Ok(PuzzleData {
        meta: Meta {
            title: root.title.unwrap_or_default().trim().to_string(),
            author,
            copyright,
            date: root.publication_date,
        },
        width,
        height,
        grid,
        circled,
        clues,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::puzzle::Puzzle;

    const PUZZLE: &str = include_str!("../../tests/fixtures/nyt.json");

    #[test]
    fn puzzle_parses_rebus_circles_and_clues() {
        let data = parse_puzzle(PUZZLE).unwrap();
        assert_eq!(
            data.meta.author,
            "Ada Lovelace and Alan Turing, edited by Grace Hopper"
        );
        assert_eq!(data.meta.copyright, "© 2026 The New York Times");
        assert_eq!(data.meta.date.as_deref(), Some("2026-10-08"));
        assert_eq!(data.circled, [8]);

        let puzzle = Puzzle::new(data).unwrap();
        assert_eq!(puzzle.solution(2), Some("TEA")); // rebus square
        assert_eq!(puzzle.solution(1), Some("A")); // from moreAnswers
        assert!(!puzzle.is_open(6));
        let clues: Vec<(u32, char, &str)> = puzzle
            .entries()
            .iter()
            .map(|e| (e.number, e.direction.letter(), e.clue.as_str()))
            .collect();
        assert_eq!(
            clues,
            [
                (1, 'A', "Feline drink?"),
                (4, 'A', "Exist"),
                (5, 'A', "Letter pair"),
                (1, 'D', "Golden State, briefly"),
                (2, 'D', "Exist"),
                (3, 'D', "Teeing ground"),
            ]
        );
    }

    #[test]
    fn cookie_header_accepts_bare_or_named_tokens() {
        assert_eq!(cookie_header(" abc^123 "), "NYT-S=abc^123");
        assert_eq!(cookie_header("NYT-S=abc^123"), "NYT-S=abc^123");
    }

    #[test]
    fn urls_name_the_series_and_date() {
        let date = NaiveDate::from_ymd_opt(2026, 10, 8).unwrap();
        assert_eq!(
            puzzle_url(SourceId::NytMidi, date),
            "https://www.nytimes.com/svc/crosswords/v6/puzzle/midi/2026-10-08.json"
        );
        assert!(puzzle_url(SourceId::NytDaily, date).contains("/daily/"));
    }
}
