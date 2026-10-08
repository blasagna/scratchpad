//! Files on disk: downloaded puzzles and the player's progress.
//!
//! Puzzles go under the cache directory, since they can be downloaded again.
//! Progress goes under the data directory. On Linux these are
//! `~/.cache/crossword/puzzles/` and `~/.local/share/crossword/progress/`. Both
//! hold one JSON file per puzzle at `<source slug>/<id>.json`.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use serde::Serialize;
use serde::de::DeserializeOwned;

use crate::game::Progress;
use crate::puzzle::PuzzleData;
use crate::sources::{PuzzleRef, is_safe_id};

/// How far the player has got with a puzzle, for the listing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    New,
    Started,
    Solved,
}

#[derive(Debug, Clone)]
pub struct Store {
    puzzles: PathBuf,
    progress: PathBuf,
}

impl Store {
    /// A store rooted at two directories, for tests and custom setups.
    pub fn new(cache_dir: impl AsRef<Path>, data_dir: impl AsRef<Path>) -> Store {
        Store {
            puzzles: cache_dir.as_ref().join("puzzles"),
            progress: data_dir.as_ref().join("progress"),
        }
    }

    /// The platform's cache and data directories, under `crossword/`.
    pub fn from_system_dirs() -> Option<Store> {
        Some(Store::new(
            dirs::cache_dir()?.join("crossword"),
            dirs::data_dir()?.join("crossword"),
        ))
    }

    fn file(root: &Path, puzzle: &PuzzleRef) -> Option<PathBuf> {
        // Ids come from the network; never let one escape the directory.
        is_safe_id(&puzzle.id).then(|| {
            root.join(puzzle.source.slug())
                .join(format!("{}.json", puzzle.id))
        })
    }

    pub fn load_puzzle(&self, puzzle: &PuzzleRef) -> Option<PuzzleData> {
        read_json(&Store::file(&self.puzzles, puzzle)?)
    }

    pub fn save_puzzle(&self, puzzle: &PuzzleRef, data: &PuzzleData) -> io::Result<()> {
        write_json(
            &Store::file(&self.puzzles, puzzle).ok_or_else(bad_id)?,
            data,
        )
    }

    pub fn load_progress(&self, puzzle: &PuzzleRef) -> Option<Progress> {
        read_json(&Store::file(&self.progress, puzzle)?)
    }

    pub fn save_progress(&self, puzzle: &PuzzleRef, progress: &Progress) -> io::Result<()> {
        write_json(
            &Store::file(&self.progress, puzzle).ok_or_else(bad_id)?,
            progress,
        )
    }

    pub fn status(&self, puzzle: &PuzzleRef) -> Status {
        self.load_progress(puzzle)
            .map_or(Status::New, |p| p.status())
    }
}

fn bad_id() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, "unsafe puzzle id")
}

/// Reads a JSON file. A missing or damaged file reads as nothing.
fn read_json<T: DeserializeOwned>(path: &Path) -> Option<T> {
    let text = fs::read_to_string(path).ok()?;
    serde_json::from_str(&text).ok()
}

/// Writes through a temporary file and a rename, so a crash mid-write never
/// leaves half a file behind. Each write has a temporary file of its own:
/// two writers of one file at once, such as the TUI and the GUI saving the
/// same puzzle, cannot mix their bytes, and the last rename wins.
fn write_json<T: Serialize>(path: &Path, value: &T) -> io::Result<()> {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension(format!(
        "json.{}-{}.tmp",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let written = fs::write(&tmp, serde_json::to_vec(value)?).and_then(|()| fs::rename(&tmp, path));
    if written.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    written
}

#[cfg(test)]
mod tests {
    use std::time::Instant;

    use super::*;
    use crate::game::Game;
    use crate::puzzle::Puzzle;
    use crate::puzzle::tests::small;
    use crate::sources::SourceId;

    fn reference(id: &str) -> PuzzleRef {
        PuzzleRef {
            source: SourceId::Universal,
            id: id.into(),
            date: None,
            title: None,
        }
    }

    #[test]
    fn puzzles_and_progress_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path().join("cache"), dir.path().join("data"));
        let r = reference("2026-10-08");

        assert!(store.load_puzzle(&r).is_none());
        store.save_puzzle(&r, small().data()).unwrap();
        let loaded = Puzzle::new(store.load_puzzle(&r).unwrap()).unwrap();
        assert_eq!(&loaded, &small());
        assert!(
            dir.path()
                .join("cache/puzzles/universal/2026-10-08.json")
                .exists()
        );

        assert_eq!(store.status(&r), Status::New);
        let mut game = Game::new(small());
        game.type_letters("C");
        store
            .save_progress(&r, &game.progress(Instant::now()))
            .unwrap();
        assert_eq!(store.status(&r), Status::Started);
        assert!(
            dir.path()
                .join("data/progress/universal/2026-10-08.json")
                .exists()
        );
    }

    #[test]
    fn unsafe_ids_never_touch_the_disk() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path(), dir.path());
        let r = reference("../../escape");
        assert!(store.save_puzzle(&r, small().data()).is_err());
        assert!(store.load_puzzle(&r).is_none());
    }

    #[test]
    fn concurrent_writes_of_one_file_all_land_whole() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path(), dir.path());
        let r = reference("2026-10-08");
        let data = small().data().clone();
        std::thread::scope(|scope| {
            for _ in 0..8 {
                scope.spawn(|| {
                    for _ in 0..25 {
                        store.save_puzzle(&r, &data).unwrap();
                    }
                });
            }
        });
        assert_eq!(store.load_puzzle(&r), Some(data));
        // No temporary file is left behind.
        let files = fs::read_dir(dir.path().join("puzzles/universal")).unwrap();
        assert_eq!(files.count(), 1);
    }

    #[test]
    fn damaged_files_read_as_missing() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::new(dir.path(), dir.path());
        let r = reference("x1");
        let path = dir.path().join("progress/universal/x1.json");
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, "{not json").unwrap();
        assert!(store.load_progress(&r).is_none());
        assert_eq!(store.status(&r), Status::New);
    }
}
