//! Golden tests: render every case in `testdata/` and compare against the
//! committed expected output.
//!
//! The C and C++ ports run the same corpus against the same golden files, so all
//! three agreeing with the goldens means all three agree with each other. That is
//! the project's central invariant, and this is what enforces it.
//!
//! Regenerate the goldens with `testdata/regenerate.sh` after an intentional
//! behavior change.

use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};

use text_analyzer::{Config, analyze, render_json, render_text};

const TESTDATA_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../testdata");

/// Must stay in sync with `regenerate.sh`'s `ALT_FLAGS` and the other two ports.
fn alt_config() -> Config {
    Config {
        top_n: 3,
        max_word_len: 5,
        ..Config::default()
    }
}

fn read(path: &Path) -> String {
    fs::read_to_string(path).unwrap_or_else(|err| panic!("cannot read {}: {err}", path.display()))
}

/// Renders one input with the given config, as text or JSON.
fn render(input: &Path, config: &Config, json: bool) -> String {
    let bytes =
        fs::read(input).unwrap_or_else(|err| panic!("cannot read {}: {err}", input.display()));
    let stats = analyze(Cursor::new(bytes), config).expect("in-memory read cannot fail");
    if json {
        // The CLI prints JSON with println!, so the golden file has a trailing
        // newline that render_json does not produce.
        format!("{}\n", render_json(&stats))
    } else {
        render_text(&stats)
    }
}

#[test]
fn matches_committed_output() {
    let dir = PathBuf::from(TESTDATA_DIR);
    let cases: Vec<String> = read(&dir.join("cases.txt"))
        .lines()
        .filter(|line| !line.is_empty())
        .map(str::to_string)
        .collect();

    // A golden suite that silently finds no data would pass while proving
    // nothing, so treat an empty corpus as a failure.
    assert!(
        !cases.is_empty(),
        "no cases found in {}/cases.txt",
        dir.display()
    );

    let defaults = Config::default();
    let alt = alt_config();

    for name in &cases {
        let input = dir.join(name);
        let golden = |suffix: &str| read(&dir.join(format!("{name}{suffix}")));

        assert_eq!(render(&input, &defaults, false), golden(".out"), "{name}");
        assert_eq!(render(&input, &defaults, true), golden(".json"), "{name}");
        assert_eq!(render(&input, &alt, false), golden(".alt.out"), "{name}");
        assert_eq!(render(&input, &alt, true), golden(".alt.json"), "{name}");
    }
}
