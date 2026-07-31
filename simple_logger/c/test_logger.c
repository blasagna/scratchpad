#include <gtest/gtest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

extern "C" {
#include "logger.h"
}

/* A message with no surprises, for the cases that are not about the message. */
static const char *kMsg = "hello";

static LogFormat default_format(void) {
  LogFormat fmt;
  fmt.delimiter = " ";
  fmt.separator = "\n";
  fmt.level = LOG_LEVEL_INFO;
  fmt.show_timestamp = 1;
  fmt.show_level = 1;
  return fmt;
}

/* Renders a timestamp, failing the test if it cannot be formatted. */
static std::string stamp(time_t when) {
  char buf[LOG_TIMESTAMP_BUF];
  EXPECT_EQ(log_format_timestamp(when, buf, sizeof(buf)), LOG_OK);
  return std::string(buf);
}

/* Copies and frees the malloc'd result of log_unescape. */
static std::string take(char *p) {
  std::string s(p);
  free(p);
  return s;
}

/* Runs body against an in-memory output stream and returns what it wrote. */
template<typename F> static std::string captured(F body) {
  char buf[4096];
  FILE *out = fmemopen(buf, sizeof(buf), "w");
  EXPECT_NE(out, nullptr);
  body(out);
  long written = ftell(out);
  EXPECT_EQ(fclose(out), 0);
  return std::string(buf, static_cast<size_t>(written < 0 ? 0 : written));
}

/* An in-memory input stream over data; the caller fcloses it. */
static FILE *make_input(const std::string &data) {
  FILE *in = fmemopen(const_cast<char *>(data.data()), data.size(), "r");
  EXPECT_NE(in, nullptr);
  return in;
}

static std::string tmp_path(const char *name) {
  const char *dir = getenv("TEST_TMPDIR");
  return std::string(dir ? dir : ".") + "/" + name;
}

static void write_file(const std::string &path, const std::string &contents) {
  FILE *f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(fwrite(contents.data(), 1, contents.size(), f), contents.size());
  ASSERT_EQ(fclose(f), 0);
}

static std::string read_file(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  EXPECT_NE(f, nullptr);
  if (!f)
    return "";
  std::string out;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  fclose(f);
  return out;
}

/* --- log_format_timestamp --- */

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
  /* 253402300800 is 10000-01-01T00:00:00Z. */
  char buf[LOG_TIMESTAMP_BUF];
  EXPECT_EQ(log_format_timestamp(253402300800, buf, sizeof(buf)),
            LOG_ERR_BAD_TIME);
}

TEST(FormatTimestamp, RejectsShortBuffer) {
  char buf[8];
  EXPECT_EQ(log_format_timestamp(0, buf, sizeof(buf)), LOG_ERR_BAD_TIME);
}

/* --- log_level_parse / log_level_str --- */

TEST(LevelParse, AcceptsAllFourNames) {
  LogLevel level;
  ASSERT_EQ(log_level_parse("debug", &level), LOG_OK);
  EXPECT_EQ(level, LOG_LEVEL_DEBUG);
  ASSERT_EQ(log_level_parse("info", &level), LOG_OK);
  EXPECT_EQ(level, LOG_LEVEL_INFO);
  ASSERT_EQ(log_level_parse("warning", &level), LOG_OK);
  EXPECT_EQ(level, LOG_LEVEL_WARNING);
  ASSERT_EQ(log_level_parse("error", &level), LOG_OK);
  EXPECT_EQ(level, LOG_LEVEL_ERROR);
}

TEST(LevelParse, RejectsUppercase) {
  LogLevel level;
  EXPECT_EQ(log_level_parse("INFO", &level), LOG_ERR_BAD_LEVEL);
}

TEST(LevelParse, RejectsUnknown) {
  LogLevel level;
  EXPECT_EQ(log_level_parse("warn", &level), LOG_ERR_BAD_LEVEL);
  EXPECT_EQ(log_level_parse("trace", &level), LOG_ERR_BAD_LEVEL);
}

TEST(LevelParse, RejectsEmpty) {
  LogLevel level;
  EXPECT_EQ(log_level_parse("", &level), LOG_ERR_BAD_LEVEL);
}

TEST(LevelStr, UppercaseLabels) {
  EXPECT_STREQ(log_level_str(LOG_LEVEL_DEBUG), "DEBUG");
  EXPECT_STREQ(log_level_str(LOG_LEVEL_INFO), "INFO");
  EXPECT_STREQ(log_level_str(LOG_LEVEL_WARNING), "WARNING");
  EXPECT_STREQ(log_level_str(LOG_LEVEL_ERROR), "ERROR");
}

/* --- log_unescape --- */

TEST(Unescape, PassesPlainText) {
  char *out = nullptr;
  ASSERT_EQ(log_unescape(" | ", &out), LOG_OK);
  EXPECT_EQ(take(out), " | ");
}

TEST(Unescape, TranslatesNewlineTabCarriageReturn) {
  char *out = nullptr;
  ASSERT_EQ(log_unescape("\\n\\t\\r", &out), LOG_OK);
  EXPECT_EQ(take(out), "\n\t\r");
}

