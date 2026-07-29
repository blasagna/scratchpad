//! The `lrukit` command-line driver.
//!
//! The only entry point the library has. `../cpp` ships no `main`, so this is
//! not "the Rust CLI" as distinct from a C++ one -- there is nothing to stay in
//! sync with, and no output parity to defend.
//!
//! It reads a small command language, either from the arguments or one command
//! per line on stdin:
//!
//! ```text
//!   put KEY VALUE     store, evicting the least recently used entry if full
//!   get KEY           print the value, or "(miss)"
//!   del KEY           remove, printing whether it was there
//!   has KEY           report presence without disturbing the order
//!   keys              print the keys, most recently used first
//!   len               print the number of entries
//!   stats             print the counters
//! ```

use std::io::{self, BufRead, Write};
use std::process::ExitCode;

use clap::Parser;
use lrukit::Cache;

/// Exit codes, matching the rest of this repo: 0 success, 1 a failure the
/// program was asked to attempt, 2 a request that never made sense.
const EXIT_ERROR: u8 = 1;
const EXIT_USAGE: u8 = 2;

#[derive(Parser)]
#[command(
    name = "lrukit",
    about = "Drive an LRU cache from the command line.",
    long_about = "Drive an LRU cache from the command line.\n\n\
                  Runs each COMMAND against one cache, in order, stopping at the \
                  first failure. With no COMMAND, reads one command per line from \
                  stdin; blank lines and lines starting with '#' are skipped.\n\n\
                  Commands:\n  \
                  put KEY VALUE     store, evicting the least recently used entry if full\n  \
                  get KEY           print the value, or \"(miss)\"\n  \
                  del KEY           remove, printing whether it was there\n  \
                  has KEY           report presence without disturbing the order\n  \
                  keys              print the keys, most recently used first\n  \
                  len               print the number of entries\n  \
                  stats             print the counters"
)]
struct Args {
    /// Maximum number of entries the cache holds.
    #[arg(short, long, default_value_t = 8)]
    capacity: usize,

    /// Commands to run; if none are given, they are read from stdin.
    #[arg(value_name = "COMMAND")]
    commands: Vec<String>,
}

/// A parsed command, with its arguments already checked.
///
/// Parsing is separated from running so that a malformed command is a usage
/// error before the cache is touched, and so the parser can be tested without
/// one.
#[derive(Debug, PartialEq, Eq)]
enum Command {
    Put { key: String, value: String },
    Get(String),
    Del(String),
    Has(String),
    Keys,
    Len,
    Stats,
}

/// Splits a command line into a verb and its arguments.
///
/// Whitespace-separated, with the last argument of `put` taking the rest of the
/// line verbatim -- so `put greeting hello there` stores "hello there" and a
/// value with spaces needs no quoting the shell would have eaten first.
fn parse_command(line: &str) -> std::result::Result<Command, String> {
    let trimmed = line.trim();
    let (verb, rest) = match trimmed.split_once(char::is_whitespace) {
        Some((verb, rest)) => (verb, rest.trim()),
        None => (trimmed, ""),
    };

    match verb {
        "" => Err("empty command".to_string()),
        "put" => {
            // Only the key is a token; the value is whatever is left, so
            // `put greeting hello there` stores "hello there" without the
            // caller having to quote past the shell.
            let (key, value) = rest
                .split_once(char::is_whitespace)
                .map_or((rest, ""), |(key, value)| (key, value.trim_start()));
            if key.is_empty() || value.is_empty() {
                return Err("put needs a key and a value".to_string());
            }
            Ok(Command::Put {
                key: key.to_string(),
                value: value.to_string(),
            })
        }
        "get" | "del" | "has" => {
            if rest.is_empty() {
                return Err(format!("{verb} needs a key"));
            }
            if rest.split_whitespace().count() > 1 {
                return Err(format!("{verb} takes exactly one key"));
            }
            let key = rest.to_string();
            match verb {
                "get" => Ok(Command::Get(key)),
                "del" => Ok(Command::Del(key)),
                _ => Ok(Command::Has(key)),
            }
        }
        "keys" | "len" | "stats" => {
            if !rest.is_empty() {
                return Err(format!("{verb} takes no arguments"));
            }
            match verb {
                "keys" => Ok(Command::Keys),
                "len" => Ok(Command::Len),
                _ => Ok(Command::Stats),
            }
        }
        other => Err(format!("unknown command: {other}")),
    }
}

