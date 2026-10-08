//! The New York Times Mini, Midi and Crossword, for subscribers.
//!
//! These are subscriber puzzles, so every request carries the player's own
//! `NYT-S` cookie, and without one [`super::Fetcher`] makes no request at all.
//! The JSON is the v6 format the NYT web app reads. Each square is either an
//! empty object (a block) or carries its `answer`. A `type` other than 1 marks
//! a circled or shaded square. `moreAnswers.valid` lists other answers that
//! the square accepts, such as another rebus spelling or the second reading
//! of a Schrödinger square.

use std::collections::BTreeMap;

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

/// The player's cookie as a `Cookie` header value. It comes from the `NYT_S`
/// environment variable, or else from `nyt-s` in the config directory
/// (`~/.config/crossword/nyt-s` on Linux).
pub fn configured_cookie() -> Option<String> {
    first_cookie(std::env::var("NYT_S").ok(), || {
        let path = dirs::config_dir()?.join("crossword").join("nyt-s");
        std::fs::read_to_string(path).ok()
    })
}

/// The first token that is not blank, as a `Cookie` header value. A blank
/// `NYT_S`, as from `export NYT_S=`, does not hide the file.
fn first_cookie(env: Option<String>, file: impl FnOnce() -> Option<String>) -> Option<String> {
    let usable = |token: &String| !token.trim().is_empty();
    let token = env.filter(usable).or_else(|| file().filter(usable))?;
    Some(cookie_header(&token))
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
    let mut alternates = BTreeMap::new();
    for (i, cell) in board.cells.into_iter().enumerate() {
        let valid = cell.more_answers.map(|m| m.valid).unwrap_or_default();
        let answer = cell
            .answer
            .filter(|a| !a.trim().is_empty())
            .or_else(|| valid.first().cloned());
        if let Some(answer) = &answer {
            if cell.kind.is_some_and(|k| k != 1) {
                circled.push(i);
            }
            let others: Vec<String> = valid.into_iter().filter(|v| v != answer).collect();
            if !others.is_empty() {
                alternates.insert(i, others);
            }
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

    let copyright = match root.copyright.as_deref().map(str::trim) {
        Some(year) if !year.is_empty() => format!("© {year} The New York Times"),
        _ => "The New York Times".to_string(),
    };
    Ok(PuzzleData {
        meta: Meta {
            title: root.title.unwrap_or_default().trim().to_string(),
            author: join_names(&root.constructors),
            editor: root.editor.unwrap_or_default().trim().to_string(),
            copyright,
            date: root.publication_date,
        },
        width,
        height,
        grid,
        circled,
        alternates,
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
            data.meta.byline().as_deref(),
            Some("by Ada Lovelace and Alan Turing, edited by Grace Hopper")
        );
        assert_eq!(data.meta.copyright, "© 2026 The New York Times");
        assert_eq!(data.meta.date.as_deref(), Some("2026-10-08"));
        assert_eq!(data.circled, [8]);

        let puzzle = Puzzle::new(data).unwrap();
        assert_eq!(puzzle.solution(2), Some("TEA")); // rebus square
        assert_eq!(puzzle.solution(1), Some("A")); // from moreAnswers
        // Square 0 has an answer and also accepts another in moreAnswers.
        assert!(puzzle.accepts(0, "C") && puzzle.accepts(0, "B"));
        assert!(!puzzle.data().alternates.contains_key(&1));
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
    fn byline_without_constructors_names_the_editor() {
        let none = PUZZLE.replace(r#"["Ada Lovelace", "Alan Turing"]"#, "[]");
        let data = parse_puzzle(&none).unwrap();
        assert_eq!(
            data.meta.byline().as_deref(),
            Some("edited by Grace Hopper")
        );
    }

    #[test]
    fn a_blank_environment_cookie_falls_back_to_the_file() {
        let file = || Some("abc\n".to_string());
        assert_eq!(
            first_cookie(Some(" ".into()), file).as_deref(),
            Some("NYT-S=abc")
        );
        assert_eq!(
            first_cookie(Some("xyz".into()), file).as_deref(),
            Some("NYT-S=xyz")
        );
        assert_eq!(first_cookie(None, || Some("\n".into())), None);
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
