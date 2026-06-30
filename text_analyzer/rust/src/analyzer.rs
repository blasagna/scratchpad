//! Core analysis: stream a reader byte by byte and compute text statistics.

use std::collections::HashMap;
use std::io::{self, BufRead};

/// Default number of top words/chars to report.
pub const DEFAULT_TOP_N: u64 = 5;
/// Default maximum number of bytes kept per word before truncation.
pub const DEFAULT_MAX_WORD_LEN: u64 = 256;
/// Default initial capacity hint for the word frequency table.
pub const DEFAULT_WORD_TABLE_CAP: u64 = 64;

/// Printable ASCII range excluding space: '!' (33) through '~' (126). These are
/// the only characters considered for the top-chars report.
const PRINTABLE_ASCII_MIN: u8 = b'!';
const PRINTABLE_ASCII_MAX: u8 = b'~';

/// Runtime configuration for [`analyze`].
///
/// Fields must be positive for useful output; zero is degenerate but well
/// defined (it yields empty rankings or keeps no word bytes). The CLI validates
/// user-supplied values, so `analyze` performs no further checking.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    /// Bytes kept per word before truncation.
    pub max_word_len: usize,
    /// Number of top words/chars to report.
    pub top_n: usize,
    /// Reserve hint for the word frequency table.
    pub word_table_cap: usize,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            max_word_len: DEFAULT_MAX_WORD_LEN as usize,
            top_n: DEFAULT_TOP_N as usize,
            word_table_cap: DEFAULT_WORD_TABLE_CAP as usize,
        }
    }
}

/// A word and how many times it occurred.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WordFreq {
    pub word: String,
    pub count: u64,
}

/// A character and how many times it occurred.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharFreq {
    pub ch: char,
    pub count: u64,
}

/// Computed statistics for an analyzed stream.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TextStats {
    pub line_count: u64,
    pub word_count: u64,
    pub char_count: u64,
    /// Sorted descending by count, ties broken ascending by word.
    pub top_words: Vec<WordFreq>,
    /// Sorted descending by count, ties broken ascending by char.
    pub top_chars: Vec<CharFreq>,
}

/// Reads all bytes from `reader` and returns the computed statistics.
///
/// Every byte counts toward `char_count`; each `\n` counts a line. A word is a
/// maximal run of alphabetic ASCII bytes, lowercased and truncated to at most
/// `config.max_word_len - 1` bytes. Top words and characters are sorted by count
/// descending with ties broken ascending. Only printable ASCII (`'!'`..`'~'`)
/// with a nonzero count is eligible for the top-chars report.
pub fn analyze<R: BufRead>(reader: R, config: &Config) -> io::Result<TextStats> {
    let mut stats = TextStats::default();
    let mut char_counts = [0u64; 256];
    let mut words: HashMap<String, u64> = HashMap::with_capacity(config.word_table_cap);

    let mut word = String::new();
    let mut in_word = false;

    for byte in reader.bytes() {
        let b = byte?;
        char_counts[b as usize] += 1;
        stats.char_count += 1;

        if b == b'\n' {
            stats.line_count += 1;
        }

        if b.is_ascii_alphabetic() {
            // Keep at most max_word_len - 1 bytes, matching the C buffer.
            if word.len() + 1 < config.max_word_len {
                word.push(b.to_ascii_lowercase() as char);
            }
            in_word = true;
        } else if in_word {
            flush_word(&mut words, &mut word, &mut stats);
            in_word = false;
        }
    }
    if in_word {
        flush_word(&mut words, &mut word, &mut stats);
    }

    stats.top_words = top_words(words, config.top_n);
    stats.top_chars = top_chars(&char_counts, config.top_n);
    Ok(stats)
}

/// Records the accumulated word, resets the buffer, and bumps the word count.
fn flush_word(words: &mut HashMap<String, u64>, word: &mut String, stats: &mut TextStats) {
    *words.entry(std::mem::take(word)).or_insert(0) += 1;
    stats.word_count += 1;
}

/// Returns the `top_n` most frequent words, ties broken ascending by word.
fn top_words(words: HashMap<String, u64>, top_n: usize) -> Vec<WordFreq> {
    let mut entries: Vec<WordFreq> = words
        .into_iter()
        .map(|(word, count)| WordFreq { word, count })
        .collect();
    entries.sort_by(|a, b| b.count.cmp(&a.count).then_with(|| a.word.cmp(&b.word)));
    entries.truncate(top_n);
    entries
}

