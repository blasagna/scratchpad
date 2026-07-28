//! CLI for the Rust `exprkit` bindings.
//!
//! `../cpp/main.cpp` is the same program written directly against the C++
//! library, and prints byte-identical output for the same input. That is not a
//! convention being maintained by hand: the two share their formatter, because
//! `exprkit::format_value` is the C++ one reached through the bindings.
//!
//! Three things are outside that guarantee, all of them argument-parsing or
//! encoding conventions rather than exprkit behavior:
//!
//! * `--help` text. clap writes this one; the C++ hand-rolls its own.
//! * A bare `--`. clap takes it as an end-of-options separator, the C++ CLI
//!   reports it as an unknown option.
//! * Input that is not valid UTF-8. The bindings take `&str`, so a stray byte
//!   is replaced with U+FFFD and the error message spells it differently --
//!   same failure, same exit code, different character.

use std::io::{self, BufRead, Write};
use std::process::ExitCode;

use clap::Parser;

use exprkit::{Evaluator, ExprError, format_value};

const EXIT_ERROR: u8 = 1;
const EXIT_USAGE: u8 = 2;

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
    ///
    /// `allow_hyphen_values` is required, not cosmetic: without it clap reads
    /// `-2 ^ 2` as an unknown flag and exits 2, while the C++ CLI evaluates it
    /// to -4. A leading minus is ordinary arithmetic, so it cannot be a usage
    /// error here.
    ///
    /// It costs two things, both taken back by `split_options` below: clap
    /// stops rejecting unknown options, and it stops recognizing *any* option
    /// once the first positional appears, so `exprkit '1+1' --names` would
    /// otherwise treat `--names` as an expression.
    #[arg(allow_hyphen_values = true)]
    expressions: Vec<String>,
}

/// What `split_options` found in the leftover positional arguments.
enum Options {
    /// The real expressions, with any late `--names` folded into the flag.
    Expressions(Vec<String>),
    /// A late `--help`; the C++ CLI prints usage and exits 0 wherever it sits.
    Help,
    /// An unrecognized `--` argument, to report as a usage error.
    Unknown(String),
}

fn main() -> ExitCode {
    let mut cli = Cli::parse();

    // The C++ CLI scans every argument for options regardless of position,
    // because it hand-rolls its parser. clap with allow_hyphen_values does not,
    // so this pass restores C-style semantics over what clap left behind.
    match split_options(std::mem::take(&mut cli.expressions), &mut cli.names) {
        Options::Expressions(expressions) => cli.expressions = expressions,
        Options::Help => {
            // Exits 0, as `clap` does for an early --help and the C++ does for
            // a late one. The text itself is clap's and differs from the C++
            // usage block -- the one accepted difference between the two CLIs.
            let mut command = <Cli as clap::CommandFactory>::command();
            if command.print_long_help().is_err() {
                return ExitCode::from(EXIT_ERROR);
            }
            return ExitCode::SUCCESS;
        }
        Options::Unknown(option) => {
            // Kept byte-identical with the same path in ../cpp/main.cpp.
            eprintln!("exprkit: unknown option: {option}");
            eprintln!("Try 'exprkit --help' for more information.");
            return ExitCode::from(EXIT_USAGE);
        }
    }

    match run(&cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("exprkit: {message}");
            ExitCode::from(EXIT_ERROR)
        }
    }
}

