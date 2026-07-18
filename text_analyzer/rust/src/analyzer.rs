//! Core analysis: stream one or more readers byte by byte and compute text
//! statistics.

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

/// Buckets in the word length histogram, indexed by length. Lengths at or above
/// the last index are clamped into it, so quantiles (but never the exactly
/// tracked count/sum/min/max) lose resolution for absurdly long words.
const LENGTH_HIST_BUCKETS: usize = DEFAULT_MAX_WORD_LEN as usize + 1;

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

/// Distribution of word lengths, measured in alphabetic characters.
///
/// Lengths are the true lengths of each word in the input, unaffected by
/// [`Config::max_word_len`] truncation of the stored spelling. The mean is not
/// stored: derive it as `sum / count`, guarding against `count == 0`. All fields
/// are zero when no words were seen.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct WordLengthStats {
    /// Number of words measured; equals [`TextStats::word_count`].
    pub count: u64,
    /// Total of all word lengths.
    pub sum: u64,
    pub min: u64,
    pub max: u64,
    /// 25th, 50th, and 75th percentile lengths by nearest rank.
    pub p25: u64,
    pub p50: u64,
    pub p75: u64,
}

/// Computed statistics for an analyzed stream.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TextStats {
    pub line_count: u64,
    /// Lines containing no non-whitespace characters.
    pub blank_line_count: u64,
    pub word_count: u64,
    pub char_count: u64,
    pub digit_count: u64,
    pub punct_count: u64,
    pub word_length: WordLengthStats,
    /// Sorted descending by count, ties broken ascending by word.
    pub top_words: Vec<WordFreq>,
    /// Sorted descending by count, ties broken ascending by char.
    pub top_chars: Vec<CharFreq>,
}

/// Accumulates statistics across one or more readers.
///
/// Feed any number of readers with [`Accum::feed`], then call [`Accum::finish`]
/// to rank and return the totals. Scan state persists between feeds, so feeding
/// two readers is equivalent to feeding their concatenation — a word or line
/// split across the boundary is counted once, not twice.
pub struct Accum {
    config: Config,
    stats: TextStats,
    char_counts: [u64; 256],
    words: HashMap<String, u64>,
    length_hist: Vec<u64>,
    length_sum: u64,
    length_min: u64,
    length_max: u64,
    /// Spelling of the word in progress, truncated to `config.max_word_len`.
    word: String,
    /// True length of the word in progress, ignoring truncation.
    cur_word_len: u64,
    in_word: bool,
    line_has_content: bool,
}

impl Accum {
    /// Creates an accumulator that will apply `config` when finishing.
    pub fn new(config: &Config) -> Self {
        Accum {
            config: config.clone(),
            stats: TextStats::default(),
            char_counts: [0u64; 256],
            words: HashMap::with_capacity(config.word_table_cap),
            length_hist: vec![0u64; LENGTH_HIST_BUCKETS],
            length_sum: 0,
            length_min: 0,
            length_max: 0,
            word: String::new(),
            cur_word_len: 0,
            in_word: false,
            line_has_content: false,
        }
    }

    /// Reads all bytes from `reader` into the accumulator.
    ///
    /// Every byte counts toward `char_count`; each `\n` counts a line, and a
    /// line with no non-whitespace byte counts as blank. A word is a maximal run
    /// of alphabetic ASCII bytes, lowercased and truncated to at most
    /// `config.max_word_len - 1` bytes for storage.
    pub fn feed<R: BufRead>(&mut self, reader: R) -> io::Result<()> {
        for byte in reader.bytes() {
            let b = byte?;
            self.char_counts[b as usize] += 1;
            self.stats.char_count += 1;

            if b == b'\n' {
                self.stats.line_count += 1;
                if !self.line_has_content {
                    self.stats.blank_line_count += 1;
                }
                self.line_has_content = false;
            } else if !b.is_ascii_whitespace() {
                self.line_has_content = true;
            }

            if b.is_ascii_alphabetic() {
                // Keep at most max_word_len - 1 bytes, matching the C buffer,
                // but measure the untruncated length.
                if self.word.len() + 1 < self.config.max_word_len {
                    self.word.push(b.to_ascii_lowercase() as char);
                }
                self.cur_word_len += 1;
                self.in_word = true;
            } else if self.in_word {
                self.flush_word();
            }
        }
        Ok(())
    }

    /// Flushes any trailing word, then ranks and returns the totals.
    pub fn finish(mut self) -> TextStats {
        if self.in_word {
            self.flush_word();
        }

        for (b, &count) in self.char_counts.iter().enumerate() {
            let b = b as u8;
            if b.is_ascii_digit() {
                self.stats.digit_count += count;
            } else if b.is_ascii_punctuation() {
                self.stats.punct_count += count;
            }
        }

        self.stats.word_length = WordLengthStats {
            count: self.stats.word_count,
            sum: self.length_sum,
            min: self.length_min,
            max: self.length_max,
            p25: quantile(&self.length_hist, self.stats.word_count, 25),
            p50: quantile(&self.length_hist, self.stats.word_count, 50),
            p75: quantile(&self.length_hist, self.stats.word_count, 75),
        };

        self.stats.top_words = top_words(self.words, self.config.top_n);
        self.stats.top_chars = top_chars(&self.char_counts, self.config.top_n);
        self.stats
    }

