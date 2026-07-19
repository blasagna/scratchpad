//! Property tests over the public API.
//!
//! These assert invariants of the shared semantics — the ones that must hold for
//! every input, and so are awkward to pin down with example-based tests. The
//! chunk-invariance property in particular guards the accumulator that makes
//! multi-file and stdin input work.

use std::io::Cursor;

use proptest::prelude::*;

use text_analyzer::{Accum, Config, TextStats, analyze, render_json, render_text};

/// Bytes weighted toward text-shaped input, so generated cases land in
/// interesting states (word boundaries, blank lines, ties) far more often than
/// uniformly random bytes would.
fn texty() -> impl Strategy<Value = Vec<u8>> {
    let byte = prop_oneof![
        10 => prop::sample::select(b"abcdefghijklmnopqrstuvwxyz".to_vec()),
        4 => prop::sample::select(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ".to_vec()),
        6 => Just(b' '),
        4 => Just(b'\n'),
        2 => Just(b'\t'),
        2 => prop::sample::select(b"0123456789".to_vec()),
        2 => prop::sample::select(b".,!?;:'\"\\-".to_vec()),
        1 => (0x80u8..=0xffu8).boxed(),
    ];
    prop::collection::vec(byte, 0..400)
}

/// Unrestricted bytes, including NUL and invalid UTF-8.
fn any_bytes() -> impl Strategy<Value = Vec<u8>> {
    prop::collection::vec(any::<u8>(), 0..300)
}

/// Configs including the degenerate-but-defined settings (zero top_n, zero
/// max_word_len), which the library documents as legal.
fn config() -> impl Strategy<Value = Config> {
    (0usize..=8, 0usize..=12, 0usize..=16).prop_map(|(top_n, max_word_len, word_table_cap)| {
        Config {
            top_n,
            max_word_len,
            word_table_cap,
        }
    })
}

fn analyze_bytes(bytes: &[u8], config: &Config) -> TextStats {
    analyze(Cursor::new(bytes.to_vec()), config).expect("in-memory read cannot fail")
}

/// Feeds `bytes` to one accumulator in the given chunks.
fn analyze_chunked(bytes: &[u8], chunk_size: usize, config: &Config) -> TextStats {
    let mut accum = Accum::new(config);
    for chunk in bytes.chunks(chunk_size.max(1)) {
        accum
            .feed(Cursor::new(chunk.to_vec()))
            .expect("in-memory read cannot fail");
    }
    accum.finish()
}

proptest! {
    /// Splitting the input and feeding the pieces to one accumulator must equal
    /// analyzing it whole. This is what makes multi-file input equivalent to
    /// analyzing the concatenation, including when a word or line straddles a
    /// chunk boundary.
    #[test]
    fn chunking_does_not_change_results(
        bytes in texty(),
        chunk_size in 1usize..64,
        config in config(),
    ) {
        prop_assert_eq!(
            analyze_chunked(&bytes, chunk_size, &config),
            analyze_bytes(&bytes, &config)
        );
    }

    /// The convenience wrapper must agree with driving the accumulator directly.
    #[test]
    fn analyze_matches_accumulator(bytes in any_bytes(), config in config()) {
        prop_assert_eq!(
            analyze_chunked(&bytes, bytes.len().max(1), &config),
            analyze_bytes(&bytes, &config)
        );
    }

    /// Counting is byte-oriented, per the ASCII contract: every byte is a
    /// character, and lines are exactly the newline bytes.
    #[test]
    fn counts_match_the_raw_bytes(bytes in any_bytes(), config in config()) {
        let stats = analyze_bytes(&bytes, &config);
        prop_assert_eq!(stats.char_count as usize, bytes.len());
        prop_assert_eq!(
            stats.line_count as usize,
            bytes.iter().filter(|&&b| b == b'\n').count()
        );
    }

    /// Subsets of the character count stay within it.
    #[test]
    fn counts_are_mutually_consistent(bytes in texty(), config in config()) {
        let stats = analyze_bytes(&bytes, &config);
        prop_assert!(stats.blank_line_count <= stats.line_count);
        prop_assert!(stats.digit_count + stats.punct_count <= stats.char_count);
    }

    /// Word length stats are internally consistent, and their quantiles are
    /// ordered and bracketed by min/max.
    #[test]
    fn word_length_stats_are_ordered(bytes in texty(), config in config()) {
        let stats = analyze_bytes(&bytes, &config);
        let wl = &stats.word_length;
        prop_assert_eq!(wl.count, stats.word_count);

        if wl.count == 0 {
            prop_assert_eq!(wl, &Default::default());
        } else {
            prop_assert!(wl.min <= wl.p25);
            prop_assert!(wl.p25 <= wl.p50);
            prop_assert!(wl.p50 <= wl.p75);
            prop_assert!(wl.p75 <= wl.max);
            // Every word has at least one character, so the total is at least
            // the count, and the mean lies within [min, max].
            prop_assert!(wl.sum >= wl.count);
            let mean = wl.sum as f64 / wl.count as f64;
            prop_assert!(mean >= wl.min as f64);
            prop_assert!(mean <= wl.max as f64);
        }
    }

    /// Rankings respect top_n, are sorted with the documented tie-break, and
    /// cannot claim more occurrences than were counted.
    #[test]
    fn rankings_are_bounded_and_sorted(bytes in texty(), config in config()) {
        let stats = analyze_bytes(&bytes, &config);

        prop_assert!(stats.top_words.len() <= config.top_n);
        prop_assert!(stats.top_chars.len() <= config.top_n);

        for pair in stats.top_words.windows(2) {
            let ordered = pair[0].count > pair[1].count
                || (pair[0].count == pair[1].count && pair[0].word < pair[1].word);
            prop_assert!(ordered, "unsorted: {:?}", pair);
        }
        for pair in stats.top_chars.windows(2) {
            let ordered = pair[0].count > pair[1].count
                || (pair[0].count == pair[1].count && pair[0].ch < pair[1].ch);
            prop_assert!(ordered, "unsorted: {:?}", pair);
        }

        let word_total: u64 = stats.top_words.iter().map(|w| w.count).sum();
        prop_assert!(word_total <= stats.word_count);
        for c in &stats.top_chars {
            prop_assert!(c.count <= stats.char_count);
        }
    }

    /// JSON output must always be well formed. This one earns its keep: the
    /// values go through a hand-rolled formatter that fixes float precision, so
    /// this proves that formatter cannot emit malformed JSON for any input.
    #[test]
    fn json_output_always_parses(bytes in texty(), config in config()) {
        let stats = analyze_bytes(&bytes, &config);
        let json = render_json(&stats);
        let parsed: Result<serde_json::Value, _> = serde_json::from_str(&json);
        prop_assert!(parsed.is_ok(), "invalid JSON: {json}");
    }

    /// Text rendering is total — no input should panic it — and always reports
    /// the totals it was given.
    #[test]
    fn text_output_reports_totals(bytes in texty(), config in config()) {
        let stats = analyze_bytes(&bytes, &config);
        let text = render_text(&stats);
        // Built outside prop_assert!, which stringifies its expression into a
        // format string and would misread these placeholders.
        let words_line = format!("Words:        {}", stats.word_count);
        let chars_line = format!("Characters:   {}", stats.char_count);
        prop_assert!(text.contains(&words_line), "missing {}", words_line);
        prop_assert!(text.contains(&chars_line), "missing {}", chars_line);
    }
}