/// Pulls options out of the positional arguments clap handed back.
///
/// A single `-` is deliberately not an option: `-2 ^ 2` is an expression, and
/// so is `-x`. Only `--` prefixes are considered, matching `../cpp/main.cpp`.
fn split_options(arguments: Vec<String>, names: &mut bool) -> Options {
    let mut expressions = Vec::with_capacity(arguments.len());

    for argument in arguments {
        match argument.as_str() {
            "--names" => *names = true,
            "--help" => return Options::Help,
            argument if argument.starts_with("--") => {
                return Options::Unknown(argument.to_string());
            }
            _ => expressions.push(argument),
        }
    }

    Options::Expressions(expressions)
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
        let mut input = io::stdin().lock();
        let mut number = 0;
        // Deliberately not `BufRead::lines()`: that validates UTF-8 over the
        // whole stream and fails the entire run on one stray byte, where the
        // C++ CLI -- which reads bytes -- carries on. A Latin-1 character in a
        // comment was enough to break parity. See read_line below.
        while let Some(line) = read_line(&mut input).map_err(|err| err.to_string())? {
            number += 1;
            if is_blank_or_comment(&line) {
                continue;
            }
            let value = evaluator
                .eval(&line)
                .map_err(|err: ExprError| format!("line {number}: {err}"))?;
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

/// Reads one newline-terminated line, or `None` at end of input.
///
/// Byte-oriented like C++'s `std::getline`, with invalid UTF-8 replaced rather
/// than rejected, so a bad byte costs at most a mangled character instead of
/// the whole run. The replacement is not a parity hole in practice: a non-UTF-8
/// byte inside an actual expression is an error either way, and the message
/// already comes back from C++ with U+FFFD substituted (see `lib.rs`).
fn read_line(input: &mut impl BufRead) -> io::Result<Option<String>> {
    let mut bytes = Vec::new();
    if input.read_until(b'\n', &mut bytes)? == 0 {
        return Ok(None);
    }
    // getline strips the newline and leaves any \r; the tokenizer and
    // is_blank_or_comment both treat \r as whitespace, so this matches.
    if bytes.last() == Some(&b'\n') {
        bytes.pop();
    }
    Ok(Some(String::from_utf8_lossy(&bytes).into_owned()))
}

/// Reports whether a line carries no expression.
///
/// The whitespace set is exactly the one in `../cpp/main.cpp`, and is written
/// out rather than using `trim_start`: Rust trims every Unicode `White_Space`
/// character, so a form feed or a non-breaking space would make this skip a
/// line the C++ CLI tries to evaluate.
fn is_blank_or_comment(line: &str) -> bool {
    for c in line.chars() {
        if c == ' ' || c == '\t' || c == '\r' {
            continue;
        }
        return c == '#';
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blank_and_comment_lines_are_skipped() {
        assert!(is_blank_or_comment(""));
        assert!(is_blank_or_comment("   \t "));
        assert!(is_blank_or_comment("\r"));
        assert!(is_blank_or_comment("# a note"));
        assert!(is_blank_or_comment("   # indented"));
        assert!(!is_blank_or_comment("1 + 1"));
        assert!(!is_blank_or_comment("  x = 2  "));
    }

    #[test]
    fn only_the_cpp_whitespace_set_counts_as_blank() {
        // `trim_start` would treat all of these as leading whitespace and skip
        // the line; the C++ CLI does not, and hands them to the tokenizer,
        // which rejects them. Parity means agreeing on the failures too.
        assert!(!is_blank_or_comment("\u{0c}# form feed"));
        assert!(!is_blank_or_comment("\u{a0}")); // non-breaking space
        assert!(!is_blank_or_comment("\u{2028}# line separator"));
        assert!(!is_blank_or_comment("\u{3000}# ideographic space"));
    }

    #[test]
    fn read_line_survives_invalid_utf8() {
        // One bad byte used to fail the entire run via BufRead::lines().
        let mut input = &b"# caf\xe9\n3+3\n"[..];
        assert_eq!(read_line(&mut input).unwrap().unwrap(), "# caf\u{fffd}");
        assert_eq!(read_line(&mut input).unwrap().unwrap(), "3+3");
        assert_eq!(read_line(&mut input).unwrap(), None);
    }

    #[test]
    fn read_line_strips_only_the_newline() {
        let mut input = &b"a\r\nb\nc"[..];
        assert_eq!(read_line(&mut input).unwrap().unwrap(), "a\r");
        assert_eq!(read_line(&mut input).unwrap().unwrap(), "b");
        assert_eq!(read_line(&mut input).unwrap().unwrap(), "c"); // no trailing \n
        assert_eq!(read_line(&mut input).unwrap(), None);
    }

    /// Runs `split_options` over string literals, returning the outcome and the
    /// resulting `--names` state.
    fn split(values: &[&str]) -> (Options, bool) {
        let mut names = false;
        let arguments = values.iter().map(|s| s.to_string()).collect();
        (split_options(arguments, &mut names), names)
    }

    #[test]
    fn expressions_beginning_with_a_hyphen_are_not_options() {
        // The whole reason allow_hyphen_values is on: `-2 ^ 2` is the C++
        // suite's own example of unary-minus precedence, and must evaluate.
        let (options, names) = split(&["-2 ^ 2", "-x", "1+1"]);
        assert!(matches!(options, Options::Expressions(e) if e == ["-2 ^ 2", "-x", "1+1"]));
        assert!(!names);
    }

    #[test]
    fn a_late_names_flag_is_recognized() {
        // clap stops parsing options after the first positional, so without
        // split_options this `--names` would be evaluated as an expression.
        let (options, names) = split(&["r = 3", "--names"]);
        assert!(matches!(options, Options::Expressions(e) if e == ["r = 3"]));
        assert!(names);
    }

    #[test]
    fn a_late_help_flag_is_recognized() {
        assert!(matches!(split(&["1+1", "--help"]).0, Options::Help));
    }

    #[test]
    fn unknown_double_hyphen_arguments_are_usage_errors() {
        assert!(matches!(split(&["--bogus"]).0, Options::Unknown(o) if o == "--bogus"));
        assert!(matches!(split(&["1+1", "--x"]).0, Options::Unknown(o) if o == "--x"));
    }

    #[test]
    fn no_arguments_yields_no_expressions() {
        let (options, names) = split(&[]);
        assert!(matches!(options, Options::Expressions(e) if e.is_empty()));
        assert!(!names);
    }
}
