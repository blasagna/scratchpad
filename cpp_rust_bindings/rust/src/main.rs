//! CLI for the Rust `exprkit` bindings.
//!
//! `../cpp/main.cpp` is the same program written directly against the C++
//! library, and prints byte-identical results for the same input. That is not a
//! convention being maintained by hand: the two share their formatter, because
//! `exprkit::format_value` is the C++ one reached through the bindings.
//!
//! Both take a single quoted EXPRESSION and let their argument parser do the
//! parsing -- clap here, CLI11 there -- so what falls outside the guarantee is
//! what those two libraries own, plus one encoding convention:
//!
//! * `--help` text, and the wording and exit code of an argument error. clap
//!   exits 2, CLI11 picks from its own set.
//! * Input that is not valid UTF-8. The bindings take `&str`, so a stray byte
//!   is replaced with U+FFFD and the error message spells it differently --
//!   same failure, same exit code, different character.

use std::io::{self, BufRead, Write};
use std::process::ExitCode;

use clap::{ArgAction, Parser};

use exprkit::{Evaluator, ExprError, format_value};

const EXIT_ERROR: u8 = 1;

#[derive(Parser)]
#[command(
    name = "exprkit",
    about = "Evaluates arithmetic expressions.",
    long_about = "Evaluates arithmetic expressions.\n\n\
                  With an EXPRESSION argument it is evaluated and printed; otherwise \
                  expressions are read one per line from standard input, and variables \
                  assigned with `name = expr` persist from one line to the next. Blank \
                  lines and lines whose first non-blank character is '#' are skipped.\n\n\
                  EXPRESSION is one argument, so quote it: `exprkit '1 + 2'`. An \
                  expression starting with '-' looks like an option, so write \
                  `exprkit -- '-e'`; a leading negative number, as in `exprkit '-2 ^ 2'`, \
                  needs no separator.",
    // Only `--help`, with no `-h` alias, matching ../cpp/main.cpp. With `-h`
    // declared, `exprkit '-h + 1'` matches the help flag and prints help with
    // exit 0 -- a wrong answer that looks like success. Undeclared, the same
    // argument is an ordinary unknown-option rejection.
    disable_help_flag = true
)]
struct Cli {
    /// Print help.
    #[arg(long, action = ArgAction::Help)]
    help: Option<bool>,

    /// After the expression, print every defined name and its value.
    #[arg(long)]
    names: bool,

    /// The expression to evaluate; standard input is read when it is omitted.
    expression: Option<String>,
}

fn main() -> ExitCode {
    let cli = Cli::parse();

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

    if let Some(expression) = &cli.expression {
        let value = evaluator.eval(expression).map_err(|err| err.to_string())?;
        writeln!(out, "{}", format_value(value)).map_err(|err| err.to_string())?;
    } else {
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
            // The first failure stops everything, so a bad line reports once
            // and the rest of the input is not read.
            let value = evaluator
                .eval(&line)
                .map_err(|err: ExprError| format!("line {number}: {err}"))?;
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
}
