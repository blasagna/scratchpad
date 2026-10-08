//! The Guardian's Quick crossword: a 13×13 with plain definitions for clues,
//! published Monday to Saturday. It stands in for the NYT Midi because no free
//! 9×9 to 11×11 series serves its puzzles as plain data.
//!
//! The grids follow British style, so many squares are checked one way only.
//! Each clue ends with its enumeration, such as `(5,3)`.
//!
//! Each puzzle page also answers at `.json`, with the crossword under a
//! `crossword` key. The series page is HTML. It links the latest twenty or so
//! puzzles, each followed by a `<time dateTime>` tag.

use std::cmp::Reverse;

use chrono::{DateTime, Duration, NaiveDate, Utc};
use serde::Deserialize;

use super::{PlacedAnswer, PuzzleRef, SourceError, SourceId, grid_from_answers, is_safe_id};
use crate::puzzle::{Direction, Meta, PuzzleData};

pub const SERIES_URL: &str = "https://www.theguardian.com/crosswords/series/quick";
const LINK: &str = "/crosswords/quick/";

pub fn puzzle_url(id: &str) -> Result<String, SourceError> {
    if !is_safe_id(id) || !id.chars().all(|c| c.is_ascii_digit()) {
        return Err(SourceError::Parse(format!("unexpected puzzle id {id:?}")));
    }
    Ok(format!("https://www.theguardian.com{LINK}{id}.json"))
}

/// The day a timestamp falls on in Britain. Puzzles go up at midnight UK
/// time, which is 23:00 or 00:00 UTC. Rounding to the nearest UTC day gives
/// the British date without a time zone database.
fn uk_day(at: DateTime<Utc>) -> NaiveDate {
    (at + Duration::hours(12)).date_naive()
}

/// Finds the puzzle links on the series page, newest first. Each link's date
/// comes from the first `dateTime` attribute after it, before the next link
/// to a different puzzle. The page also repeats each link for the thumbnail.
pub fn parse_series(source: SourceId, html: &str) -> Vec<PuzzleRef> {
    let mut links: Vec<(usize, u32)> = Vec::new();
    let mut from = 0;
    while let Some(found) = html[from..].find(LINK) {
        let start = from + found + LINK.len();
        let digits: String = html[start..]
            .chars()
            .take_while(char::is_ascii_digit)
            .collect();
        if let Ok(number) = digits.parse::<u32>() {
            links.push((start, number));
        }
        from = start;
    }

    let mut refs: Vec<PuzzleRef> = Vec::new();
    for (i, &(at, number)) in links.iter().enumerate() {
        if refs.iter().any(|r| r.id == number.to_string()) {
            continue;
        }
        let end = links[i + 1..]
            .iter()
            .find(|&&(_, n)| n != number)
            .map_or(html.len(), |&(pos, _)| pos);
        let date = html[at..end].find("dateTime=\"").and_then(|t| {
            let value = &html[at + t + 10..end];
            let value = &value[..value.find('"')?];
            DateTime::parse_from_rfc3339(value)
                .ok()
                .map(|d| uk_day(d.with_timezone(&Utc)))
        });
        refs.push(PuzzleRef {
            source,
            id: number.to_string(),
            date,
            title: Some(format!("No {}", group_thousands(number))),
        });
    }
    refs.sort_by_key(|r| Reverse(r.id.parse::<u32>().ok()));
    refs
}

fn group_thousands(n: u32) -> String {
    let digits = n.to_string();
    let mut out = String::new();
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(c);
    }
    out
}

