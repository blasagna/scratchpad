#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "logger.hpp"

namespace {

namespace fs = std::filesystem;

using logger::Format;
using logger::Level;
using logger::LogStage;

// A message with no surprises, for the cases that are not about the message.
constexpr std::string_view kMsg = "hello";

// Renders a timestamp, failing the test if it cannot be formatted.
std::string stamp(std::time_t when) {
  const std::optional<std::string> out = logger::format_timestamp(when);
  EXPECT_TRUE(out.has_value());
  return out.value_or("");
}

// Runs body against an in-memory stream and returns what it wrote.
template <typename F> std::string captured(F body) {
  std::ostringstream out;
  body(out);
  return out.str();
}

std::string tmp_path(const char *name) {
  const char *dir = std::getenv("TEST_TMPDIR");
  return std::string(dir ? dir : ".") + "/" + name;
}

void write_file(const std::string &path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  ASSERT_TRUE(out.good());
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// Wraps a single message so it can be passed as a span.
std::vector<std::string_view> one(std::string_view message) {
  return {message};
}

// --- format_timestamp ---

TEST(FormatTimestamp, EpochZero) {
  EXPECT_EQ(stamp(0), "1970-01-01T00:00:00Z");
}

TEST(FormatTimestamp, OneSecondBeforeEpoch) {
  EXPECT_EQ(stamp(-1), "1969-12-31T23:59:59Z");
}

TEST(FormatTimestamp, Year2000) {
  EXPECT_EQ(stamp(946684800), "2000-01-01T00:00:00Z");
}

TEST(FormatTimestamp, LeapDay2000) {
  EXPECT_EQ(stamp(951782400), "2000-02-29T00:00:00Z");
}

TEST(FormatTimestamp, LeapDay2024WithTimeOfDay) {
  EXPECT_EQ(stamp(1709210096), "2024-02-29T12:34:56Z");
}

TEST(FormatTimestamp, Year1900HadNoLeapDay) {
  EXPECT_EQ(stamp(-2203977600), "1900-02-28T00:00:00Z");
  EXPECT_EQ(stamp(-2203977600 + 86400), "1900-03-01T00:00:00Z");
}

TEST(FormatTimestamp, EndOfYearRollover) {
  EXPECT_EQ(stamp(1735689599), "2024-12-31T23:59:59Z");
  EXPECT_EQ(stamp(1735689600), "2025-01-01T00:00:00Z");
}

TEST(FormatTimestamp, Past2038) {
  EXPECT_EQ(stamp(2147483648), "2038-01-19T03:14:08Z");
}

TEST(FormatTimestamp, RejectsAYearBeyondFourDigits) {
  // 253402300800 is 10000-01-01T00:00:00Z.
  EXPECT_FALSE(logger::format_timestamp(253402300800).has_value());
}

// --- parse_level / name ---

TEST(ParseLevel, AcceptsAllFourNames) {
  EXPECT_EQ(logger::parse_level("debug"), Level::kDebug);
  EXPECT_EQ(logger::parse_level("info"), Level::kInfo);
  EXPECT_EQ(logger::parse_level("warning"), Level::kWarning);
  EXPECT_EQ(logger::parse_level("error"), Level::kError);
}

TEST(ParseLevel, RejectsUppercase) {
  EXPECT_FALSE(logger::parse_level("INFO").has_value());
}

TEST(ParseLevel, RejectsUnknown) {
  EXPECT_FALSE(logger::parse_level("warn").has_value());
  EXPECT_FALSE(logger::parse_level("trace").has_value());
}

TEST(ParseLevel, RejectsEmpty) {
  EXPECT_FALSE(logger::parse_level("").has_value());
}

TEST(LevelName, UppercaseLabels) {
  EXPECT_EQ(logger::name(Level::kDebug), "DEBUG");
  EXPECT_EQ(logger::name(Level::kInfo), "INFO");
  EXPECT_EQ(logger::name(Level::kWarning), "WARNING");
  EXPECT_EQ(logger::name(Level::kError), "ERROR");
}

// --- resolve_clock ---

TEST(ResolveClock, UsesRealNowWhenUnset) {
  EXPECT_EQ(logger::resolve_clock(std::nullopt, 12345), 12345);
}

TEST(ResolveClock, ParsesFakeSeconds) {
  EXPECT_EQ(logger::resolve_clock(std::string_view("1751328000"), 99),
            1751328000);
}

TEST(ResolveClock, ParsesNegativeFakeSeconds) {
  EXPECT_EQ(logger::resolve_clock(std::string_view("-1"), 99), -1);
}

TEST(ResolveClock, RejectsTrailingJunk) {
  EXPECT_FALSE(logger::resolve_clock(std::string_view("12x"), 99).has_value());
}

TEST(ResolveClock, RejectsEmptyValue) {
  EXPECT_FALSE(logger::resolve_clock(std::string_view(""), 99).has_value());
}

TEST(ResolveClock, RejectsNonNumeric) {
  EXPECT_FALSE(logger::resolve_clock(std::string_view("nope"), 99).has_value());
}

TEST(ResolveClock, RejectsLeadingWhitespaceAndPlusSign) {
  // strtoll and Rust's parse are each looser than this in a different way, so
  // the three ports agree only because all of them refuse both spellings.
  EXPECT_FALSE(logger::resolve_clock(std::string_view(" 5"), 99).has_value());
  EXPECT_FALSE(logger::resolve_clock(std::string_view("+5"), 99).has_value());
}

// --- format_entry ---

TEST(FormatEntry, DefaultFormat) {
  const Format fmt;
  EXPECT_EQ(logger::format_entry(fmt, "TS", kMsg), "[TS] [INFO] hello\n");
}

TEST(FormatEntry, EachLevelLabel) {
  const std::array<std::pair<Level, std::string_view>, 4> cases{{
      {Level::kDebug, "[TS] [DEBUG] hello\n"},
      {Level::kInfo, "[TS] [INFO] hello\n"},
      {Level::kWarning, "[TS] [WARNING] hello\n"},
      {Level::kError, "[TS] [ERROR] hello\n"},
  }};
  for (const auto &[level, expected] : cases) {
    Format fmt;
    fmt.level = level;
    EXPECT_EQ(logger::format_entry(fmt, "TS", kMsg), expected);
  }
}

TEST(FormatEntry, WithoutTimestamp) {
  Format fmt;
  fmt.show_timestamp = false;
  EXPECT_EQ(logger::format_entry(fmt, "TS", kMsg), "[INFO] hello\n");
}

TEST(FormatEntry, WithoutLevel) {
  Format fmt;
  fmt.show_level = false;
  EXPECT_EQ(logger::format_entry(fmt, "TS", kMsg), "[TS] hello\n");
}

TEST(FormatEntry, WithoutTimestampOrLevel) {
  Format fmt;
  fmt.show_timestamp = false;
  fmt.show_level = false;
  EXPECT_EQ(logger::format_entry(fmt, "TS", kMsg), "hello\n");
}

TEST(FormatEntry, EmptyMessageKeepsBothFields) {
  const Format fmt;
  EXPECT_EQ(logger::format_entry(fmt, "TS", ""), "[TS] [INFO] \n");
}

TEST(FormatEntry, MessageContainingNewlineIsVerbatim) {
  const Format fmt;
  EXPECT_EQ(logger::format_entry(fmt, "TS", "first\nsecond"),
            "[TS] [INFO] first\nsecond\n");
}

TEST(FormatEntry, MessageWithEmbeddedNul) {
  const Format fmt;
  const std::string_view message("a\0b", 3);
  EXPECT_EQ(logger::format_entry(fmt, "TS", message),
            std::string("[TS] [INFO] a\0b\n", 16));
}

TEST(FormatEntry, NonAsciiBytesArePassedThrough) {
  const Format fmt;
  EXPECT_EQ(logger::format_entry(fmt, "TS", "h\xc3\xa9llo"),
            "[TS] [INFO] h\xc3\xa9llo\n");
}

// --- write_messages ---

// The trailing newline after the last entry is deliberate: it is what makes the
// next run start on a fresh line.
TEST(WriteMessages, WritesOneEntryPerMessage) {
  const Format fmt;
  const std::vector<std::string_view> messages{"one", "two", "three"};
  const std::string out = captured([&](std::ostream &o) {
    EXPECT_EQ(logger::write_messages(o, fmt, "TS", messages), LogStage::kOk);
  });
  EXPECT_EQ(out, "[TS] [INFO] one\n[TS] [INFO] two\n[TS] [INFO] three\n");
}

TEST(WriteMessages, AllEntriesShareOneTimestamp) {
  const Format fmt;
  const std::vector<std::string_view> messages{"a", "b"};
  const std::string out = captured([&](std::ostream &o) {
    EXPECT_EQ(logger::write_messages(o, fmt, "SAME", messages), LogStage::kOk);
  });
  EXPECT_EQ(out, "[SAME] [INFO] a\n[SAME] [INFO] b\n");
}

TEST(WriteMessages, ZeroMessagesWritesNothing) {
  const Format fmt;
  const std::vector<std::string_view> messages;
  const std::string out = captured([&](std::ostream &o) {
    EXPECT_EQ(logger::write_messages(o, fmt, "TS", messages), LogStage::kOk);
  });
  EXPECT_EQ(out, "");
}

// --- write_lines ---

std::string lines_output(const std::string &input) {
  const Format fmt;
  std::istringstream in(input);
  return captured([&](std::ostream &o) {
    EXPECT_EQ(logger::write_lines(o, fmt, "TS", in), LogStage::kOk);
  });
}

TEST(WriteLines, OneEntryPerLine) {
  EXPECT_EQ(lines_output("a\nb\nc\n"),
            "[TS] [INFO] a\n[TS] [INFO] b\n[TS] [INFO] c\n");
}

TEST(WriteLines, LastLineWithoutNewlineStillLogs) {
  EXPECT_EQ(lines_output("a\nb"), "[TS] [INFO] a\n[TS] [INFO] b\n");
}

TEST(WriteLines, StripsTrailingCarriageReturn) {
  EXPECT_EQ(lines_output("a\r\nb\r\n"), "[TS] [INFO] a\n[TS] [INFO] b\n");
}

TEST(WriteLines, KeepsInteriorCarriageReturn) {
  EXPECT_EQ(lines_output("a\rb\n"), "[TS] [INFO] a\rb\n");
}

TEST(WriteLines, BlankLineBecomesEmptyMessage) {
  EXPECT_EQ(lines_output("a\n\nb\n"),
            "[TS] [INFO] a\n[TS] [INFO] \n[TS] [INFO] b\n");
}

TEST(WriteLines, EmptyInputWritesNothing) { EXPECT_EQ(lines_output(""), ""); }

// --- append_messages / append_lines ---

TEST(Append, CreatesFileWhenMissing) {
  const std::string path = tmp_path("cpp_creates.txt");
  fs::remove(path);

  const Format fmt;
  EXPECT_TRUE(logger::append_messages(path, fmt, "TS", one(kMsg)));
  EXPECT_EQ(read_file(path), "[TS] [INFO] hello\n");
}

TEST(Append, AppendsWithoutTruncating) {
  const std::string path = tmp_path("cpp_appends.txt");
  write_file(path, "existing\n");

  const Format fmt;
  EXPECT_TRUE(logger::append_messages(path, fmt, "TS", one(kMsg)));
  EXPECT_EQ(read_file(path), "existing\n[TS] [INFO] hello\n");
}

TEST(Append, CreatesFileEvenWithNoEntries) {
  const std::string path = tmp_path("cpp_empty.txt");
  fs::remove(path);

  const Format fmt;
  const std::vector<std::string_view> none;
  EXPECT_TRUE(logger::append_messages(path, fmt, "TS", none));
  EXPECT_EQ(read_file(path), "");
}

TEST(Append, TwoRunsAppendInOrder) {
  const std::string path = tmp_path("cpp_two_runs.txt");
  fs::remove(path);

  const Format fmt;
  EXPECT_TRUE(logger::append_messages(path, fmt, "T1", one("first")));
  EXPECT_TRUE(logger::append_messages(path, fmt, "T2", one("second")));
  EXPECT_EQ(read_file(path), "[T1] [INFO] first\n[T2] [INFO] second\n");
}

TEST(Append, LinesFromAStream) {
  const std::string path = tmp_path("cpp_lines.txt");
  fs::remove(path);

  const Format fmt;
  std::istringstream in("a\nb\n");
  EXPECT_TRUE(logger::append_lines(path, fmt, "TS", in));
  EXPECT_EQ(read_file(path), "[TS] [INFO] a\n[TS] [INFO] b\n");
}

TEST(Append, OpenFailsWhenParentDirectoryIsMissing) {
  const std::string path = tmp_path("cpp_no_such_dir/log.txt");
  const Format fmt;
  const logger::LogResult result =
      logger::append_messages(path, fmt, "TS", one(kMsg));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.stage, LogStage::kOpenFile);
}

TEST(Append, OpenFailsOnADirectory) {
  const char *dir = std::getenv("TEST_TMPDIR");
  const Format fmt;
  const logger::LogResult result =
      logger::append_messages(dir ? dir : ".", fmt, "TS", one(kMsg));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.stage, LogStage::kOpenFile);
}

// --- describe ---

TEST(Describe, NamesEveryStage) {
  const std::array<LogStage, 7> all{
      LogStage::kOk,      LogStage::kOpenFile, LogStage::kWrite,
      LogStage::kClose,   LogStage::kRead,     LogStage::kBadLevel,
      LogStage::kBadTime,
  };
  for (const LogStage stage : all)
    EXPECT_NE(logger::describe(stage), "unknown error");
}

} // namespace
