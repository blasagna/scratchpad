//! Library for analyzing text streams and reporting basic statistics: line,
//! blank line, word, character, digit, and punctuation counts, the word length
//! distribution, and the most frequent words and characters.

pub mod analyzer;
pub mod report;

pub use analyzer::{
    Accum, CharFreq, Config, DEFAULT_MAX_WORD_LEN, DEFAULT_TOP_N, DEFAULT_WORD_TABLE_CAP,
    TextStats, WordFreq, WordLengthStats, analyze,
};
pub use report::{render_json, render_text};