/// Runs one command, writing whatever it has to say to `out`.
fn run(cache: &mut Cache, command: &Command, out: &mut impl Write) -> io::Result<()> {
    match command {
        Command::Put { key, value } => match cache.put(key, value) {
            Ok(true) => writeln!(out, "stored {key}"),
            Ok(false) => writeln!(out, "updated {key}"),
            Err(err) => Err(io::Error::other(err.to_string())),
        },
        Command::Get(key) => match cache.get(key) {
            // A miss is the ordinary business of a cache, not a failure: it
            // prints and the run continues.
            Ok(Some(value)) => writeln!(out, "{value}"),
            Ok(None) => writeln!(out, "(miss)"),
            Err(err) => Err(io::Error::other(err.to_string())),
        },
        Command::Del(key) => match cache.remove(key) {
            Ok(true) => writeln!(out, "removed {key}"),
            Ok(false) => writeln!(out, "(absent)"),
            Err(err) => Err(io::Error::other(err.to_string())),
        },
        Command::Has(key) => match cache.contains(key) {
            Ok(present) => writeln!(out, "{present}"),
            Err(err) => Err(io::Error::other(err.to_string())),
        },
        Command::Keys => {
            for key in cache.keys() {
                writeln!(out, "{key}")?;
            }
            Ok(())
        }
        Command::Len => writeln!(out, "{}", cache.len()),
        // The text comes from the C++ formatter through Display; there is no
        // second rendering of these numbers anywhere.
        Command::Stats => writeln!(out, "{}", cache.stats()),
    }
}

/// True for lines a script may contain that are not commands.
fn is_blank_or_comment(line: &str) -> bool {
    let trimmed = line.trim_start();
    trimmed.is_empty() || trimmed.starts_with('#')
}

fn main() -> ExitCode {
    let args = Args::parse();

    let mut cache = match Cache::new(args.capacity) {
        Ok(cache) => cache,
        Err(err) => {
            eprintln!("lrukit: {err}");
            return ExitCode::from(EXIT_ERROR);
        }
    };

    let stdout = io::stdout();
    let mut out = stdout.lock();

    if args.commands.is_empty() {
        return run_stdin(&mut cache, &mut out);
    }

    for line in &args.commands {
        let command = match parse_command(line) {
            Ok(command) => command,
            Err(message) => {
                eprintln!("lrukit: {message}");
                return ExitCode::from(EXIT_USAGE);
            }
        };
        if let Err(err) = run(&mut cache, &command, &mut out) {
            eprintln!("lrukit: {err}");
            return ExitCode::from(EXIT_ERROR);
        }
    }
    ExitCode::SUCCESS
}