TEST(Unescape, TranslatesDoubleBackslash) {
  char *out = nullptr;
  ASSERT_EQ(log_unescape("a\\\\b", &out), LOG_OK);
  EXPECT_EQ(take(out), "a\\b");
}

TEST(Unescape, RejectsUnknownEscape) {
  char *out = nullptr;
  EXPECT_EQ(log_unescape("\\q", &out), LOG_ERR_BAD_ESCAPE);
}

TEST(Unescape, RejectsTrailingBackslash) {
  char *out = nullptr;
  EXPECT_EQ(log_unescape("ab\\", &out), LOG_ERR_BAD_ESCAPE);
}

TEST(Unescape, HandlesEmptyString) {
  char *out = nullptr;
  ASSERT_EQ(log_unescape("", &out), LOG_OK);
  EXPECT_EQ(take(out), "");
}

/* --- log_clock_resolve --- */

TEST(ClockResolve, UsesRealNowWhenUnset) {
  time_t when = 0;
  ASSERT_EQ(log_clock_resolve(nullptr, 12345, &when), LOG_OK);
  EXPECT_EQ(when, 12345);
}

TEST(ClockResolve, ParsesFakeSeconds) {
  time_t when = 0;
  ASSERT_EQ(log_clock_resolve("1751328000", 99, &when), LOG_OK);
  EXPECT_EQ(when, 1751328000);
}

TEST(ClockResolve, ParsesNegativeFakeSeconds) {
  time_t when = 0;
  ASSERT_EQ(log_clock_resolve("-1", 99, &when), LOG_OK);
  EXPECT_EQ(when, -1);
}

TEST(ClockResolve, RejectsTrailingJunk) {
  time_t when = 0;
  EXPECT_EQ(log_clock_resolve("12x", 99, &when), LOG_ERR_BAD_TIME);
}

TEST(ClockResolve, RejectsEmptyValue) {
  time_t when = 0;
  EXPECT_EQ(log_clock_resolve("", 99, &when), LOG_ERR_BAD_TIME);
}

TEST(ClockResolve, RejectsNonNumeric) {
  time_t when = 0;
  EXPECT_EQ(log_clock_resolve("nope", 99, &when), LOG_ERR_BAD_TIME);
}

TEST(ClockResolve, RejectsLeadingWhitespaceAndPlusSign) {
  /* strtoll accepts both; Rust's parse accepts '+'. The three ports agree only
   * because all of them refuse both spellings. */
  time_t when = 0;
  EXPECT_EQ(log_clock_resolve(" 5", 99, &when), LOG_ERR_BAD_TIME);
  EXPECT_EQ(log_clock_resolve("+5", 99, &when), LOG_ERR_BAD_TIME);
}

/* --- log_write_entry --- */

TEST(WriteEntry, DefaultFormat) {
  LogFormat fmt = default_format();
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] hello\n");
}

TEST(WriteEntry, EachLevelLabel) {
  const LogLevel levels[] = {LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARNING,
                             LOG_LEVEL_ERROR};
  const char *expected[] = {"[TS] [DEBUG] hello\n", "[TS] [INFO] hello\n",
                            "[TS] [WARNING] hello\n", "[TS] [ERROR] hello\n"};
  for (size_t i = 0; i < 4; i++) {
    LogFormat fmt = default_format();
    fmt.level = levels[i];
    std::string out = captured([&](FILE *f) {
      EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
    });
    EXPECT_EQ(out, expected[i]);
  }
}

TEST(WriteEntry, CustomDelimiter) {
  LogFormat fmt = default_format();
  fmt.delimiter = " | ";
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] | [INFO] | hello\n");
}

TEST(WriteEntry, CustomSeparator) {
  LogFormat fmt = default_format();
  fmt.separator = "\n\n";
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] hello\n\n");
}

TEST(WriteEntry, WithoutTimestamp) {
  LogFormat fmt = default_format();
  fmt.show_timestamp = 0;
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  });
  EXPECT_EQ(out, "[INFO] hello\n");
}

TEST(WriteEntry, WithoutLevel) {
  LogFormat fmt = default_format();
  fmt.show_level = 0;
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] hello\n");
}

TEST(WriteEntry, WithoutTimestampOrLevel) {
  LogFormat fmt = default_format();
  fmt.show_timestamp = 0;
  fmt.show_level = 0;
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  });
  EXPECT_EQ(out, "hello\n");
}

TEST(WriteEntry, EmptyMessageKeepsBothFields) {
  LogFormat fmt = default_format();
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", "", 0), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] \n");
}

TEST(WriteEntry, MessageContainingNewlineIsVerbatim) {
  LogFormat fmt = default_format();
  const char *msg = "first\nsecond";
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", msg, strlen(msg)), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] first\nsecond\n");
}

TEST(WriteEntry, MessageWithEmbeddedNul) {
  LogFormat fmt = default_format();
  const std::string msg("a\0b", 3);
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", msg.data(), msg.size()), LOG_OK);
  });
  EXPECT_EQ(out, std::string("[TS] [INFO] a\0b\n", 16));
}

