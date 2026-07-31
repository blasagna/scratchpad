//! Library for appending timestamped, level-tagged messages to a log file.
//!
//! Semantics match the C and C++ ports: an entry is
//! `[<timestamp>]<delim>[<LEVEL>]<delim><message><separator>`, the timestamp is
//! UTC ISO 8601 read once per run, and the separator follows every entry
//! including the last so the next run appends onto a fresh line.
//!
//! Note: the timestamp comes from `jiff::civil::DateTime`, which renders the
//! same `YYYY-MM-DDTHH:MM:SS` the other two ports build by hand from `gmtime_r`.
//! Deliberately not `jiff::Timestamp` — see [`format_timestamp`] for why that
//! one cannot cover the range. The C and C++ ports have no equivalent to reach
//! for, so they still do the civil date arithmetic themselves.

use std::ffi::OsStr;
use std::fmt;
use std::fs::{File, OpenOptions};
use std::io::{self, BufRead, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// Text placed between an entry's fields.
pub const DEFAULT_DELIMITER: &str = " ";
/// Text placed after every entry.
pub const DEFAULT_SEPARATOR: &str = "\n";
/// The default separator as the user would type it on a command line.
///
/// Routing this through the same unescaper as a user-supplied value is what
/// makes `--help` show `[default: \n]` instead of breaking its own layout with a
/// real newline.
pub const DEFAULT_SEPARATOR_ESCAPED: &str = "\\n";
/// Level assumed when `--level` is not given.
pub const DEFAULT_LEVEL: Level = Level::Info;
/// Environment variable holding fake epoch seconds for tests.
pub const FAKE_TIME_VAR: &str = "SIMPLE_LOGGER_FAKE_TIME";

/// Severity tag written with each entry, ordered least to most severe.
///
/// The spellings accepted on the command line are the lowercase names; the
/// label written to the log is uppercase.
#[derive(Debug, Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum Level {
    Debug,
    Info,
    Warning,
    Error,
}

impl Level {
    /// The uppercase label written inside the brackets (`"INFO"`).
    pub fn label(self) -> &'static str {
        match self {
            Level::Debug => "DEBUG",
            Level::Info => "INFO",
            Level::Warning => "WARNING",
            Level::Error => "ERROR",
        }
    }
}

impl fmt::Display for Level {
    /// The lowercase spelling accepted on the command line, so clap can render
    /// the default in `--help`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let name = match self {
            Level::Debug => "debug",
            Level::Info => "info",
            Level::Warning => "warning",
            Level::Error => "error",
        };
        f.write_str(name)
    }
}

/// How an entry is laid out.
///
/// Omitting a field with `show_timestamp` or `show_level` drops its trailing
/// delimiter too.
#[derive(Debug, Clone)]
pub struct Format {
    pub delimiter: String,
    pub separator: String,
    pub level: Level,
    pub show_timestamp: bool,
    pub show_level: bool,
}

impl Default for Format {
    fn default() -> Self {
        Format {
            delimiter: DEFAULT_DELIMITER.to_string(),
            separator: DEFAULT_SEPARATOR.to_string(),
            level: DEFAULT_LEVEL,
            show_timestamp: true,
            show_level: true,
        }
    }
}

/// A logging operation that failed, naming the stage and the file involved.
#[derive(Debug)]
pub enum LogError {
    /// Opening the log file for append failed.
    Open { path: PathBuf, source: io::Error },
    /// Writing or flushing the log file failed.
    Write { path: PathBuf, source: io::Error },
    /// Reading the message input stream failed.
    Read(io::Error),
}

impl fmt::Display for LogError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LogError::Open { path, source } => {
                write!(f, "cannot open log file '{}': {source}", path.display())
            }
            LogError::Write { path, source } => {
                write!(f, "error writing log file '{}': {source}", path.display())
            }
            LogError::Read(source) => write!(f, "error reading input: {source}"),
        }
    }
}

impl std::error::Error for LogError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            LogError::Open { source, .. }
            | LogError::Write { source, .. }
            | LogError::Read(source) => Some(source),
        }
    }
}

/// Which side of [`write_lines`] failed, so the caller can name the right file.
#[derive(Debug)]
pub enum LineError {
    Read(io::Error),
    Write(io::Error),
}