    /// Records the accumulated word and its length, then resets word state.
    fn flush_word(&mut self) {
        *self.words.entry(std::mem::take(&mut self.word)).or_insert(0) += 1;
        self.stats.word_count += 1;

        let len = self.cur_word_len;
        self.length_sum += len;
        if self.length_max == 0 || len > self.length_max {
            self.length_max = len;
        }
        if self.length_min == 0 || len < self.length_min {
            self.length_min = len;
        }
        let bucket = (len as usize).min(LENGTH_HIST_BUCKETS - 1);
        self.length_hist[bucket] += 1;

        self.cur_word_len = 0;
        self.in_word = false;
    }
}

/// Reads all bytes from a single `reader` and returns the computed statistics.
///
/// Equivalent to one [`Accum::feed`] followed by [`Accum::finish`].
pub fn analyze<R: BufRead>(reader: R, config: &Config) -> io::Result<TextStats> {
    let mut accum = Accum::new(config);
    accum.feed(reader)?;
    Ok(accum.finish())
}

/// Returns the length at the `pct`th percentile by nearest rank, or 0 when there
/// are no words. Integer arithmetic throughout so the three ports agree exactly.
fn quantile(hist: &[u64], count: u64, pct: u64) -> u64 {
    if count == 0 {
        return 0;
    }
    // 1-based rank of the target element: ceil(pct/100 * count), at least 1.
    let rank = (pct * count).div_ceil(100).max(1);
    let mut cumulative = 0u64;
    for (len, &n) in hist.iter().enumerate() {
        cumulative += n;
        if cumulative >= rank {
            return len as u64;
        }
    }
    0
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
        assert_eq!(stats.word_length, WordLengthStats::default());
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

    #[test]
    fn blank_lines() {
        // Four terminated lines; the empty one and the whitespace-only one are
        // both blank.
        let stats = analyze_str("a\n\n  \nb\n", &Config::default());
        assert_eq!(stats.line_count, 4);
        assert_eq!(stats.blank_line_count, 2);
    }

    #[test]
    fn unterminated_final_line_is_not_counted() {
        // Consistent with line_count: a final line without '\n' is not a line,
        // so it is not a blank line either.
        let stats = analyze_str("a\n   ", &Config::default());
        assert_eq!(stats.line_count, 1);
        assert_eq!(stats.blank_line_count, 0);
    }

    #[test]
    fn digits_and_punctuation() {
        let stats = analyze_str("ab 12, c!\n", &Config::default());
        assert_eq!(stats.digit_count, 2);
        assert_eq!(stats.punct_count, 2);
    }

    #[test]
    fn word_length_stats() {
        // Lengths 1, 2, 3, 4. Nearest rank picks element ceil(p/100 * 4):
        // p25 -> 1st (1), p50 -> 2nd (2), p75 -> 3rd (3).
        let stats = analyze_str("a bb ccc dddd\n", &Config::default());
        assert_eq!(
            stats.word_length,
            WordLengthStats {
                count: 4,
                sum: 10,
                min: 1,
                max: 4,
                p25: 1,
                p50: 2,
                p75: 3,
            }
        );
    }

    #[test]
    fn word_length_ignores_truncation() {
        let config = Config {
            max_word_len: 3,
            ..Config::default()
        };
        let stats = analyze_str("hello\n", &config);
        // Stored spelling is truncated, but the measured length is the real one.
        assert_eq!(stats.top_words[0].word, "he");
        assert_eq!(stats.word_length.max, 5);
        assert_eq!(stats.word_length.sum, 5);
    }

    #[test]
    fn multi_feed_aggregates() {
        let config = Config::default();
        let mut accum = Accum::new(&config);
        accum
            .feed(Cursor::new(b"the cat\n".as_slice()))
            .expect("in-memory read cannot fail");
        accum
            .feed(Cursor::new(b"the dog\n".as_slice()))
            .expect("in-memory read cannot fail");
        let stats = accum.finish();

        assert_eq!(stats, analyze_str("the cat\nthe dog\n", &config));
        assert_eq!(stats.line_count, 2);
        assert_eq!(stats.word_count, 4);
        assert_eq!(stats.top_words[0].word, "the");
        assert_eq!(stats.top_words[0].count, 2);
    }

    #[test]
    fn word_split_across_feeds() {
        let config = Config::default();
        let mut accum = Accum::new(&config);
        accum
            .feed(Cursor::new(b"hel".as_slice()))
            .expect("in-memory read cannot fail");
        accum
            .feed(Cursor::new(b"lo\n".as_slice()))
            .expect("in-memory read cannot fail");
        let stats = accum.finish();

        assert_eq!(stats.word_count, 1);
        assert_eq!(stats.top_words[0].word, "hello");
        assert_eq!(stats.word_length.max, 5);
    }
}
