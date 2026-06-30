//! CLI for the text analyzer: reads a file and prints statistics to stdout.

use std::fs::File;
use std::io::BufReader;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;

use text_analyzer::{
    Config, DEFAULT_MAX_WORD_LEN, DEFAULT_TOP_N, DEFAULT_WORD_TABLE_CAP, analyze, render_json,
    render_text,
};

#[derive(Parser)]
#[command(
    name = "text_analyzer",
    about = "Reads a text file and prints line, word, and character statistics.",
    long_about = "Reads a text file and prints statistics:\n  \
                  - total line, word, and character counts\n  \
                  - top N most frequent words (case-insensitive)\n  \
                  - top N most frequent non-space characters"
)]
struct Cli {
    /// Number of top words/chars to report.
    #[arg(long, default_value_t = DEFAULT_TOP_N, value_parser = clap::value_parser!(u64).range(1..))]
    top_n: u64,

    /// Max characters per word before truncation.
    #[arg(long, default_value_t = DEFAULT_MAX_WORD_LEN, value_parser = clap::value_parser!(u64).range(1..))]
    max_word_len: u64,

    /// Initial word frequency table capacity.
    #[arg(long, default_value_t = DEFAULT_WORD_TABLE_CAP, value_parser = clap::value_parser!(u64).range(1..))]
    word_table_cap: u64,

    /// Print the summary as JSON instead of text.
    #[arg(long)]
    json: bool,

    /// Input file to analyze.
    file: PathBuf,
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    let config = Config {
        max_word_len: cli.max_word_len as usize,
        top_n: cli.top_n as usize,
        word_table_cap: cli.word_table_cap as usize,
    };

    let file = match File::open(&cli.file) {
        Ok(f) => f,
        Err(err) => {
            eprintln!("{}: {}", cli.file.display(), err);
            return ExitCode::FAILURE;
        }
    };

    let stats = match analyze(BufReader::new(file), &config) {
        Ok(stats) => stats,
        Err(err) => {
            eprintln!("{}: {}", cli.file.display(), err);
            return ExitCode::FAILURE;
        }
    };

    if cli.json {
        println!("{}", render_json(&stats));
    } else {
        print!("{}", render_text(&stats));
    }

    ExitCode::SUCCESS
}
