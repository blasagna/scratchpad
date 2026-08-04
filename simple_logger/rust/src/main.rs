//! CLI for the logger: appends timestamped messages to a log file.

use std::io;
use std::path::Path;
use std::process::ExitCode;

use clap::Parser;
use simple_logger::{
    DEFAULT_LEVEL, FAKE_TIME_VAR, Format, Level, append_lines, append_messages, clock_now,
    format_timestamp,
};

/// Exit code for a usage error, matching the C and C++ ports. clap already uses
/// it for the errors it reports itself.
const EXIT_USAGE: u8 = 2;

#[derive(Parser)]
#[command(
    name = "simple_logger",
    about = "Appends timestamped messages to a log file.",
    long_about = "Appends timestamped messages to a log file. Each message argument becomes \
                  one entry; with no message arguments, one entry is read per line from \
                  stdin. The log file is opened for append, so previous entries are kept.\n\n\
                  Each entry is written as:\n  \
                  [<timestamp>] [<LEVEL>] <message>\n\n\
                  The timestamp is UTC ISO 8601 (e.g. [2026-07-30T18:22:05Z]) and is read \
                  once per run, so every entry one run writes shares it.",
    after_help = after_help()
)]
struct Cli {
    /// Severity tag written with each entry.
    #[arg(short, long, value_enum, default_value_t = DEFAULT_LEVEL)]
    level: Level,

    /// Omit the [timestamp] field.
    #[arg(long)]
    no_timestamp: bool,

    /// Omit the [LEVEL] field.
    #[arg(long)]
    no_level: bool,

    /// Log file to append to; created if it does not exist.
    #[arg(value_parser = clap::builder::NonEmptyStringValueParser::new())]
    logfile: String,

    /// Messages to log, one entry each. Reads stdin when none are given.
    messages: Vec<String>,
}

/// Trailing `--help` section. Built at run time rather than written as a
/// literal so the environment variable's name comes from the one constant the
/// library also reads it from.
fn after_help() -> String {
    format!(
        "Use -- before a message that begins with '-'.\n\n\
         Environment:\n  \
         {FAKE_TIME_VAR}  epoch seconds to use instead of the real clock; used by the \
         cross-port parity script."
    )
}

fn main() -> ExitCode {
    let cli = Cli::parse();

    let fmt = Format {
        level: cli.level,
        show_timestamp: !cli.no_timestamp,
        show_level: !cli.no_level,
    };

    // One clock reading for the whole run, so a slow stdin pipe cannot spread
    // one invocation's entries across several seconds.
    let when = match clock_now() {
        Ok(when) => when,
        // Only a bad FAKE_TIME_VAR override reaches here, and that is the
        // user's mistake rather than an operational failure.
        Err(err) => {
            eprintln!("error: {err}");
            return ExitCode::from(EXIT_USAGE);
        }
    };

    let Some(timestamp) = format_timestamp(when) else {
        eprintln!("simple_logger: cannot render the time {when} as a timestamp");
        return ExitCode::FAILURE;
    };

    let path = Path::new(&cli.logfile);
    let result = if cli.messages.is_empty() {
        append_lines(path, &fmt, &timestamp, &mut io::stdin().lock())
    } else {
        append_messages(path, &fmt, &timestamp, &cli.messages)
    };

    if let Err(err) = result {
        eprintln!("simple_logger: {err}");
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}
