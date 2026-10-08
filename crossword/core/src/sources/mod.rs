//! Where puzzles come from.
//!
//! Each puzzle size offers one or more sources, each in its own module. A
//! module turns a publisher's own format into a [`PuzzleData`] with a pure
//! `parse_*` function, which the tests cover with small synthetic fixtures.
//! Only [`Fetcher`] touches the network. It runs on a worker thread and talks
//! to the UI through [`Request`] and [`Response`].
//!
//! The free sources need no account. The NYT sources need the player's own
//! `NYT-S` subscription cookie, and without one they report
//! [`SourceError::NeedsCookie`] instead of making a request.

pub mod guardian;
mod http;
pub mod nyt;
pub mod princetonian;
pub mod universal;

use std::collections::BTreeMap;
use std::fmt;

use chrono::{Days, NaiveDate};

pub use http::Http;

use crate::puzzle::{ClueData, Direction, MAX_SIDE, Meta, PuzzleData, numbering};
use crate::store::Store;

/// How many days back a date-addressed source lists.
pub const LISTING_DAYS: u64 = 60;

/// The three puzzle sizes, after the NYT's Mini, Midi and daily Crossword.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, clap::ValueEnum)]
pub enum Size {
    Mini,
    Midi,
    #[value(alias = "full", alias = "daily")]
    Crossword,
}

impl Size {
    pub const ALL: [Size; 3] = [Size::Mini, Size::Midi, Size::Crossword];

    pub fn name(self) -> &'static str {
        match self {
            Size::Mini => "Mini",
            Size::Midi => "Midi",
            Size::Crossword => "Crossword",
        }
    }

    /// A short description for the home screen.
    pub fn blurb(self) -> &'static str {
        match self {
            Size::Mini => "5×5 to 7×7 · a few minutes",
            Size::Midi => "9×9 to 13×13 · a coffee break",
            Size::Crossword => "15×15 · the daily",
        }
    }

    /// The sources for this size, free ones first.
    pub fn sources(self) -> &'static [SourceId] {
        match self {
            Size::Mini => &[SourceId::PrincetonianMini, SourceId::NytMini],
            Size::Midi => &[SourceId::GuardianQuick, SourceId::NytMidi],
            Size::Crossword => &[
                SourceId::Universal,
                SourceId::Princetonian,
                SourceId::NytDaily,
            ],
        }
    }
}

/// One publisher's series of puzzles.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SourceId {
    PrincetonianMini,
    NytMini,
    GuardianQuick,
    NytMidi,
    Universal,
    Princetonian,
    NytDaily,
}

impl SourceId {
    pub fn name(self) -> &'static str {
        match self {
            SourceId::PrincetonianMini => "Daily Princetonian Mini",
            SourceId::NytMini => "NYT Mini",
            SourceId::GuardianQuick => "Guardian Quick",
            SourceId::NytMidi => "NYT Midi",
            SourceId::Universal => "Universal",
            SourceId::Princetonian => "Daily Princetonian",
            SourceId::NytDaily => "NYT Crossword",
        }
    }

    /// A stable name for cache and progress file paths.
    pub fn slug(self) -> &'static str {
        match self {
            SourceId::PrincetonianMini => "princetonian-mini",
            SourceId::NytMini => "nyt-mini",
            SourceId::GuardianQuick => "guardian-quick",
            SourceId::NytMidi => "nyt-midi",
            SourceId::Universal => "universal",
            SourceId::Princetonian => "princetonian",
            SourceId::NytDaily => "nyt-daily",
        }
    }

    pub fn needs_nyt_cookie(self) -> bool {
        matches!(
            self,
            SourceId::NytMini | SourceId::NytMidi | SourceId::NytDaily
        )
    }
}

/// A pointer to one puzzle in a listing, before it is downloaded.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PuzzleRef {
    pub source: SourceId,
    /// The source's own id: a date, a number or a UUID.
    pub id: String,
    pub date: Option<NaiveDate>,
    pub title: Option<String>,
}

impl PuzzleRef {
    /// A puzzle addressed by its date, whose id is `YYYY-MM-DD`.
    pub fn dated(source: SourceId, date: NaiveDate) -> PuzzleRef {
        PuzzleRef {
            source,
            id: date.format("%Y-%m-%d").to_string(),
            date: Some(date),
            title: None,
        }
    }

    /// A one-line description: the date, the title, or both.
    pub fn label(&self) -> String {
        let date = self.date.map(|d| d.format("%a %b %-d, %Y").to_string());
        match (date, self.title.as_deref()) {
            (Some(date), Some(title)) => format!("{date} · {title}"),
            (Some(date), None) => date,
            (None, Some(title)) => title.to_string(),
            (None, None) => self.id.clone(),
        }
    }

