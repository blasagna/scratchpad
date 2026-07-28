//! CLI for the Rust `exprkit` bindings.
//!
//! `../cpp/main.cpp` is the same program written directly against the C++
//! library, and prints byte-identical output for the same input. That is not a
//! convention being maintained by hand: the two share their formatter, because
//! `exprkit::format_value` is the C++ one reached through the bindings.

use std::io::{self, BufRead, Write};
use std::process::ExitCode;

use clap::Parser;

use exprkit::{Evaluator, ExprError, format_value};

const EXIT_ERROR: u8 = 1;

#[derive(Parser)]
#[command(
    name = "exprkit",
    about = "Evaluates arithmetic expressions.",
    long_about = "Evaluates arithmetic expressions.\n\n\
                  With one or more EXPRESSION arguments, each is evaluated in turn; \
                  otherwise expressions are read one per line from standard input. \
                  Variables assigned with `name = expr` persist across expressions. \
                  Blank lines and lines whose first non-blank character is '#' are skipped."
)]
struct Cli {
    /// After the last expression, print every defined name and its value.
    #[arg(long)]
    names: bool,

    /// Expressions to evaluate; standard input is read when none are given.
    expressions: Vec<String>,
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    // clap already exits 2 on a usage error, matching the C++ CLI.
    match run(&cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("exprkit: {message}");
            ExitCode::from(EXIT_ERROR)
        }
    }
}

/// Does the work, returning the message to print on failure.
///
/// The error is a `String` rather than an [`ExprError`] because the stdin path
/// prefixes it with a line number, which the argument path has nothing to
/// number.
fn run(cli: &Cli) -> Result<(), String> {
    let mut evaluator = Evaluator::new();
    let stdout = io::stdout();
    let mut out = stdout.lock();

    if cli.expressions.is_empty() {
        for (index, line) in io::stdin().lock().lines().enumerate() {
            let line = line.map_err(|err| err.to_string())?;
            if is_blank_or_comment(&line) {
                continue;
            }
            let value = evaluator
                .eval(&line)
                .map_err(|err: ExprError| format!("line {}: {err}", index + 1))?;
            writeln!(out, "{}", format_value(value)).map_err(|err| err.to_string())?;
        }
    } else {
        // The first failure stops everything, so `exprkit 'x = 1/0' 'x'`
        // reports once rather than twice.
        for expression in &cli.expressions {
            let value = evaluator.eval(expression).map_err(|err| err.to_string())?;
            writeln!(out, "{}", format_value(value)).map_err(|err| err.to_string())?;
        }
    }

    if cli.names {
        for name in evaluator.names() {
            // `names` only ever returns defined names, so `get` cannot fail.
            let value = evaluator.get(&name).map_err(|err| err.to_string())?;
            writeln!(out, "{name} = {}", format_value(value)).map_err(|err| err.to_string())?;
        }
    }

    Ok(())
}

/// Reports whether a line carries no expression.
fn is_blank_or_comment(line: &str) -> bool {
    match line.trim_start().chars().next() {
        Some(c) => c == '#',
        None => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blank_and_comment_lines_are_skipped() {
        assert!(is_blank_or_comment(""));
        assert!(is_blank_or_comment("   \t "));
        assert!(is_blank_or_comment("# a note"));
        assert!(is_blank_or_comment("   # indented"));
        assert!(!is_blank_or_comment("1 + 1"));
        assert!(!is_blank_or_comment("  x = 2  "));
    }
}
