#ifndef SIMPLE_LOGGER_CPP_LOGGER_HPP
#define SIMPLE_LOGGER_CPP_LOGGER_HPP

#include <ctime>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace logger {

// Defaults shared with the C and Rust ports; they are part of the contract, so
// they live in the header rather than in an implementation detail.
inline constexpr std::string_view kDefaultDelimiter = " ";
inline constexpr std::string_view kDefaultSeparator = "\n";

// Environment variable holding fake epoch seconds for tests.
inline constexpr std::string_view kFakeTimeVar = "SIMPLE_LOGGER_FAKE_TIME";

// Severity tag written with each entry, ordered least to most severe.
enum class Level { kDebug, kInfo, kWarning, kError };

// The stage at which a logging operation failed. Stages backed by a libc call
// carry the failing errno in LogResult::ec; the rest describe bad input.
enum class LogStage {
  kOk,
  kOpenFile,  // opening the log file for append failed
  kWrite,     // a write or flush error occurred on the log file
  kClose,     // closing the log file failed; data may not be on disk
  kRead,      // a read error occurred on the message input stream
  kBadLevel,  // the level name was not debug, info, warning, or error
  kBadEscape, // a delimiter/separator escape was unrecognized
  kBadTime,   // the epoch seconds could not be rendered
};

// Returns a short human-readable label for a stage.
std::string_view describe(LogStage stage);

// Returns the uppercase label written for a level ("DEBUG", "INFO", ...).
std::string_view name(Level level);

// Maps a level name to a Level. Only the exact lowercase spellings "debug",
// "info", "warning", and "error" are accepted, so every port takes the same
// set. Returns nullopt for anything else.
std::optional<Level> parse_level(std::string_view text);

// Outcome of an operation that touches the filesystem. A stage other than kOk
// names what failed; ec carries the underlying errno when there was one.
struct LogResult {
  LogStage stage = LogStage::kOk;
  std::error_code ec{};

  bool ok() const noexcept { return stage == LogStage::kOk; }
  explicit operator bool() const noexcept { return ok(); }
};

// How an entry is laid out. An entry is written as
//
//   [<timestamp>]<delimiter>[<LEVEL>]<delimiter><message><separator>
//
// with the timestamp field omitted when show_timestamp is false and the level
// field omitted when show_level is false; omitting a field drops its trailing
// delimiter too. The separator follows every entry, including the last, so the
// next run appends onto a fresh line.
struct Format {
  std::string delimiter{kDefaultDelimiter};
  std::string separator{kDefaultSeparator};
  Level level = Level::kInfo;
  bool show_timestamp = true;
  bool show_level = true;
};

// Renders epoch seconds as a UTC ISO 8601 timestamp, "YYYY-MM-DDTHH:MM:SSZ".
// Always UTC, never local time, so the three ports agree without a timezone
// database. Negative values are valid. Returns nullopt when the time cannot be
// converted or the year does not fit in four digits.
std::optional<std::string> format_timestamp(std::time_t when);

// Expands backslash escapes in a delimiter or separator. A shell cannot
// portably hand a program a real newline, so "\n" typed on the command line has
// to mean one. Exactly four escapes are recognized: \n, \t, \r, and \\. Any
// other escape, including a trailing lone backslash, yields nullopt rather than
// passing through, so the accepted set cannot drift between ports.
std::optional<std::string> unescape(std::string_view text);

// Picks the timestamp for a run, reading the clock once so every entry a single
// invocation writes shares one timestamp. `fake` is the raw kFakeTimeVar value,
// or nullopt when unset; `real_now` is used only in that case. A fake value
// that is empty, unparseable, or has trailing junk returns nullopt — never a
// silent fallback to real_now, which would let a parity check pass while
// comparing three real clocks.
std::optional<std::time_t> resolve_clock(std::optional<std::string_view> fake,
                                         std::time_t real_now);

// Reads kFakeTimeVar and the system clock. The only impure function here; call
// it from main, not from library code.
std::optional<std::time_t> clock_now();

// Renders one entry. The message is copied verbatim: embedded newlines make the
// entry span physical lines, and embedded NULs and non-ASCII bytes are
// preserved. `timestamp` is unused when fmt.show_timestamp is false.
std::string format_entry(const Format &fmt, std::string_view timestamp,
                         std::string_view message);

// Writes one entry to out. Returns kOk or kWrite.
LogStage write_entry(std::ostream &out, const Format &fmt,
                     std::string_view timestamp, std::string_view message);

// Writes one entry per message, in order. An empty span writes nothing and
// succeeds.
LogStage write_messages(std::ostream &out, const Format &fmt,
                        std::string_view timestamp,
                        std::span<const std::string_view> messages);

// Writes one entry per line read from in. One trailing '\n' is stripped from
// each line, then one trailing '\r', so CRLF input logs the same bytes as LF
// input; a '\r' anywhere else is kept. A blank line becomes an entry with an
// empty message, and a final line without a trailing newline is still logged.
// Empty input writes nothing. Returns kOk, kWrite, or kRead.
LogStage write_lines(std::ostream &out, const Format &fmt,
                     std::string_view timestamp, std::istream &in);

// Opens path for appending, creating it if needed, and writes one entry per
// message. Existing contents are always kept; the file is never truncated, and
// it is created even when no entry is written.
LogResult append_messages(const std::filesystem::path &path, const Format &fmt,
                          std::string_view timestamp,
                          std::span<const std::string_view> messages);

// append_messages, taking its entries one per line from in.
LogResult append_lines(const std::filesystem::path &path, const Format &fmt,
                       std::string_view timestamp, std::istream &in);

} // namespace logger

#endif // SIMPLE_LOGGER_CPP_LOGGER_HPP