    /// The date as an id, for date-addressed sources.
    fn id_date(&self) -> Result<NaiveDate, SourceError> {
        NaiveDate::parse_from_str(&self.id, "%Y-%m-%d")
            .map_err(|_| SourceError::Parse(format!("{:?} is not a date", self.id)))
    }
}

/// Why a listing or a download failed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SourceError {
    /// No NYT-S cookie is configured for an NYT source.
    NeedsCookie,
    /// The publisher refused the request (HTTP 401 or 403).
    Refused,
    /// No puzzle exists at that address (HTTP 404).
    NotFound,
    /// The request failed: DNS, TLS, a timeout or another HTTP status.
    Network(String),
    /// The response did not have the expected shape.
    Parse(String),
}

impl fmt::Display for SourceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SourceError::NeedsCookie => f.write_str(
                "NYT puzzles need your subscription cookie. \
                 Set NYT_S or write it to ~/.config/crossword/nyt-s (see the README).",
            ),
            SourceError::Refused => f.write_str(
                "The publisher refused the request. If this is an NYT puzzle, \
                 your NYT-S cookie may have expired.",
            ),
            SourceError::NotFound => f.write_str("There is no puzzle at that address."),
            SourceError::Network(e) => write!(f, "Network error: {e}"),
            SourceError::Parse(e) => write!(f, "Unexpected puzzle data: {e}"),
        }
    }
}

impl std::error::Error for SourceError {}

impl From<serde_json::Error> for SourceError {
    fn from(e: serde_json::Error) -> Self {
        SourceError::Parse(e.to_string())
    }
}

impl From<crate::puzzle::PuzzleError> for SourceError {
    fn from(e: crate::puzzle::PuzzleError) -> Self {
        SourceError::Parse(e.0)
    }
}

/// Work for the fetch thread.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Request {
    List(SourceId),
    Fetch(PuzzleRef),
}

impl Request {
    /// The response for a request whose worker failed before it could
    /// answer, so the screen that waits for it does not wait for ever.
    pub fn failed(self, error: SourceError) -> Response {
        match self {
            Request::List(source) => Response::Listed(source, Err(error)),
            Request::Fetch(puzzle) => Response::Fetched(puzzle, Err(error)),
        }
    }
}

/// What the fetch thread sends back. There is one response per player
/// action, so the size gap between the variants costs nothing.
#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(clippy::large_enum_variant)]
pub enum Response {
    Listed(SourceId, Result<Vec<PuzzleRef>, SourceError>),
    Fetched(PuzzleRef, Result<PuzzleData, SourceError>),
}

/// Lists and downloads puzzles, reading and filling the puzzle cache.
pub struct Fetcher {
    http: Http,
    nyt_cookie: Option<String>,
    store: Option<Store>,
}

impl Fetcher {
    pub fn new(nyt_cookie: Option<String>, store: Option<Store>) -> Fetcher {
        Fetcher {
            http: Http::new(),
            nyt_cookie,
            store,
        }
    }

    /// Serves one request. `today` anchors the date-addressed listings.
    pub fn handle(&self, request: Request, today: NaiveDate) -> Response {
        match request {
            Request::List(source) => Response::Listed(source, self.list(source, today)),
            Request::Fetch(puzzle) => {
                let result = self.fetch(&puzzle);
                Response::Fetched(puzzle, result)
            }
        }
    }

    fn cookie(&self, source: SourceId) -> Result<Option<&str>, SourceError> {
        if !source.needs_nyt_cookie() {
            return Ok(None);
        }
        self.nyt_cookie
            .as_deref()
            .map(Some)
            .ok_or(SourceError::NeedsCookie)
    }

    /// The puzzles a source offers, newest first.
    pub fn list(&self, source: SourceId, today: NaiveDate) -> Result<Vec<PuzzleRef>, SourceError> {
        self.cookie(source)?;
        match source {
            SourceId::PrincetonianMini | SourceId::Princetonian => {
                let mini = source == SourceId::PrincetonianMini;
                let body = self.http.get(&princetonian::list_url(mini), None)?;
                princetonian::parse_list(source, &body)
            }
            SourceId::GuardianQuick => {
                let body = self.http.get(guardian::SERIES_URL, None)?;
                Ok(guardian::parse_series(source, &body))
            }
            SourceId::Universal | SourceId::NytMini | SourceId::NytMidi | SourceId::NytDaily => {
                Ok(recent_days(source, today, LISTING_DAYS))
            }
        }
    }

