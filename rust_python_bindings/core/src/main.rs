//! CLI for `statkit_core`: reads numbers from a file or stdin and prints their
//! summary statistics.
//!
//! `python/statkit/cli.py` is the same program written against the Python
//! bindings, and prints byte-identical output for the same input.

use std::fs;
use std::io::{Read, stdin};
use std::process::ExitCode;

use clap::Parser;

// `StatError` implements `std::error::Error`, so `?` in `run` boxes it for free.
use statkit_core::{Summary, parse_values, summarize, zscores};

#[derive(Parser)]
#[command(
    name = "statkit",
    about = "Summarizes a list of numbers.",
    long_about = "Summarizes a list of numbers read from a file or standard input.\n\n\
                  Numbers may be separated by any mix of whitespace and commas."
)]
struct Cli {
    /// Print each value's z-score instead of the summary table.
    #[arg(long)]
    zscores: bool,

    /// File to read numbers from; standard input is used when omitted.
    file: Option<String>,
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    match run(&cli) {
        Ok(output) => {
            print!("{output}");
            ExitCode::SUCCESS
        }
        Err(err) => {
            eprintln!("statkit: {err}");
            ExitCode::FAILURE
        }
    }
}

/// Does the work and returns what should be printed, so the error paths all
/// funnel through one place in `main`.
fn run(cli: &Cli) -> Result<String, Box<dyn std::error::Error>> {
    let text = read_input(cli.file.as_deref())?;
    let values = parse_values(&text)?;

    if cli.zscores {
        Ok(format_zscores(&zscores(&values)?))
    } else {
        Ok(format_summary(&summarize(&values)?))
    }
}

fn read_input(file: Option<&str>) -> Result<String, std::io::Error> {
    match file {
        Some(path) => fs::read_to_string(path),
        None => {
            let mut text = String::new();
            stdin().read_to_string(&mut text)?;
            Ok(text)
        }
    }
}

/// Keep this format in lock-step with `format_summary` in `python/statkit/cli.py`.
fn format_summary(summary: &Summary) -> String {
    let Summary {
        count,
        mean,
        median,
        min,
        max,
        stddev,
    } = summary;

    format!(
        "{:<8}{count}\n{:<8}{mean:.6}\n{:<8}{median:.6}\n{:<8}{min:.6}\n{:<8}{max:.6}\n{:<8}{stddev:.6}\n",
        "count", "mean", "median", "min", "max", "stddev"
    )
}

/// Keep this format in lock-step with `format_zscores` in `python/statkit/cli.py`.
fn format_zscores(scores: &[f64]) -> String {
    scores.iter().map(|score| format!("{score:.6}\n")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn summary_table_is_label_padded_to_eight_columns() {
        let summary = summarize(&[1.0, 2.0, 3.0]).unwrap();
        assert_eq!(
            format_summary(&summary),
            "count   3\n\
             mean    2.000000\n\
             median  2.000000\n\
             min     1.000000\n\
             max     3.000000\n\
             stddev  1.000000\n"
        );
    }

    #[test]
    fn zscores_print_one_per_line() {
        assert_eq!(
            format_zscores(&[-1.0, 0.0, 1.5]),
            "-1.000000\n0.000000\n1.500000\n"
        );
    }
}