/// An unrecognized escape in a delimiter or separator.
#[derive(Debug, PartialEq, Eq)]
pub struct UnescapeError {
    /// The character after the backslash, or `None` for a trailing lone one.
    pub escape: Option<char>,
}

impl fmt::Display for UnescapeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.escape {
            Some(c) => write!(f, "unknown escape '\\{c}'"),
            None => f.write_str("trailing lone backslash"),
        }
    }
}

impl std::error::Error for UnescapeError {}

/// A [`FAKE_TIME_VAR`] value that is not epoch seconds.
#[derive(Debug, PartialEq, Eq)]
pub struct ClockError {
    pub value: String,
}

impl fmt::Display for ClockError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "invalid {FAKE_TIME_VAR} value '{}' (expected epoch seconds)",
            self.value
        )
    }
}

impl std::error::Error for ClockError {}

/// Renders epoch seconds as a UTC ISO 8601 timestamp, `YYYY-MM-DDTHH:MM:SSZ`.
///
/// Always UTC, never local time, so the three ports agree without a timezone
/// database. Negative values are valid. Returns `None` outside the four-digit
/// years 0000 through 9999, exactly where the C and C++ ports return
/// `LOG_ERR_BAD_TIME`, since a wider year has no agreed rendering across them.
///
/// Built on `jiff::civil::DateTime` rather than `jiff::Timestamp`, which cannot
/// do the job: `Timestamp` reserves headroom for a timezone offset, so its
/// ceiling is 9999-12-30T22:00:00Z — *inside* the four-digit range, leaving the
/// last 26 hours of year 9999 renderable by C and C++ but not by it. The civil
/// types carry no offset and span the whole range.
///
/// Two properties of `jiff` make the output line up with the other ports, and
/// both are asserted in the tests below rather than left implicit: `DateTime`'s
/// `Display` zero-pads the year to four digits, and it omits the fractional
/// seconds when they are zero. The trailing `Z` is ours to add, since a civil
/// datetime has no zone to name.
pub fn format_timestamp(epoch_seconds: i64) -> Option<String> {
    let epoch = jiff::civil::date(1970, 1, 1).at(0, 0, 0, 0);
    let datetime = epoch
        .checked_add(jiff::SignedDuration::from_secs(epoch_seconds))
        .ok()?;

    // checked_add already refuses year 10000 and beyond; a negative year is
    // representable but renders as "-000001-...", which is neither the
    // documented shape nor what C and C++ accept.
    if datetime.year() < 0 {
        return None;
    }
    Some(format!("{datetime}Z"))
}

/// Expands backslash escapes in a delimiter or separator.
///
/// A shell cannot portably hand a program a real newline, so `\n` typed on the
/// command line has to mean one. Exactly four escapes are recognized: `\n`,
/// `\t`, `\r`, and `\\`. Any other escape, including a trailing lone backslash,
/// is an error rather than a pass-through, so the accepted set cannot drift
/// between ports.
pub fn unescape(text: &str) -> Result<String, UnescapeError> {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars();

    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('t') => out.push('\t'),
            Some('r') => out.push('\r'),
            Some('\\') => out.push('\\'),
            other => return Err(UnescapeError { escape: other }),
        }
    }

    Ok(out)
}

/// Picks the timestamp for a run.
///
/// `fake` is the raw [`FAKE_TIME_VAR`] value, or `None` when unset; `real_now`
/// is used only in that case. A fake value that is empty, unparseable, or has
/// trailing junk is an error — never a silent fallback to `real_now`, which
/// would let a parity check pass while comparing three real clocks.
pub fn resolve_clock(fake: Option<&OsStr>, real_now: i64) -> Result<i64, ClockError> {
    let Some(fake) = fake else {
        return Ok(real_now);
    };
    let text = fake.to_string_lossy();
    let reject = || ClockError {
        value: text.to_string(),
    };

    // Accept exactly -?[0-9]+. `str::parse` would also take a '+' sign and the
    // C port's strtoll would take leading whitespace, so the shape is checked
    // by hand to keep the three ports on the same set.
    let digits = text.strip_prefix('-').unwrap_or(&text);
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return Err(reject());
    }

    text.parse::<i64>().map_err(|_| reject())
}