    /// Downloads a puzzle, or reads it from the cache when it is there.
    pub fn fetch(&self, puzzle: &PuzzleRef) -> Result<PuzzleData, SourceError> {
        if let Some(cached) = self.store.as_ref().and_then(|s| s.load_puzzle(puzzle)) {
            return Ok(cached);
        }
        let data = self.download(puzzle)?;
        // Prove the data builds a playable puzzle before caching it.
        crate::puzzle::Puzzle::new(data.clone())?;
        if let Some(store) = &self.store {
            // A failed cache write costs only a later re-download.
            let _ = store.save_puzzle(puzzle, &data);
        }
        Ok(data)
    }

    fn download(&self, puzzle: &PuzzleRef) -> Result<PuzzleData, SourceError> {
        let cookie = self.cookie(puzzle.source)?;
        match puzzle.source {
            SourceId::PrincetonianMini | SourceId::Princetonian => {
                let urls = princetonian::puzzle_urls(&puzzle.id)?;
                let meta = self.http.get(&urls[0], None)?;
                let clues = self.http.get(&urls[1], None)?;
                let authors = self.http.get(&urls[2], None)?;
                princetonian::parse_puzzle(&meta, &clues, &authors)
            }
            SourceId::GuardianQuick => {
                let body = self.http.get(&guardian::puzzle_url(&puzzle.id)?, None)?;
                guardian::parse_puzzle(&body)
            }
            SourceId::Universal => {
                let body = self
                    .http
                    .get(&universal::puzzle_url(puzzle.id_date()?), None)?;
                universal::parse_puzzle(&body)
            }
            SourceId::NytMini | SourceId::NytMidi | SourceId::NytDaily => {
                let url = nyt::puzzle_url(puzzle.source, puzzle.id_date()?);
                let body = self.http.get(&url, cookie)?;
                nyt::parse_puzzle(&body)
            }
        }
    }
}

/// One entry per day for the last `days` days, newest first.
pub fn recent_days(source: SourceId, today: NaiveDate, days: u64) -> Vec<PuzzleRef> {
    (0..days)
        .filter_map(|back| today.checked_sub_days(Days::new(back)))
        .map(|date| PuzzleRef::dated(source, date))
        .collect()
}

/// Joins names the way a byline does: `A`, `A and B`, `A, B and C`.
pub fn join_names(names: &[String]) -> String {
    let names: Vec<&str> = names
        .iter()
        .map(|n| n.trim())
        .filter(|n| !n.is_empty())
        .collect();
    match names.as_slice() {
        [] => String::new(),
        [one] => one.to_string(),
        [rest @ .., last] => format!("{} and {last}", rest.join(", ")),
    }
}

