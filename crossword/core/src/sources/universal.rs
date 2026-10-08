//! Universal, the daily syndicated 15×15 from Andrews McMeel, edited by David
//! Steinberg. Its embed serves each day's puzzle as JSON at a fixed address
//! with the date in it. The archive goes back years, and the next day's
//! puzzle often appears early.

use chrono::NaiveDate;
use serde::{Deserialize, Deserializer};

use super::SourceError;
use crate::puzzle::{ClueData, Direction, Meta, PuzzleData};

/// The public data path the Universal embed reads from. The long segment is a
/// fixed token from the embed's configuration, not a credential.
const BASE: &str = "https://gamedata.services.amuniversal.com/c/uucom/l/\
    U2FsdGVkX18YuMv20%2B8cekf85%2Friz1H%2FzlWW4bn0cizt8yclLsp7UYv34S77X0aX\
    %0Axa513fPTc5RoN2wa0h4ED9QWuBURjkqWgHEZey0WFL8%3D/g/fcx/d/";

pub fn puzzle_url(date: NaiveDate) -> String {
    format!("{BASE}{}/data.json", date.format("%Y-%m-%d"))
}

#[derive(Deserialize)]
#[serde(rename_all = "PascalCase")]
struct Universal {
    #[serde(default)]
    title: String,
    #[serde(default)]
    author: String,
    #[serde(default)]
    editor: String,
    #[serde(default)]
    copyright: String,
    /// `YYYYMMDD`.
    #[serde(default)]
    date: String,
    #[serde(deserialize_with = "number_or_string")]
    width: usize,
    #[serde(deserialize_with = "number_or_string")]
    height: usize,
    all_answer: String,
    across_clue: String,
    down_clue: String,
}

/// Accepts `15` or `"15"`; the feed uses strings.
fn number_or_string<'de, D: Deserializer<'de>>(d: D) -> Result<usize, D::Error> {
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Either {
        Number(usize),
        Text(String),
    }
    match Either::deserialize(d)? {
        Either::Number(n) => Ok(n),
        Either::Text(s) => s.trim().parse().map_err(serde::de::Error::custom),
    }
}

/// Parses `NN|clue` lines. Lines without a number are skipped.
fn parse_clues(direction: Direction, text: &str) -> Vec<ClueData> {
    text.lines()
        .filter_map(|line| {
            let (number, clue) = line.split_once('|')?;
            Some(ClueData {
                direction,
                number: number.trim().parse().ok()?,
                text: super::clean_text(clue),
            })
        })
        .collect()
}

pub fn parse_puzzle(body: &str) -> Result<PuzzleData, SourceError> {
    let u: Universal = serde_json::from_str(body)?;
    let grid: Vec<Option<String>> = u
        .all_answer
        .trim()
        .chars()
        .map(|c| match c {
            '-' | '#' | '.' => None,
            c => Some(c.to_string()),
        })
        .collect();
    if grid.len() != u.width * u.height {
        return Err(SourceError::Parse(format!(
            "{} answer squares for a {}x{} grid",
            grid.len(),
            u.width,
            u.height
        )));
    }

    let author = u.author.trim();
    let author = author
        .strip_prefix("By ")
        .or_else(|| author.strip_prefix("by "))
        .unwrap_or(author)
        .trim();
    let author = match u.editor.trim() {
        "" => author.to_string(),
        editor => format!("{author}, edited by {editor}"),
    };
    let date = NaiveDate::parse_from_str(u.date.trim(), "%Y%m%d")
        .ok()
        .map(|d| d.to_string());

    let mut clues = parse_clues(Direction::Across, &u.across_clue);
    clues.extend(parse_clues(Direction::Down, &u.down_clue));
    Ok(PuzzleData {
        meta: Meta {
            title: u.title.trim().to_string(),
            author,
            copyright: u.copyright.trim().to_string(),
            date,
        },
        width: u.width,
        height: u.height,
        grid,
        circled: Vec::new(),
        clues,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::puzzle::Puzzle;

    const PUZZLE: &str = include_str!("../../tests/fixtures/universal.json");

    #[test]
    fn puzzle_parses_grid_clues_and_byline() {
        let data = parse_puzzle(PUZZLE).unwrap();
        assert_eq!(data.meta.author, "Ada Lovelace, edited by Grace Hopper");
        assert_eq!(data.meta.date.as_deref(), Some("2026-10-08"));
        let puzzle = Puzzle::new(data).unwrap();
        assert!(!puzzle.is_open(6));
        let clues: Vec<(u32, char, &str)> = puzzle
            .entries()
            .iter()
            .map(|e| (e.number, e.direction.letter(), e.clue.as_str()))
            .collect();
        assert_eq!(
            clues,
            [
                (1, 'A', "Feline"),
                (4, 'A', "Exist"),
                (5, 'A', "Reddit Q&A, e.g."),
                (1, 'D', "Golden State, briefly"),
                (2, 'D', "Exist"),
                (3, 'D', "Golf peg"),
            ]
        );
    }

    #[test]
    fn wrong_answer_length_is_an_error() {
        let short = PUZZLE.replace("CATARE-EE", "CATARE-E");
        assert!(parse_puzzle(&short).is_err());
    }

    #[test]
    fn url_carries_the_date() {
        let date = NaiveDate::from_ymd_opt(2026, 1, 2).unwrap();
        assert!(puzzle_url(date).ends_with("/d/2026-01-02/data.json"));
    }
}
