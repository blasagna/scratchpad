//! The Daily Princetonian, the student paper at Princeton. It publishes a
//! regular 15×15 and a 5×5 or 7×7 mini through a plain JSON API, with the
//! whole archive available.
//!
//! The API lists puzzles and serves each one's clues with their answers and
//! first squares. The grid size is not stated, so it is the extent of the
//! answers. A lowercase letter in an answer marks a circled square.

use std::cmp::Reverse;

use chrono::NaiveDate;
use serde::Deserialize;

use super::{
    PlacedAnswer, PuzzleRef, SourceError, SourceId, grid_from_answers, is_safe_id, join_names,
};
use crate::puzzle::{Direction, Meta, PuzzleData};

const API: &str = "https://crossword.dailyprincetonian.com/api/crosswords";

pub fn list_url(mini: bool) -> String {
    format!("{API}?mini={mini}")
}

/// The metadata, clues and authors URLs for one puzzle.
pub fn puzzle_urls(id: &str) -> Result<[String; 3], SourceError> {
    if !is_safe_id(id) {
        return Err(SourceError::Parse(format!("unexpected puzzle id {id:?}")));
    }
    Ok([
        format!("{API}/{id}"),
        format!("{API}/{id}/clues"),
        format!("{API}/{id}/authors"),
    ])
}

#[derive(Deserialize)]
struct Listed {
    id: String,
    #[serde(default)]
    date: String,
    #[serde(default)]
    title: Option<String>,
}

#[derive(Deserialize)]
struct Clue {
    answer: String,
    clue: String,
    is_across: bool,
    x: usize,
    y: usize,
}

#[derive(Deserialize)]
struct Author {
    #[serde(default)]
    first_name: String,
    #[serde(default)]
    last_name: String,
}

/// Reads `YYYY-MM-DD` from the start of a timestamp.
fn date_prefix(timestamp: &str) -> Option<NaiveDate> {
    let prefix = timestamp.get(..10)?;
    NaiveDate::parse_from_str(prefix, "%Y-%m-%d").ok()
}

/// Parses the archive listing, newest first.
pub fn parse_list(source: SourceId, body: &str) -> Result<Vec<PuzzleRef>, SourceError> {
    let mut listed: Vec<Listed> = serde_json::from_str(body)?;
    listed.retain(|p| is_safe_id(&p.id));
    let mut refs: Vec<PuzzleRef> = listed
        .into_iter()
        .map(|p| PuzzleRef {
            source,
            date: date_prefix(&p.date),
            title: p.title.filter(|t| !t.trim().is_empty()),
            id: p.id,
        })
        .collect();
    refs.sort_by_key(|r| Reverse(r.date));
    Ok(refs)
}

/// Builds a puzzle from the three API responses.
pub fn parse_puzzle(meta: &str, clues: &str, authors: &str) -> Result<PuzzleData, SourceError> {
    let listed: Listed = serde_json::from_str(meta)?;
    let clues: Vec<Clue> = serde_json::from_str(clues)?;
    let authors: Vec<Author> = serde_json::from_str(authors)?;
    if clues.is_empty() {
        return Err(SourceError::Parse("the puzzle has no clues".into()));
    }

    let names: Vec<String> = authors
        .iter()
        .map(|a| format!("{} {}", a.first_name.trim(), a.last_name.trim()))
        .collect();
    let meta = Meta {
        title: listed.title.unwrap_or_default(),
        author: join_names(&names),
        editor: String::new(),
        copyright: "The Daily Princetonian".into(),
        date: date_prefix(&listed.date).map(|d| d.to_string()),
    };
    let answers: Vec<PlacedAnswer> = clues
        .into_iter()
        .map(|c| PlacedAnswer {
            x: c.x,
            y: c.y,
            direction: if c.is_across {
                Direction::Across
            } else {
                Direction::Down
            },
            answer: c.answer.trim().to_string(),
            clue: c.clue,
        })
        .collect();
    grid_from_answers(meta, None, &answers, true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::puzzle::Puzzle;

    const LIST: &str = include_str!("../../tests/fixtures/princetonian_list.json");
    const META: &str = include_str!("../../tests/fixtures/princetonian_meta.json");
    const CLUES: &str = include_str!("../../tests/fixtures/princetonian_clues.json");
    const AUTHORS: &str = include_str!("../../tests/fixtures/princetonian_authors.json");

    #[test]
    fn list_is_newest_first_and_drops_bad_ids() {
        let refs = parse_list(SourceId::PrincetonianMini, LIST).unwrap();
        let ids: Vec<&str> = refs.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(ids, ["bbbb-2222", "aaaa-1111"]);
        assert_eq!(refs[0].date, NaiveDate::from_ymd_opt(2026, 4, 3));
        assert_eq!(refs[0].title.as_deref(), Some("Second"));
    }

    #[test]
    fn puzzle_is_built_from_placed_answers() {
        let data = parse_puzzle(META, CLUES, AUTHORS).unwrap();
        assert_eq!((data.width, data.height), (3, 3));
        assert_eq!(data.meta.author, "Ada Lovelace and Alan Turing");
        assert_eq!(data.meta.date.as_deref(), Some("2026-04-03"));
        // The lowercase "e" in the fixture's 5-Across marks a circle.
        assert_eq!(data.circled, [8]);

        let puzzle = Puzzle::new(data).unwrap();
        let rows: Vec<String> = (0..3)
            .map(|r| {
                (0..3)
                    .map(|c| puzzle.solution(r * 3 + c).unwrap_or("#"))
                    .collect()
            })
            .collect();
        assert_eq!(rows, ["CAT", "ARE", "#EE"]);
        assert!(puzzle.entries().iter().all(|e| !e.clue.is_empty()));
        assert_eq!(puzzle.entries()[0].clue, "Feline");
    }

    #[test]
    fn ids_are_checked_before_use_in_urls() {
        assert!(puzzle_urls("../../x").is_err());
        assert_eq!(
            puzzle_urls("ab-12").unwrap()[1],
            format!("{API}/ab-12/clues")
        );
    }
}