/// Reads [`FAKE_TIME_VAR`] and the system clock.
///
/// The only impure function here; call it from `main`, not from library code.
pub fn clock_now() -> Result<i64, ClockError> {
    if let Some(fake) = std::env::var_os(FAKE_TIME_VAR) {
        return resolve_clock(Some(&fake), 0);
    }

    let now = SystemTime::now();
    let seconds = match now.duration_since(UNIX_EPOCH) {
        Ok(since) => since.as_secs() as i64,
        // A system clock set before 1970 is still a time we can render.
        Err(before) => -(before.duration().as_secs() as i64),
    };
    resolve_clock(None, seconds)
}

/// Renders one entry.
///
/// The message is copied verbatim, so embedded newlines make the entry span
/// physical lines and embedded NULs and non-ASCII bytes are preserved — which
/// is why it takes bytes rather than a `&str`. `timestamp` is unused when
/// `fmt.show_timestamp` is false.
pub fn format_entry(fmt: &Format, timestamp: &str, message: &[u8]) -> Vec<u8> {
    let mut entry = Vec::with_capacity(timestamp.len() + message.len() + 16);

    if fmt.show_timestamp {
        entry.push(b'[');
        entry.extend_from_slice(timestamp.as_bytes());
        entry.push(b']');
        entry.extend_from_slice(fmt.delimiter.as_bytes());
    }
    if fmt.show_level {
        entry.push(b'[');
        entry.extend_from_slice(fmt.level.label().as_bytes());
        entry.push(b']');
        entry.extend_from_slice(fmt.delimiter.as_bytes());
    }
    entry.extend_from_slice(message);
    entry.extend_from_slice(fmt.separator.as_bytes());
    entry
}

/// Writes one entry to `out`.
pub fn write_entry<W: Write>(
    out: &mut W,
    fmt: &Format,
    timestamp: &str,
    message: &[u8],
) -> io::Result<()> {
    out.write_all(&format_entry(fmt, timestamp, message))
}

/// Writes one entry per message, in order. An empty slice writes nothing.
pub fn write_messages<W: Write, M: AsRef<[u8]>>(
    out: &mut W,
    fmt: &Format,
    timestamp: &str,
    messages: &[M],
) -> io::Result<()> {
    for message in messages {
        write_entry(out, fmt, timestamp, message.as_ref())?;
    }
    Ok(())
}

/// Writes one entry per line read from `input`.
///
/// One trailing `\n` is stripped from each line, then one trailing `\r`, so
/// CRLF input logs the same bytes as LF input; a `\r` anywhere else is kept. A
/// blank line becomes an entry with an empty message, and a final line without a
/// trailing newline is still logged. Empty input writes nothing.
pub fn write_lines<R: BufRead, W: Write>(
    input: &mut R,
    out: &mut W,
    fmt: &Format,
    timestamp: &str,
) -> Result<(), LineError> {
    // read_until rather than lines(): it keeps the bytes as they arrived, so a
    // line containing a NUL or invalid UTF-8 survives intact, and it leaves the
    // '\r' for us to strip on purpose rather than silently.
    let mut line = Vec::new();
    loop {
        line.clear();
        if input
            .read_until(b'\n', &mut line)
            .map_err(LineError::Read)?
            == 0
        {
            return Ok(());
        }
        if line.last() == Some(&b'\n') {
            line.pop();
        }
        if line.last() == Some(&b'\r') {
            line.pop();
        }
        write_entry(out, fmt, timestamp, &line).map_err(LineError::Write)?;
    }
}

/// Opens `path` for appending, creating it if needed.
///
/// Existing contents are always kept; the file is never truncated, and it is
/// created even when no entry ends up being written.
fn open_append(path: &Path) -> Result<File, LogError> {
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .map_err(|source| LogError::Open {
            path: path.to_path_buf(),
            source,
        })
}

/// Appends one entry per message to the log file at `path`.
pub fn append_messages<M: AsRef<[u8]>>(
    path: &Path,
    fmt: &Format,
    timestamp: &str,
    messages: &[M],
) -> Result<(), LogError> {
    let mut out = BufWriter::new(open_append(path)?);
    let write_failed = |source| LogError::Write {
        path: path.to_path_buf(),
        source,
    };

    write_messages(&mut out, fmt, timestamp, messages).map_err(write_failed)?;
    // Flush explicitly: dropping a BufWriter discards any error it hits.
    out.flush().map_err(write_failed)
}