#[derive(Deserialize)]
struct Page {
    crossword: Crossword,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Crossword {
    #[serde(default)]
    name: String,
    /// Milliseconds since the epoch.
    #[serde(default)]
    date: Option<i64>,
    dimensions: Dimensions,
    entries: Vec<GuardianEntry>,
    #[serde(default)]
    creator: Option<Creator>,
}

#[derive(Deserialize)]
struct Dimensions {
    cols: usize,
    rows: usize,
}

#[derive(Deserialize)]
struct Creator {
    #[serde(default)]
    name: String,
}

#[derive(Deserialize)]
struct GuardianEntry {
    direction: String,
    position: Position,
    #[serde(default)]
    solution: Option<String>,
    clue: String,
    length: usize,
}

#[derive(Deserialize)]
struct Position {
    x: usize,
    y: usize,
}

/// Parses a puzzle page's JSON.
pub fn parse_puzzle(body: &str) -> Result<PuzzleData, SourceError> {
    let crossword = serde_json::from_str::<Page>(body)
        .map(|p| p.crossword)
        .or_else(|_| serde_json::from_str::<Crossword>(body))?;

    let mut answers = Vec::with_capacity(crossword.entries.len());
    for entry in &crossword.entries {
        let Some(solution) = entry.solution.as_deref().filter(|s| !s.is_empty()) else {
            return Err(SourceError::Parse(
                "the Guardian has not published this solution yet".into(),
            ));
        };
        if solution.chars().count() != entry.length {
            return Err(SourceError::Parse(format!(
                "the solution {solution:?} does not have {} letters",
                entry.length
            )));
        }
        let direction = match entry.direction.as_str() {
            "across" => Direction::Across,
            "down" => Direction::Down,
            other => return Err(SourceError::Parse(format!("unknown direction {other:?}"))),
        };
        answers.push(PlacedAnswer {
            x: entry.position.x,
            y: entry.position.y,
            direction,
            answer: solution.to_string(),
            clue: entry.clue.clone(),
        });
    }

    let meta = Meta {
        title: crossword.name,
        author: crossword.creator.map(|c| c.name).unwrap_or_default(),
        copyright: "Guardian News & Media".into(),
        date: crossword
            .date
            .and_then(DateTime::from_timestamp_millis)
            .map(|d| uk_day(d).to_string()),
    };
    let size = (crossword.dimensions.cols, crossword.dimensions.rows);
    grid_from_answers(meta, Some(size), &answers, false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::puzzle::Puzzle;

    const SERIES: &str = include_str!("../../tests/fixtures/guardian_series.html");
    const PUZZLE: &str = include_str!("../../tests/fixtures/guardian_quick.json");

    #[test]
    fn series_lists_each_puzzle_once_with_its_date() {
        let refs = parse_series(SourceId::GuardianQuick, SERIES);
        let summary: Vec<(&str, Option<String>, Option<&str>)> = refs
            .iter()
            .map(|r| {
                (
                    r.id.as_str(),
                    r.date.map(|d| d.to_string()),
                    r.title.as_deref(),
                )
            })
            .collect();
        assert_eq!(
            summary,
            [
                ("17605", Some("2026-10-08".into()), Some("No 17,605")),
                ("17604", Some("2026-10-07".into()), Some("No 17,604")),
                ("9", None, Some("No 9")),
            ]
        );
    }

    #[test]
    fn puzzle_uses_stated_size_and_cleans_clues() {
        let data = parse_puzzle(PUZZLE).unwrap();
        assert_eq!((data.width, data.height), (3, 3));
        assert_eq!(data.meta.title, "Quick crossword No 1");
        assert_eq!(data.meta.date.as_deref(), Some("2025-10-16"));

        let puzzle = Puzzle::new(data).unwrap();
        // The middle square is checked only by 2-Down: blocks sit either side.
        assert_eq!(puzzle.solution(4), Some("O"));
        assert_eq!(puzzle.entry_at(4, Direction::Across), None);
        let clues: Vec<(u32, char, &str)> = puzzle
            .entries()
            .iter()
            .map(|e| (e.number, e.direction.letter(), e.clue.as_str()))
            .collect();
        assert_eq!(
            clues,
            [
                (1, 'A', "Water carrier (3)"),
                (3, 'A', "Afternoon cuppa & cake? (3)"),
                (2, 'D', "Fish eggs (3)"),
            ]
        );
    }

    #[test]
    fn missing_solution_is_an_error() {
        let hidden = PUZZLE.replace(", \"solution\": \"TEA\"", "");
        assert_ne!(hidden, PUZZLE);
        assert!(parse_puzzle(&hidden).is_err());
    }

    #[test]
    fn puzzle_ids_must_be_numbers() {
        assert!(puzzle_url("17605").is_ok());
        assert!(puzzle_url("17605-x").is_err());
    }

    #[test]
    fn thousands_are_grouped() {
        assert_eq!(group_thousands(9), "9");
        assert_eq!(group_thousands(17605), "17,605");
        assert_eq!(group_thousands(1234567), "1,234,567");
    }
}
