//! Rendering of [`TextStats`] as a human-readable summary or as JSON.

use std::fmt::Write;

use serde::Serialize;

use crate::analyzer::{TextStats, WordLengthStats};

/// Width of the label column in the text summary, sized for the longest label.
const LABEL_WIDTH: usize = 13;

/// Builds a human-readable summary: line/word/character totals followed by the
/// word length distribution and ranked lists of the top words and characters,
/// each with its count and a percentage of the relevant total.
pub fn render_text(stats: &TextStats) -> String {
    let mut out = String::new();
    // String writes are infallible, so the `write!` results cannot error.
    let mut row = |label: &str, value: u64| {
        let _ = writeln!(out, "{label:<LABEL_WIDTH$} {value}");
    };
    row("Lines:", stats.line_count);
    row("Blank lines:", stats.blank_line_count);
    row("Words:", stats.word_count);
    row("Characters:", stats.char_count);
    row("Digits:", stats.digit_count);
    row("Punctuation:", stats.punct_count);

    let wl = &stats.word_length;
    let _ = writeln!(
        out,
        "\n{:<LABEL_WIDTH$} mean {:.1}, min {}, max {}, p25 {}, p50 {}, p75 {}",
        "Word length:",
        mean(wl),
        wl.min,
        wl.max,
        wl.p25,
        wl.p50,
        wl.p75
    );

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

/// Builds a single JSON object with line/word/character totals, the word length
/// distribution, and `top_words` and `top_characters` arrays. Each entry carries
/// its count and a frequency expressed as a ratio in `[0, 1]`, rounded to four
/// decimal places.
pub fn render_json(stats: &TextStats) -> String {
    let wl = &stats.word_length;
    let report = JsonReport {
        lines: stats.line_count,
        blank_lines: stats.blank_line_count,
        words: stats.word_count,
        characters: stats.char_count,
        digits: stats.digit_count,
        punctuation: stats.punct_count,
        word_length: JsonWordLength {
            mean: round4(mean(wl)),
            min: wl.min,
            max: wl.max,
            p25: wl.p25,
            p50: wl.p50,
            p75: wl.p75,
        },
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

/// Returns the mean word length, or 0.0 when no words were seen.
fn mean(wl: &WordLengthStats) -> f64 {
    if wl.count == 0 {
        0.0
    } else {
        wl.sum as f64 / wl.count as f64
    }
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
    round4(count as f64 / total as f64)
}

/// Rounds to four decimal places, the precision used throughout the JSON output.
fn round4(value: f64) -> f64 {
    (value * 1e4).round() / 1e4
}

#[derive(Serialize)]
struct JsonReport {
    lines: u64,
    blank_lines: u64,
    words: u64,
    characters: u64,
    digits: u64,
    punctuation: u64,
    word_length: JsonWordLength,
    top_words: Vec<JsonWord>,
    top_characters: Vec<JsonChar>,
}

#[derive(Serialize)]
struct JsonWordLength {
    mean: f64,
    min: u64,
    max: u64,
    p25: u64,
    p50: u64,
    p75: u64,
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
            blank_line_count: 0,
            word_count: 3,
            char_count: 11,
            digit_count: 0,
            punct_count: 1,
            word_length: WordLengthStats {
                count: 3,
                sum: 9,
                min: 3,
                max: 3,
                p25: 3,
                p50: 3,
                p75: 3,
            },
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
        assert!(out.contains("\"blank_lines\": 0"));
        assert!(out.contains("\"punctuation\": 1"));
        assert!(out.contains("\"mean\": 3.0"));
        assert!(out.contains("\"p50\": 3"));
    }

    #[test]
    fn text_contains_totals_and_top_word() {
        let out = render_text(&sample());
        assert!(out.contains("Lines:        1"));
        assert!(out.contains("Words:        3"));
        assert!(out.contains("Characters:   11"));
        assert!(out.contains("Punctuation:  1"));
        assert!(out.contains("Word length:  mean 3.0, min 3, max 3, p25 3, p50 3, p75 3"));
        assert!(out.contains("1. the (2,"));
    }

    #[test]
    fn empty_stats_render_zeroed_word_length() {
        let out = render_text(&TextStats::default());
        assert!(out.contains("Word length:  mean 0.0, min 0, max 0, p25 0, p50 0, p75 0"));
    }
}