/// [`append_messages`], taking its entries one per line from `input`.
pub fn append_lines<R: BufRead>(
    path: &Path,
    fmt: &Format,
    timestamp: &str,
    input: &mut R,
) -> Result<(), LogError> {
    let mut out = BufWriter::new(open_append(path)?);
    let write_failed = |source| LogError::Write {
        path: path.to_path_buf(),
        source,
    };

    write_lines(input, &mut out, fmt, timestamp).map_err(|err| match err {
        LineError::Read(source) => LogError::Read(source),
        LineError::Write(source) => write_failed(source),
    })?;
    out.flush().map_err(write_failed)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stamp(epoch_seconds: i64) -> String {
        format_timestamp(epoch_seconds).expect("timestamp should render")
    }

    fn entry(fmt: &Format, message: &str) -> String {
        String::from_utf8(format_entry(fmt, "TS", message.as_bytes())).unwrap()
    }

    fn lines_output(input: &str) -> String {
        let fmt = Format::default();
        let mut out: Vec<u8> = Vec::new();
        write_lines(&mut input.as_bytes(), &mut out, &fmt, "TS").expect("write should succeed");
        String::from_utf8(out).unwrap()
    }

    // --- format_timestamp ---
    //
    // These vectors no longer test arithmetic in this file; they pin the format
    // the three ports have to agree on, against whatever jiff renders. That is
    // the reason to keep them: a jiff upgrade that changed Display, or a swap to
    // another date crate, would break parity with C and C++ here rather than in
    // check_parity.sh. The same vectors appear in c/test_logger.c and
    // cpp/test_logger.cpp.

    #[test]
    fn epoch_zero_is_the_unix_epoch() {
        assert_eq!(stamp(0), "1970-01-01T00:00:00Z");
    }

    #[test]
    fn one_second_before_the_epoch() {
        assert_eq!(stamp(-1), "1969-12-31T23:59:59Z");
    }

    #[test]
    fn year_2000_starts_correctly() {
        assert_eq!(stamp(946_684_800), "2000-01-01T00:00:00Z");
    }

    #[test]
    fn leap_day_2000_exists() {
        assert_eq!(stamp(951_782_400), "2000-02-29T00:00:00Z");
    }

    #[test]
    fn leap_day_2024_with_time_of_day() {
        assert_eq!(stamp(1_709_210_096), "2024-02-29T12:34:56Z");
    }

    #[test]
    fn year_1900_had_no_leap_day() {
        assert_eq!(stamp(-2_203_977_600), "1900-02-28T00:00:00Z");
        assert_eq!(stamp(-2_203_977_600 + 86_400), "1900-03-01T00:00:00Z");
    }

    #[test]
    fn end_of_year_rollover() {
        assert_eq!(stamp(1_735_689_599), "2024-12-31T23:59:59Z");
        assert_eq!(stamp(1_735_689_600), "2025-01-01T00:00:00Z");
    }

    #[test]
    fn survives_the_2038_signed_32_bit_wrap() {
        assert_eq!(stamp(2_147_483_648), "2038-01-19T03:14:08Z");
    }

    #[test]
    fn pads_every_field_to_two_digits() {
        // 2000-01-01 would render as "2000-1-1" without the padding, which is
        // exactly what glibc's strftime "%Y" does to a year below 1000.
        assert_eq!(stamp(946_684_800), "2000-01-01T00:00:00Z");
        assert_eq!(stamp(946_684_800 + 3661), "2000-01-01T01:01:01Z");
    }

    #[test]
    fn accepts_exactly_the_four_digit_years_the_other_ports_do() {
        // Both boundaries, to the second, because this is where a date crate's
        // own limits are most likely to disagree with gmtime_r. An earlier
        // version built on jiff::Timestamp passed a lone 253402300800 vector
        // while silently rejecting everything from 253402207201 up -- the last
        // 26 hours of year 9999, which C and C++ log without complaint.
        assert_eq!(
            format_timestamp(-62_167_219_200).as_deref(),
            Some("0000-01-01T00:00:00Z")
        );
        assert_eq!(
            format_timestamp(253_402_300_799).as_deref(),
            Some("9999-12-31T23:59:59Z")
        );
        // One second past each end. 253402300800 is 10000-01-01T00:00:00Z.
        assert_eq!(format_timestamp(-62_167_219_201), None);
        assert_eq!(format_timestamp(253_402_300_800), None);
    }

    #[test]
    fn renders_a_whole_second_without_a_fractional_part() {
        // Two load-bearing jiff properties: DateTime's Display omits
        // ".000000000" when the fraction is zero, and pads the year to four
        // digits. If either stopped holding, every entry this port writes would
        // diverge from C and C++, so both are asserted directly rather than left
        // implicit in the vectors above.
        let rendered = format_timestamp(1_751_328_000).unwrap();
        assert!(!rendered.contains('.'), "unexpected fraction in {rendered}");
        assert_eq!(rendered.len(), 20);
        assert!(
            format_timestamp(-62_167_219_200)
                .unwrap()
                .starts_with("0000")
        );
    }

    // --- levels ---

    #[test]
    fn level_labels_are_uppercase() {
        assert_eq!(Level::Debug.label(), "DEBUG");
        assert_eq!(Level::Info.label(), "INFO");
        assert_eq!(Level::Warning.label(), "WARNING");
        assert_eq!(Level::Error.label(), "ERROR");
    }

    #[test]
    fn level_display_is_the_lowercase_command_line_spelling() {
        assert_eq!(Level::Warning.to_string(), "warning");
    }

    // --- unescape ---

    #[test]
    fn unescape_passes_plain_text() {
        assert_eq!(unescape(" | ").unwrap(), " | ");
    }

    #[test]
    fn unescape_translates_the_four_supported_escapes() {
        assert_eq!(unescape("\\n\\t\\r").unwrap(), "\n\t\r");
        assert_eq!(unescape("a\\\\b").unwrap(), "a\\b");
    }

    #[test]
    fn unescape_rejects_an_unknown_escape() {
        assert_eq!(unescape("\\q"), Err(UnescapeError { escape: Some('q') }));
    }

    #[test]
    fn unescape_rejects_a_trailing_backslash() {
        assert_eq!(unescape("ab\\"), Err(UnescapeError { escape: None }));
    }

    #[test]
    fn unescape_handles_an_empty_string() {
        assert_eq!(unescape("").unwrap(), "");
    }

    // --- resolve_clock ---

    #[test]
    fn resolve_clock_uses_real_now_when_unset() {
        assert_eq!(resolve_clock(None, 12_345).unwrap(), 12_345);
    }

    #[test]
    fn resolve_clock_parses_fake_seconds() {
        let fake = OsStr::new("1751328000");
        assert_eq!(resolve_clock(Some(fake), 99).unwrap(), 1_751_328_000);
    }

    #[test]
    fn resolve_clock_parses_negative_seconds() {
        assert_eq!(resolve_clock(Some(OsStr::new("-1")), 99).unwrap(), -1);
    }

    #[test]
    fn resolve_clock_rejects_trailing_junk() {
        assert!(resolve_clock(Some(OsStr::new("12x")), 99).is_err());
    }

    #[test]
    fn resolve_clock_rejects_an_empty_value() {
        assert!(resolve_clock(Some(OsStr::new("")), 99).is_err());
    }

    #[test]
    fn resolve_clock_rejects_leading_whitespace_and_a_plus_sign() {
        // str::parse would accept "+5" and the C port's strtoll would accept
        // " 5"; the three ports agree only because all of them refuse both.
        assert!(resolve_clock(Some(OsStr::new(" 5")), 99).is_err());
        assert!(resolve_clock(Some(OsStr::new("+5")), 99).is_err());
    }

    // --- format_entry ---

    #[test]
    fn formats_the_default_entry() {
        assert_eq!(entry(&Format::default(), "hello"), "[TS] [INFO] hello\n");
    }

    #[test]
    fn formats_each_level_label() {
        for (level, expected) in [
            (Level::Debug, "[TS] [DEBUG] hello\n"),
            (Level::Info, "[TS] [INFO] hello\n"),
            (Level::Warning, "[TS] [WARNING] hello\n"),
            (Level::Error, "[TS] [ERROR] hello\n"),
        ] {
            let fmt = Format {
                level,
                ..Format::default()
            };
            assert_eq!(entry(&fmt, "hello"), expected);
        }
    }

    #[test]
    fn honors_a_custom_delimiter() {
        let fmt = Format {
            delimiter: " | ".to_string(),
            ..Format::default()
        };
        assert_eq!(entry(&fmt, "hello"), "[TS] | [INFO] | hello\n");
    }

    #[test]
    fn honors_a_custom_separator() {
        let fmt = Format {
            separator: "\n\n".to_string(),
            ..Format::default()
        };
        assert_eq!(entry(&fmt, "hello"), "[TS] [INFO] hello\n\n");
    }

    #[test]
    fn omits_the_timestamp_field() {
        let fmt = Format {
            show_timestamp: false,
            ..Format::default()
        };
        assert_eq!(entry(&fmt, "hello"), "[INFO] hello\n");
    }

    #[test]
    fn omits_the_level_field() {
        let fmt = Format {
            show_level: false,
            ..Format::default()
        };
        assert_eq!(entry(&fmt, "hello"), "[TS] hello\n");
    }

    #[test]
    fn omits_both_fields() {
        let fmt = Format {
            show_timestamp: false,
            show_level: false,
            ..Format::default()
        };
        assert_eq!(entry(&fmt, "hello"), "hello\n");
    }

    #[test]
    fn logs_an_empty_message_keeping_both_fields() {
        assert_eq!(entry(&Format::default(), ""), "[TS] [INFO] \n");
    }

    #[test]
    fn keeps_a_newline_inside_a_message_verbatim() {
        assert_eq!(
            entry(&Format::default(), "first\nsecond"),
            "[TS] [INFO] first\nsecond\n"
        );
    }

    #[test]
    fn keeps_an_embedded_nul() {
        let out = format_entry(&Format::default(), "TS", b"a\0b");
        assert_eq!(out, b"[TS] [INFO] a\0b\n");
    }

    #[test]
    fn passes_non_ascii_bytes_through_unchanged() {
        // Not valid UTF-8, so it can only survive because the API is byte-based.
        let out = format_entry(&Format::default(), "TS", &[0xff, 0xfe]);
        assert_eq!(out, b"[TS] [INFO] \xff\xfe\n");
    }

    // --- write_messages ---

    #[test]
    fn writes_one_entry_per_message() {
        let mut out: Vec<u8> = Vec::new();
        write_messages(&mut out, &Format::default(), "TS", &["one", "two", "three"]).unwrap();
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "[TS] [INFO] one\n[TS] [INFO] two\n[TS] [INFO] three\n"
        );
    }

    #[test]
    fn all_entries_in_one_call_share_a_timestamp() {
        let mut out: Vec<u8> = Vec::new();
        write_messages(&mut out, &Format::default(), "SAME", &["a", "b"]).unwrap();
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "[SAME] [INFO] a\n[SAME] [INFO] b\n"
        );
    }

    #[test]
    fn writes_nothing_for_zero_messages() {
        let mut out: Vec<u8> = Vec::new();
        let none: [&str; 0] = [];
        write_messages(&mut out, &Format::default(), "TS", &none).unwrap();
        assert!(out.is_empty());
    }

    #[test]
    fn always_ends_with_the_separator() {
        let fmt = Format {
            separator: "|".to_string(),
            ..Format::default()
        };
        let mut out: Vec<u8> = Vec::new();
        write_messages(&mut out, &fmt, "TS", &["a", "b"]).unwrap();
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "[TS] [INFO] a|[TS] [INFO] b|"
        );
    }

    // --- write_lines ---

    #[test]
    fn reads_one_entry_per_input_line() {
        assert_eq!(
            lines_output("a\nb\nc\n"),
            "[TS] [INFO] a\n[TS] [INFO] b\n[TS] [INFO] c\n"
        );
    }

    #[test]
    fn logs_a_final_line_without_a_trailing_newline() {
        assert_eq!(lines_output("a\nb"), "[TS] [INFO] a\n[TS] [INFO] b\n");
    }

    #[test]
    fn strips_a_trailing_carriage_return() {
        assert_eq!(lines_output("a\r\nb\r\n"), "[TS] [INFO] a\n[TS] [INFO] b\n");
    }

    #[test]
    fn keeps_an_interior_carriage_return() {
        assert_eq!(lines_output("a\rb\n"), "[TS] [INFO] a\rb\n");
    }

    #[test]
    fn logs_a_blank_input_line_as_an_empty_message() {
        assert_eq!(
            lines_output("a\n\nb\n"),
            "[TS] [INFO] a\n[TS] [INFO] \n[TS] [INFO] b\n"
        );
    }

    #[test]
    fn writes_nothing_for_empty_input() {
        assert_eq!(lines_output(""), "");
    }
}
