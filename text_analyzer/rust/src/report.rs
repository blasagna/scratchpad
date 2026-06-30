//! Rendering of [`TextStats`] as a human-readable summary or as JSON.

use std::fmt::Write;

use serde::Serialize;

use crate::analyzer::TextStats;

/// Builds a human-readable summary: line/word/character totals followed by
/// ranked lists of the top words and characters, each with its count and a
/// percentage of the relevant total.
pub fn render_text(stats: &TextStats) -> String {
    let mut out = String::new();
    // String writes are infallible, so the `write!` results cannot error.
    let _ = writeln!(out, "Lines:      {}", stats.line_count);
    let _ = writeln!(out, "Words:      {}", stats.word_count);
    let _ = writeln!(out, "Characters: {}", stats.char_count);

    let _ = write!(out, "\nTop words:\n");
    for (i, w) in stats.top_words.iter().enumerate() {
        let pct = percentage(w.count, stats.word_count);
        let _ = writeln!(out, "  {}. {} ({}, {:.1}%)", i + 1, w.word, w.count, pct);
    }

    let _ = write!(out, "\nTop characters:\n");
    for (i, c) in stats.top_chars.iter().enumerate() {
        let pct = percentage(c.count, stats.char_count);
        let _ = writeln!(out, "  {}. '{}' ({}, {:.1}%)", i + 1, c.ch, c.count, pct);
    }

    out
}

/// Builds a single JSON object with line/word/character totals plus `top_words`
/// and `top_characters` arrays. Each entry carries its count and a frequency
/// expressed as a ratio in `[0, 1]`, rounded to four decimal places.
pub fn render_json(stats: &TextStats) -> String {
    let report = JsonReport {
        lines: stats.line_count,
        words: stats.word_count,
        characters: stats.char_count,
        top_words: stats
            .top_words
            .iter()
            .map(|w| JsonWord {
                word: w.word.clone(),
                count: w.count,
                frequency: frequency(w.count, stats.word_count),
            })
            .collect(),
        top_characters: stats
            .top_chars
            .iter()
            .map(|c| JsonChar {
                char: c.ch.to_string(),
                count: c.count,
                frequency: frequency(c.count, stats.char_count),
            })
            .collect(),
    };
    // Serialization of these plain owned types cannot fail.
    serde_json::to_string_pretty(&report).expect("TextStats JSON serialization cannot fail")
}

/// Returns `count` as a percentage of `total`, or 0.0 when `total` is zero.
fn percentage(count: u64, total: u64) -> f64 {
    if total == 0 {
        0.0
    } else {
        100.0 * count as f64 / total as f64
    }
}

/// Returns `count / total` rounded to four decimals, or 0.0 when `total` is zero.
fn frequency(count: u64, total: u64) -> f64 {
    if total == 0 {
        return 0.0;
    }
    let ratio = count as f64 / total as f64;
    (ratio * 1e4).round() / 1e4
}

#[derive(Serialize)]
struct JsonReport {
    lines: u64,
    words: u64,
    characters: u64,
    top_words: Vec<JsonWord>,
    top_characters: Vec<JsonChar>,
}

#[derive(Serialize)]
struct JsonWord {
    word: String,
    count: u64,
    frequency: f64,
}

#[derive(Serialize)]
struct JsonChar {
    char: String,
    count: u64,
    frequency: f64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::analyzer::{CharFreq, WordFreq};

    fn sample() -> TextStats {
        TextStats {
            line_count: 1,
            word_count: 3,
            char_count: 11,
            top_words: vec![
                WordFreq {
                    word: "the".to_string(),
                    count: 2,
                },
                WordFreq {
                    word: "cat".to_string(),
                    count: 1,
                },
            ],
            top_chars: vec![CharFreq { ch: 't', count: 3 }],
        }
    }

    #[test]
    fn json_contains_expected_fields() {
        let out = render_json(&sample());
        assert!(out.contains("\"words\": 3"));
        assert!(out.contains("\"word\": \"the\""));
        assert!(out.contains("\"count\": 2"));
        assert!(out.contains("\"top_characters\""));
    }

    #[test]
    fn text_contains_totals_and_top_word() {
        let out = render_text(&sample());
        assert!(out.contains("Lines:      1"));
        assert!(out.contains("Words:      3"));
        assert!(out.contains("Characters: 11"));
        assert!(out.contains("1. the (2,"));
    }
}
