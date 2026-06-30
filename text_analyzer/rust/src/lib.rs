//! Library for analyzing a text stream and reporting basic statistics:
//! line, word, and character counts plus the most frequent words and characters.

pub mod analyzer;
pub mod report;

pub use analyzer::{
    CharFreq, Config, DEFAULT_MAX_WORD_LEN, DEFAULT_TOP_N, DEFAULT_WORD_TABLE_CAP, TextStats,
    WordFreq, analyze,
};
pub use report::{render_json, render_text};
