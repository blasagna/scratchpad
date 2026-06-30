//! Library for analyzing a text stream and reporting basic statistics:
//! line, word, and character counts plus the most frequent words and characters.

pub mod analyzer;
pub mod report;

pub use analyzer::{
    analyze, CharFreq, Config, TextStats, WordFreq, DEFAULT_MAX_WORD_LEN, DEFAULT_TOP_N,
    DEFAULT_WORD_TABLE_CAP,
};
pub use report::{render_json, render_text};