/// Returns the `top_n` most frequent printable characters, ties broken ascending.
fn top_chars(char_counts: &[u64; 256], top_n: usize) -> Vec<CharFreq> {
    let mut entries: Vec<CharFreq> = (PRINTABLE_ASCII_MIN..=PRINTABLE_ASCII_MAX)
        .filter(|&b| char_counts[b as usize] > 0)
        .map(|b| CharFreq {
            ch: b as char,
            count: char_counts[b as usize],
        })
        .collect();
    entries.sort_by(|a, b| b.count.cmp(&a.count).then_with(|| a.ch.cmp(&b.ch)));
    entries.truncate(top_n);
    entries
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn analyze_str(text: &str, config: &Config) -> TextStats {
        analyze(Cursor::new(text.as_bytes()), config).expect("in-memory read cannot fail")
    }

    #[test]
    fn empty_file() {
        let stats = analyze_str("", &Config::default());
        assert_eq!(stats.line_count, 0);
        assert_eq!(stats.word_count, 0);
        assert_eq!(stats.char_count, 0);
        assert!(stats.top_words.is_empty());
        assert!(stats.top_chars.is_empty());
    }

    #[test]
    fn single_line() {
        let stats = analyze_str("hello world\n", &Config::default());
        assert_eq!(stats.line_count, 1);
        assert_eq!(stats.word_count, 2);
        assert_eq!(stats.char_count, 12);
    }

    #[test]
    fn multi_line() {
        let stats = analyze_str("one\ntwo\nthree\n", &Config::default());
        assert_eq!(stats.line_count, 3);
        assert_eq!(stats.word_count, 3);
    }

    #[test]
    fn word_frequency() {
        let stats = analyze_str("the cat sat the cat the\n", &Config::default());
        assert!(stats.top_words.len() >= 2);
        assert_eq!(stats.top_words[0].word, "the");
        assert_eq!(stats.top_words[0].count, 3);
        assert_eq!(stats.top_words[1].word, "cat");
        assert_eq!(stats.top_words[1].count, 2);
    }

    #[test]
    fn word_normalization() {
        let stats = analyze_str("The the THE tHe\n", &Config::default());
        assert_eq!(stats.word_count, 4);
        assert!(!stats.top_words.is_empty());
        assert_eq!(stats.top_words[0].word, "the");
        assert_eq!(stats.top_words[0].count, 4);
    }

    #[test]
    fn char_frequency() {
        let stats = analyze_str("aaabbc\n", &Config::default());
        assert!(!stats.top_chars.is_empty());
        assert_eq!(stats.top_chars[0].ch, 'a');
        assert_eq!(stats.top_chars[0].count, 3);
    }

    #[test]
    fn trailing_word_no_newline() {
        let stats = analyze_str("hello world", &Config::default());
        assert_eq!(stats.line_count, 0);
        assert_eq!(stats.word_count, 2);
        assert_eq!(stats.char_count, 11);
    }

    #[test]
    fn config_top_n() {
        let config = Config {
            top_n: 2,
            ..Config::default()
        };
        let stats = analyze_str("a b c d e f\n", &config);
        assert_eq!(stats.top_words.len(), 2);
        assert_eq!(stats.top_chars.len(), 2);
    }

    #[test]
    fn config_max_word_len() {
        let config = Config {
            max_word_len: 3,
            ..Config::default()
        };
        let stats = analyze_str("hello hello hi\n", &config);
        // "hello" truncated to "he" (max_word_len=3 means 2 chars + null).
        assert!(!stats.top_words.is_empty());
        assert_eq!(stats.top_words[0].word, "he");
        assert_eq!(stats.top_words[0].count, 2);
    }

    #[test]
    fn zero_top_n_yields_empty_rankings() {
        let config = Config {
            top_n: 0,
            ..Config::default()
        };
        let stats = analyze_str("the the cat\n", &config);
        assert_eq!(stats.word_count, 3);
        assert!(stats.top_words.is_empty());
        assert!(stats.top_chars.is_empty());
    }
}
