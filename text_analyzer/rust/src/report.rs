//! Rendering of [`TextStats`] as a human-readable summary or as JSON.

use std::fmt::Write;
use std::io;

use serde::Serialize;
use serde_json::ser::{Formatter, PrettyFormatter};

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
            mean: mean(wl),
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
    // Serialization of these plain owned types into a Vec cannot fail.
    let mut out = Vec::new();
    let mut serializer =
        serde_json::Serializer::with_formatter(&mut out, FixedFloatFormatter::default());
    report
        .serialize(&mut serializer)
        .expect("TextStats JSON serialization cannot fail");
    String::from_utf8(out).expect("serde_json emits UTF-8")
}

/// Pretty JSON, but with floats written to a fixed four decimal places.
///
/// serde_json's default is the shortest representation that round-trips, which
/// prints `3.0` where the C and C++ ports print `3.0000`. Fixing the precision
/// here is what lets all three ports emit byte-identical JSON. Rust's `{:.4}`
/// and C's `printf("%.4f")` both round half to even, so the values agree too —
/// which is why callers must hand this formatter *unrounded* ratios and let it
/// perform the single rounding step.
#[derive(Default)]
struct FixedFloatFormatter<'a> {
    inner: PrettyFormatter<'a>,
}

impl Formatter for FixedFloatFormatter<'_> {
    fn write_f64<W>(&mut self, writer: &mut W, value: f64) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        write!(writer, "{value:.4}")
    }

    fn begin_array<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.begin_array(writer)
    }

    fn end_array<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.end_array(writer)
    }

    fn begin_array_value<W>(&mut self, writer: &mut W, first: bool) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.begin_array_value(writer, first)
    }

    fn end_array_value<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.end_array_value(writer)
    }

    fn begin_object<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.begin_object(writer)
    }

    fn end_object<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.end_object(writer)
    }

    fn begin_object_key<W>(&mut self, writer: &mut W, first: bool) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.begin_object_key(writer, first)
    }

    fn begin_object_value<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.begin_object_value(writer)
    }

    fn end_object_value<W>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: ?Sized + io::Write,
    {
        self.inner.end_object_value(writer)
    }
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

/// Returns `count / total`, or 0.0 when `total` is zero.
///
/// Deliberately unrounded: [`FixedFloatFormatter`] does the rounding when it
/// writes the value. Pre-rounding here with `f64::round` would round halfway
/// cases away from zero, disagreeing with `printf("%.4f")` in the C and C++
/// ports — 5/32 would render as 0.1563 rather than 0.1562.
fn frequency(count: u64, total: u64) -> f64 {
    if total == 0 {
        return 0.0;
    }
    count as f64 / total as f64
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
    fn json_floats_use_fixed_four_decimals() {
        // The C and C++ ports print %.4f; serde's default shortest-round-trip
        // form would emit "3.0" here and break byte-for-byte JSON parity.
        let out = render_json(&sample());
        assert!(out.contains("\"mean\": 3.0000"), "{out}");
    }

    #[test]
    fn json_floats_round_half_to_even() {
        // 5/32 is exactly 0.15625, a halfway case. printf("%.4f") rounds half to
        // even and yields 0.1562; rounding half away from zero (as an f64::round
        // pre-pass would) yields 0.1563 and diverges from the other two ports.
        let stats = TextStats {
            char_count: 32,
            top_chars: vec![CharFreq { ch: 't', count: 5 }],
            ..Default::default()
        };
        let out = render_json(&stats);
        assert!(out.contains("\"frequency\": 0.1562"), "{out}");
    }

    #[test]
    fn json_array_elements_are_expanded() {
        // Each ranked entry occupies its own block; the C and C++ ports match
        // this layout exactly.
        let out = render_json(&sample());
        assert!(
            out.contains("  \"top_words\": [\n    {\n      \"word\": \"the\",\n"),
            "{out}"
        );
    }

    #[test]
    fn empty_stats_render_zeroed_word_length() {
        let out = render_text(&TextStats::default());
        assert!(out.contains("Word length:  mean 0.0, min 0, max 0, p25 0, p50 0, p75 0"));
    }
}
