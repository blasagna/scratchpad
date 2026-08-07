#include "logger.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <istream>
#include <ostream>

namespace logger {
namespace {

// The fixed entry layout: fields are separated by a space and every entry ends
// with a newline. Not options -- see Format in logger.hpp.
constexpr std::string_view kDelimiter = " ";
constexpr std::string_view kSeparator = "\n";

// Flushes and closes an output file, folding any late failure into the stage
// already reported. A write failure outranks a close failure: it names the
// earlier and more specific stage, and all three ports agree on that order.
LogResult finish(std::ofstream &out, LogStage stage) {
  out.flush();
  if (stage == LogStage::kOk && !out)
    stage = LogStage::kWrite;

  out.close();
  if (stage == LogStage::kOk && !out)
    stage = LogStage::kClose;

  if (stage == LogStage::kOk)
    return {};
  return {stage, std::error_code(errno, std::generic_category())};
}

// Opens path for appending without truncating it, in binary mode so the bytes
// written are the bytes stored.
LogResult open_append(const std::filesystem::path &path, std::ofstream &out) {
  out.open(path, std::ios::app | std::ios::binary);
  if (!out)
    return {LogStage::kOpenFile,
            std::error_code(errno, std::generic_category())};
  return {};
}

} // namespace

std::string_view describe(LogStage stage) {
  switch (stage) {
  case LogStage::kOk:
    return "success";
  case LogStage::kOpenFile:
    return "cannot open log file";
  case LogStage::kWrite:
    return "error writing log file";
  case LogStage::kClose:
    return "error closing log file";
  case LogStage::kRead:
    return "error reading input";
  case LogStage::kBadLevel:
    return "unknown log level";
  case LogStage::kBadTime:
    return "cannot determine the time";
  }
  return "unknown error";
}

std::string_view name(Level level) {
  switch (level) {
  case Level::kDebug:
    return "DEBUG";
  case Level::kInfo:
    return "INFO";
  case Level::kWarning:
    return "WARNING";
  case Level::kError:
    return "ERROR";
  }
  return "UNKNOWN";
}

std::optional<Level> parse_level(std::string_view text) {
  const auto it = std::find(kLevelNames.begin(), kLevelNames.end(), text);
  if (it == kLevelNames.end())
    return std::nullopt;
  return static_cast<Level>(std::distance(kLevelNames.begin(), it));
}

std::optional<std::string> format_timestamp(std::time_t when) {
  std::tm parts{};
  if (gmtime_r(&when, &parts) == nullptr)
    return std::nullopt;

  // A year outside four digits has no agreed rendering across the ports, so it
  // is refused rather than truncated or widened.
  const int year = parts.tm_year + 1900;
  if (year < 0 || year > 9999)
    return std::nullopt;

  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", year,
                     parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                     parts.tm_min, parts.tm_sec);
}

std::optional<std::time_t> resolve_clock(std::optional<std::string_view> fake,
                                         std::time_t real_now) {
  if (!fake)
    return real_now;

  // from_chars accepts exactly -?[0-9]+: no leading whitespace, no '+' sign,
  // no trailing junk. The C and Rust ports check that shape by hand to match,
  // since strtoll and str::parse are each more permissive.
  long long seconds = 0;
  const char *first = fake->data();
  const char *last = first + fake->size();
  const auto [stop, ec] = std::from_chars(first, last, seconds);
  if (ec != std::errc{} || stop != last)
    return std::nullopt;

  const auto when = static_cast<std::time_t>(seconds);
  if (static_cast<long long>(when) != seconds)
    return std::nullopt;
  return when;
}

std::optional<std::time_t> clock_now() {
  if (const char *fake = std::getenv(std::string(kFakeTimeVar).c_str()))
    return resolve_clock(std::string_view(fake), 0);

  const std::time_t now = std::time(nullptr);
  if (now == static_cast<std::time_t>(-1))
    return std::nullopt;
  return resolve_clock(std::nullopt, now);
}

std::string format_entry(const Format &fmt, std::string_view timestamp,
                         std::string_view message) {
  std::string entry;
  entry.reserve(timestamp.size() + message.size() + 16);

  if (fmt.show_timestamp) {
    entry.push_back('[');
    entry.append(timestamp);
    entry.push_back(']');
    entry.append(kDelimiter);
  }
  if (fmt.show_level) {
    entry.push_back('[');
    entry.append(name(fmt.level));
    entry.push_back(']');
    entry.append(kDelimiter);
  }
  entry.append(message);
  entry.append(kSeparator);
  return entry;
}

LogStage write_entry(std::ostream &out, const Format &fmt,
                     std::string_view timestamp, std::string_view message) {
  const std::string entry = format_entry(fmt, timestamp, message);
  out.write(entry.data(), static_cast<std::streamsize>(entry.size()));
  return out ? LogStage::kOk : LogStage::kWrite;
}

LogStage write_messages(std::ostream &out, const Format &fmt,
                        std::string_view timestamp,
                        std::span<const std::string_view> messages) {
  for (const std::string_view message : messages) {
    const LogStage stage = write_entry(out, fmt, timestamp, message);
    if (stage != LogStage::kOk)
      return stage;
  }
  return LogStage::kOk;
}

LogStage write_lines(std::ostream &out, const Format &fmt,
                     std::string_view timestamp, std::istream &in) {
  // std::getline fills a std::string by byte count, so a line containing a NUL
  // survives intact.
  std::string line;
  while (std::getline(in, line)) {
    // getline already consumed the '\n'. Strip one '\r' so CRLF input logs the
    // same bytes as LF input; a '\r' anywhere else belongs to the message.
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    const LogStage stage = write_entry(out, fmt, timestamp, line);
    if (stage != LogStage::kOk)
      return stage;
  }

  // getline sets failbit at a clean end of input too; only badbit is an error.
  return in.bad() ? LogStage::kRead : LogStage::kOk;
}

LogResult append_messages(const std::filesystem::path &path, const Format &fmt,
                          std::string_view timestamp,
                          std::span<const std::string_view> messages) {
  std::ofstream out;
  if (LogResult opened = open_append(path, out); !opened)
    return opened;
  return finish(out, write_messages(out, fmt, timestamp, messages));
}

LogResult append_lines(const std::filesystem::path &path, const Format &fmt,
                       std::string_view timestamp, std::istream &in) {
  std::ofstream out;
  if (LogResult opened = open_append(path, out); !opened)
    return opened;
  return finish(out, write_lines(out, fmt, timestamp, in));
}

} // namespace logger
