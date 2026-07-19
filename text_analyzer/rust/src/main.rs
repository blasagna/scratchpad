//! CLI for the text analyzer: reads files or stdin and prints statistics to
//! stdout.

use std::fs::File;
use std::io::{self, BufReader};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::Parser;

use text_analyzer::{
    Accum, Config, DEFAULT_MAX_WORD_LEN, DEFAULT_TOP_N, DEFAULT_WORD_TABLE_CAP, render_json,
    render_text,
};

/// Label used for stdin in error messages.
const STDIN_LABEL: &str = "<stdin>";

#[derive(Parser)]
#[command(
    name = "text_analyzer",
    about = "Reads text files and prints line, word, and character statistics.",
    long_about = "Reads text files and prints statistics:\n  \
                  - total line, blank line, word, character, digit, and \
                  punctuation counts\n  \
                  - word length distribution (mean, min, max, quartiles)\n  \
                  - top N most frequent words (case-insensitive)\n  \
                  - top N most frequent non-space characters\n\n\
                  Multiple files are analyzed as a single concatenated stream. \
                  Reads stdin when no file is given or when the file is '-'.\n\n\
                  Input is treated as ASCII bytes: characters are counted as \
                  bytes, not Unicode codepoints, and any non-ASCII byte \
                  separates words."
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

    /// Input files to analyze; '-' means stdin. Defaults to stdin.
    #[arg(num_args = 0..)]
    files: Vec<PathBuf>,
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    let config = Config {
        max_word_len: cli.max_word_len as usize,
        top_n: cli.top_n as usize,
        word_table_cap: cli.word_table_cap as usize,
    };

    let mut accum = Accum::new(&config);
    let result = if cli.files.is_empty() {
        feed_stdin(&mut accum)
    } else {
        cli.files.iter().try_for_each(|path| feed_path(&mut accum, path))
    };
    if let Err((label, err)) = result {
        eprintln!("{label}: {err}");
        return ExitCode::FAILURE;
    }
    let stats = accum.finish();

    if cli.json {
        println!("{}", render_json(&stats));
    } else {
        print!("{}", render_text(&stats));
    }

    ExitCode::SUCCESS
}

/// Feeds one path into `accum`, treating `-` as stdin. Errors carry the label to
/// report them against.
fn feed_path(accum: &mut Accum, path: &Path) -> Result<(), (String, io::Error)> {
    if path.as_os_str() == "-" {
        return feed_stdin(accum);
    }
    let label = || path.display().to_string();
    let file = File::open(path).map_err(|err| (label(), err))?;
    accum
        .feed(BufReader::new(file))
        .map_err(|err| (label(), err))
}

/// Feeds stdin into `accum`.
fn feed_stdin(accum: &mut Accum) -> Result<(), (String, io::Error)> {
    accum
        .feed(io::stdin().lock())
        .map_err(|err| (STDIN_LABEL.to_string(), err))
}