/// Reads commands from stdin, one per line, until EOF or the first failure.
fn run_stdin(cache: &mut Cache, out: &mut impl Write) -> ExitCode {
    let stdin = io::stdin();
    let mut reader = stdin.lock();
    let mut buffer = Vec::new();
    let mut number = 0usize;

    loop {
        buffer.clear();
        match reader.read_until(b'\n', &mut buffer) {
            Ok(0) => return ExitCode::SUCCESS,
            Ok(_) => {}
            Err(err) => {
                eprintln!("lrukit: {err}");
                return ExitCode::from(EXIT_ERROR);
            }
        }
        number += 1;

        // read_until plus from_utf8_lossy rather than BufRead::lines(): a line
        // with one stray byte becomes U+FFFD and fails as an unknown command,
        // where lines() would abandon the whole run on it.
        let line = String::from_utf8_lossy(&buffer);
        let line = line.trim_end_matches(['\n', '\r']);
        if is_blank_or_comment(line) {
            continue;
        }

        match parse_command(line) {
            Ok(command) => {
                if let Err(err) = run(cache, &command, out) {
                    eprintln!("lrukit: line {number}: {err}");
                    return ExitCode::from(EXIT_ERROR);
                }
            }
            Err(message) => {
                eprintln!("lrukit: line {number}: {message}");
                return ExitCode::from(EXIT_USAGE);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn output_of(capacity: usize, lines: &[&str]) -> String {
        let mut cache = Cache::new(capacity).expect("capacity is valid");
        let mut out = Vec::new();
        for line in lines {
            let command = parse_command(line).expect("test commands parse");
            run(&mut cache, &command, &mut out).expect("test commands succeed");
        }
        String::from_utf8(out).expect("output is UTF-8")
    }

    #[test]
    fn put_takes_the_rest_of_the_line_as_the_value() {
        assert_eq!(
            parse_command("put greeting hello  there").unwrap(),
            Command::Put {
                key: "greeting".to_string(),
                value: "hello  there".to_string(),
            }
        );
    }

    #[test]
    fn leading_whitespace_does_not_change_a_command() {
        assert_eq!(
            parse_command("   put k v").unwrap(),
            Command::Put {
                key: "k".to_string(),
                value: "v".to_string(),
            }
        );
    }

    #[test]
    fn key_commands_take_exactly_one_key() {
        assert_eq!(
            parse_command("get k").unwrap(),
            Command::Get("k".to_string())
        );
        assert_eq!(
            parse_command("del k").unwrap(),
            Command::Del("k".to_string())
        );
        assert_eq!(
            parse_command("has k").unwrap(),
            Command::Has("k".to_string())
        );

        assert!(parse_command("get").is_err());
        assert!(parse_command("get a b").is_err());
    }

    #[test]
    fn bare_commands_take_no_arguments() {
        assert_eq!(parse_command("keys").unwrap(), Command::Keys);
        assert_eq!(parse_command("len").unwrap(), Command::Len);
        assert_eq!(parse_command("stats").unwrap(), Command::Stats);

        assert!(parse_command("keys extra").is_err());
    }

    #[test]
    fn an_incomplete_put_is_a_usage_error() {
        assert!(parse_command("put").is_err());
        assert!(parse_command("put k").is_err());
        assert!(parse_command("put k   ").is_err());
    }

    #[test]
    fn an_unknown_verb_is_a_usage_error() {
        assert_eq!(
            parse_command("frobnicate k").unwrap_err(),
            "unknown command: frobnicate"
        );
    }

    #[test]
    fn blank_and_comment_lines_are_skipped() {
        assert!(is_blank_or_comment(""));
        assert!(is_blank_or_comment("   "));
        assert!(is_blank_or_comment("  # note"));
        assert!(!is_blank_or_comment("get k"));
    }

    #[test]
    fn a_miss_prints_rather_than_failing() {
        assert_eq!(output_of(4, &["get absent"]), "(miss)\n");
    }

    #[test]
    fn eviction_is_visible_through_the_cli() {
        let output = output_of(
            2,
            &["put a 1", "put b 2", "get a", "put c 3", "keys", "get b"],
        );

        assert_eq!(
            output, "stored a\nstored b\n1\nstored c\nc\na\n(miss)\n",
            "a was used, so b is what leaves"
        );
    }

    #[test]
    fn a_repeated_put_updates_rather_than_storing() {
        assert_eq!(
            output_of(4, &["put k 1", "put k 2", "get k", "len"]),
            "stored k\nupdated k\n2\n1\n"
        );
    }

    #[test]
    fn stats_comes_from_the_cpp_formatter() {
        assert_eq!(
            output_of(2, &["put a 1", "get a", "get z", "stats"]),
            "stored a\n1\n(miss)\nhits=1 misses=1 evictions=0 size=1/2 hit_rate=0.500\n"
        );
    }

    #[test]
    fn a_library_error_is_reported_rather_than_unwrapped() {
        // No command line can produce an empty key -- `get` with nothing after
        // it is a usage error before the cache is reached -- so this drives
        // `run` directly. The point is that the arm exists: the library says a
        // key may be rejected, and the CLI turns that into exit 1 rather than
        // a panic, whether or not today's parser can reach it.
        let mut cache = Cache::new(2).unwrap();
        let mut out = Vec::new();

        let error = run(&mut cache, &Command::Get(String::new()), &mut out)
            .expect_err("an empty key is rejected by the C++");

        assert_eq!(error.to_string(), "key must not be empty");
        assert!(out.is_empty());
    }
}
