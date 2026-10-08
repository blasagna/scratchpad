//! Downloads recent puzzles from each source and checks that each one builds
//! a complete puzzle, with a clue for every entry. These tests need the
//! network, so they are ignored by default. Run them with:
//!
//! ```sh
//! cargo test -p crossword --test live -- --ignored
//! ```
//!
//! The NYT test runs only when `NYT_S` holds a subscription cookie.

use chrono::Local;
use crossword::puzzle::Puzzle;
use crossword::sources::{Fetcher, SourceId, nyt};

fn check_latest(fetcher: &Fetcher, source: SourceId) {
    let today = Local::now().date_naive();
    let refs = fetcher
        .list(source, today)
        .unwrap_or_else(|e| panic!("{}: listing failed: {e}", source.name()));
    assert!(!refs.is_empty(), "{}: empty listing", source.name());

    // Today's puzzle may not be up yet, so try the latest few.
    let (r, data) = refs
        .iter()
        .take(3)
        .find_map(|r| fetcher.fetch(r).ok().map(|d| (r, d)))
        .unwrap_or_else(|| panic!("{}: no recent puzzle downloaded", source.name()));
    let puzzle = Puzzle::new(data).unwrap();
    let unclued: Vec<String> = puzzle
        .entries()
        .iter()
        .filter(|e| e.clue.is_empty())
        .map(|e| format!("{}{}", e.number, e.direction.letter()))
        .collect();
    assert!(
        unclued.is_empty(),
        "{} {}: entries without clues: {unclued:?}",
        source.name(),
        r.id
    );
    eprintln!(
        "{} {}: {}x{}, {} entries, {:?}",
        source.name(),
        r.id,
        puzzle.width(),
        puzzle.height(),
        puzzle.entries().len(),
        puzzle.meta().title
    );
}

#[test]
#[ignore = "needs the network"]
fn princetonian_mini() {
    check_latest(&Fetcher::new(None, None), SourceId::PrincetonianMini);
}

#[test]
#[ignore = "needs the network"]
fn princetonian() {
    check_latest(&Fetcher::new(None, None), SourceId::Princetonian);
}

#[test]
#[ignore = "needs the network"]
fn guardian_quick() {
    check_latest(&Fetcher::new(None, None), SourceId::GuardianQuick);
}

#[test]
#[ignore = "needs the network"]
fn universal() {
    check_latest(&Fetcher::new(None, None), SourceId::Universal);
}

#[test]
#[ignore = "needs the network and an NYT subscription cookie in NYT_S"]
fn nyt() {
    let Ok(token) = std::env::var("NYT_S") else {
        eprintln!("NYT_S is not set; skipping");
        return;
    };
    let fetcher = Fetcher::new(Some(nyt::cookie_header(&token)), None);
    for source in [SourceId::NytMini, SourceId::NytMidi, SourceId::NytDaily] {
        check_latest(&fetcher, source);
    }
}