/// Turns clue markup into plain text. It removes HTML tags such as `<i>` and
/// decodes the common entities. A `<` that does not open a tag is kept, so a
/// clue such as `<-- Look left` survives.
pub fn clean_text(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(start) = rest.find('<') {
        out.push_str(&rest[..start]);
        let after = &rest[start + 1..];
        let opens_tag = after
            .chars()
            .next()
            .is_some_and(|c| c.is_ascii_alphabetic() || c == '/');
        match after.find('>') {
            Some(end) if opens_tag && !after[..end].contains('<') => rest = &after[end + 1..],
            _ => {
                out.push('<');
                rest = after;
            }
        }
    }
    out.push_str(rest);

    let decoded = out
        .replace("&nbsp;", " ")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        .replace("&#x27;", "'")
        .replace("&apos;", "'")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&amp;", "&");
    decoded.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// An answer placed by its first square, the way the Princetonian and the
/// Guardian supply them.
#[derive(Debug, Clone)]
pub(crate) struct PlacedAnswer {
    pub x: usize,
    pub y: usize,
    pub direction: Direction,
    /// One character per square.
    pub answer: String,
    pub clue: String,
}

/// Lays placed answers onto a grid. Squares no answer covers are blocks.
/// `size` is the grid size when the source states it. Otherwise the size is
/// the extent of the answers. With `lowercase_circles`, a lowercase letter
/// marks a circled square. An empty answer covers no square, so it is
/// dropped with its clue.
pub(crate) fn grid_from_answers(
    meta: Meta,
    size: Option<(usize, usize)>,
    answers: &[PlacedAnswer],
    lowercase_circles: bool,
) -> Result<PuzzleData, SourceError> {
    let answers: Vec<&PlacedAnswer> = answers.iter().filter(|a| !a.answer.is_empty()).collect();
    let end = |a: &PlacedAnswer| {
        let len = a.answer.chars().count();
        match a.direction {
            Direction::Across => (a.x + len, a.y + 1),
            Direction::Down => (a.x + 1, a.y + len),
        }
    };
    let (width, height) = size.unwrap_or_else(|| {
        answers
            .iter()
            .map(|a| end(a))
            .fold((0, 0), |(w, h), (x, y)| (w.max(x), h.max(y)))
    });
    if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE {
        return Err(SourceError::Parse(format!(
            "grid size {width}x{height} is out of range"
        )));
    }

    let mut grid: Vec<Option<String>> = vec![None; width * height];
    let mut circled = Vec::new();
    for &a in &answers {
        let (x_end, y_end) = end(a);
        if x_end > width || y_end > height {
            return Err(SourceError::Parse(format!(
                "the answer at ({}, {}) runs off the grid",
                a.x, a.y
            )));
        }
        for (i, ch) in a.answer.chars().enumerate() {
            let cell = match a.direction {
                Direction::Across => a.y * width + a.x + i,
                Direction::Down => (a.y + i) * width + a.x,
            };
            if lowercase_circles && ch.is_lowercase() && !circled.contains(&cell) {
                circled.push(cell);
            }
            grid[cell] = Some(ch.to_uppercase().to_string());
        }
    }

    let numbers = numbering(width, height, &grid);
    let clues = answers
        .iter()
        .filter_map(|a| {
            numbers[a.y * width + a.x].map(|number| ClueData {
                direction: a.direction,
                number,
                text: clean_text(&a.clue),
            })
        })
        .collect();
    Ok(PuzzleData {
        meta,
        width,
        height,
        grid,
        circled,
        alternates: BTreeMap::new(),
        clues,
    })
}

/// True for ids that are safe to put in a URL path or a file name.
pub fn is_safe_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 64 && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_size_lists_free_sources_first() {
        for size in Size::ALL {
            let sources = size.sources();
            assert!(!sources[0].needs_nyt_cookie(), "{size:?}");
            assert_eq!(sources.iter().filter(|s| s.needs_nyt_cookie()).count(), 1);
        }
    }

    #[test]
    fn recent_days_count_back_from_today() {
        let today = NaiveDate::from_ymd_opt(2026, 3, 1).unwrap();
        let refs = recent_days(SourceId::Universal, today, 3);
        let ids: Vec<&str> = refs.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(ids, ["2026-03-01", "2026-02-28", "2026-02-27"]);
    }

    #[test]
    fn names_join_like_a_byline() {
        let names = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(join_names(&names(&[])), "");
        assert_eq!(join_names(&names(&["A"])), "A");
        assert_eq!(join_names(&names(&["A", " B "])), "A and B");
        assert_eq!(join_names(&names(&["A", "B", "C"])), "A, B and C");
    }

    #[test]
    fn clean_text_strips_tags_and_entities() {
        assert_eq!(clean_text("<i>Moby-Dick</i> captain"), "Moby-Dick captain");
        assert_eq!(clean_text("Q&amp;A &quot;site&quot;"), "Q&A \"site\"");
        assert_eq!(clean_text("<-- Look left"), "<-- Look left");
        assert_eq!(clean_text("a < b > c"), "a < b > c");
        assert_eq!(clean_text("  two\n lines "), "two lines");
    }

    #[test]
    fn empty_answers_place_nothing() {
        let across = |x, answer: &str| PlacedAnswer {
            x,
            y: 0,
            direction: Direction::Across,
            answer: answer.into(),
            clue: "Clue".into(),
        };
        // The empty answer sits just past the grid's right edge.
        let answers = [across(0, "AB"), across(2, "")];
        let data = grid_from_answers(Meta::default(), None, &answers, false).unwrap();
        assert_eq!((data.width, data.height), (2, 1));
        assert_eq!(data.clues.len(), 1);
        assert!(grid_from_answers(Meta::default(), None, &answers[1..], false).is_err());
    }

    #[test]
    fn unsafe_ids_are_rejected() {
        assert!(is_safe_id("4aa19786-aea3-4ae6-bf63-1fdc76b70dd4"));
        assert!(is_safe_id("17605"));
        assert!(!is_safe_id("../etc"));
        assert!(!is_safe_id("a/b"));
        assert!(!is_safe_id(""));
    }

    #[test]
    fn nyt_sources_need_a_cookie_before_any_request() {
        let fetcher = Fetcher::new(None, None);
        let today = NaiveDate::from_ymd_opt(2026, 3, 1).unwrap();
        assert_eq!(
            fetcher.list(SourceId::NytDaily, today),
            Err(SourceError::NeedsCookie)
        );
        let puzzle = PuzzleRef::dated(SourceId::NytMini, today);
        assert_eq!(fetcher.fetch(&puzzle), Err(SourceError::NeedsCookie));
    }
}