TEST(WriteEntry, NonAsciiBytesArePassedThrough) {
  LogFormat fmt = default_format();
  const char *msg = "h\xc3\xa9llo";
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_entry(f, &fmt, "TS", msg, strlen(msg)), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] h\xc3\xa9llo\n");
}

/* --- log_write_messages --- */

TEST(WriteMessages, WritesOneEntryPerMessage) {
  LogFormat fmt = default_format();
  const char *messages[] = {"one", "two", "three"};
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_messages(f, &fmt, "TS", messages, 3), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] one\n[TS] [INFO] two\n[TS] [INFO] three\n");
}

TEST(WriteMessages, AllEntriesShareOneTimestamp) {
  LogFormat fmt = default_format();
  const char *messages[] = {"a", "b"};
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_messages(f, &fmt, "SAME", messages, 2), LOG_OK);
  });
  EXPECT_EQ(out, "[SAME] [INFO] a\n[SAME] [INFO] b\n");
}

TEST(WriteMessages, ZeroMessagesWritesNothing) {
  LogFormat fmt = default_format();
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_messages(f, &fmt, "TS", nullptr, 0), LOG_OK);
  });
  EXPECT_EQ(out, "");
}

TEST(WriteMessages, AlwaysEndsWithTheSeparator) {
  LogFormat fmt = default_format();
  fmt.separator = "|";
  const char *messages[] = {"a", "b"};
  std::string out = captured([&](FILE *f) {
    EXPECT_EQ(log_write_messages(f, &fmt, "TS", messages, 2), LOG_OK);
  });
  EXPECT_EQ(out, "[TS] [INFO] a|[TS] [INFO] b|");
}

/* --- log_write_lines --- */

/* Feeds input through log_write_lines and returns what was written. */
static std::string lines_output(const std::string &input) {
  LogFormat fmt = default_format();
  FILE *in = make_input(input);
  std::string out = captured(
      [&](FILE *f) { EXPECT_EQ(log_write_lines(f, &fmt, "TS", in), LOG_OK); });
  fclose(in);
  return out;
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

/* --- log_open_append / log_close --- */

TEST(Append, CreatesFileWhenMissing) {
  const std::string path = tmp_path("creates.txt");
  remove(path.c_str());

  FILE *out = nullptr;
  ASSERT_EQ(log_open_append(path.c_str(), &out), LOG_OK);
  LogFormat fmt = default_format();
  EXPECT_EQ(log_write_entry(out, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  EXPECT_EQ(log_close(out), LOG_OK);

  EXPECT_EQ(read_file(path), "[TS] [INFO] hello\n");
}

TEST(Append, AppendsWithoutTruncating) {
  const std::string path = tmp_path("appends.txt");
  write_file(path, "existing\n");

  FILE *out = nullptr;
  ASSERT_EQ(log_open_append(path.c_str(), &out), LOG_OK);
  LogFormat fmt = default_format();
  EXPECT_EQ(log_write_entry(out, &fmt, "TS", kMsg, strlen(kMsg)), LOG_OK);
  EXPECT_EQ(log_close(out), LOG_OK);

  EXPECT_EQ(read_file(path), "existing\n[TS] [INFO] hello\n");
}

TEST(Append, CreatesFileEvenWithNoEntries) {
  const std::string path = tmp_path("empty.txt");
  remove(path.c_str());

  FILE *out = nullptr;
  ASSERT_EQ(log_open_append(path.c_str(), &out), LOG_OK);
  EXPECT_EQ(log_close(out), LOG_OK);

  EXPECT_EQ(read_file(path), "");
}

TEST(Append, OpenFailsWhenParentDirectoryIsMissing) {
  const std::string path = tmp_path("no_such_dir/log.txt");
  FILE *out = reinterpret_cast<FILE *>(1);
  EXPECT_EQ(log_open_append(path.c_str(), &out), LOG_ERR_OPEN);
  EXPECT_EQ(out, nullptr);
}

TEST(Append, OpenFailsOnADirectory) {
  const char *dir = getenv("TEST_TMPDIR");
  FILE *out = nullptr;
  EXPECT_EQ(log_open_append(dir ? dir : ".", &out), LOG_ERR_OPEN);
}

/* --- log_result_str --- */

TEST(ResultStr, NamesEveryStage) {
  const LogResult all[] = {
      LOG_OK,       LOG_ERR_OPEN,      LOG_ERR_WRITE,      LOG_ERR_CLOSE,
      LOG_ERR_READ, LOG_ERR_BAD_LEVEL, LOG_ERR_BAD_ESCAPE, LOG_ERR_BAD_TIME,
      LOG_ERR_NOMEM};
  for (LogResult r : all) {
    const char *label = log_result_str(r);
    EXPECT_NE(label, nullptr);
    EXPECT_STRNE(label, "unknown error");
  }
}

TEST(ResultStr, UnknownValueFallsBack) {
  EXPECT_STREQ(log_result_str(static_cast<LogResult>(999)), "unknown error");
}
