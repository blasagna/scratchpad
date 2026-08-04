#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include "logger.hpp"

namespace {

// The default separator as the user would type it, for the help text.
constexpr std::string_view kDefaultSeparatorEscaped = "\\n";

// Exit codes for what this program reports itself: 2 is the user's mistake, 1
// is an operation that failed. Argument errors come from CLI11 and carry its
// codes instead.
constexpr int kExitUsage = 2;
constexpr int kExitFailure = 1;

// Expands a delimiter or separator in place. The accepted escape set stays
// logger::unescape's -- exactly \n, \t, \r, and \\ -- rather than
// CLI::EscapedString's, which also takes \xNN, \uNNNN, and octal and would
// drift away from the C and Rust ports.
const CLI::Validator kUnescape{
    [](std::string &value) -> std::string {
      const std::optional<std::string> text = logger::unescape(value);
      if (!text)
        return "only \\n, \\t, \\r, and \\\\ are recognized";
      value = *text;
      return {};
    },
    "STR", "escape"};

// The four level spellings, checked through logger::parse_level so the accepted
// set has one definition. CLI::CheckedTransformer would be the obvious fit and
// is wrong here: mapping onto an enum, it also accepts the underlying numbers,
// so `--level 3` would mean "error" in this port and be a usage error in the
// other two.
const CLI::Validator kLevel{
    [](const std::string &value) -> std::string {
      return logger::parse_level(value)
                 ? std::string{}
                 : "expected debug, info, warning, or error";
    },
    "LEVEL", "level"};

// Reports the failing stage against the log file, or against stdin for a read
// error, in the same shape as the C port.
void report(const logger::LogResult &result, std::string_view path) {
  std::cerr << "simple_logger: " << logger::describe(result.stage);
  if (result.stage == logger::LogStage::kRead)
    std::cerr << ": <stdin>";
  else
    std::cerr << ": " << path;
  if (result.ec)
    std::cerr << ": " << result.ec.message();
  std::cerr << "\n";
}

} // namespace

int main(int argc, char *argv[]) {
  // The name is passed explicitly because CLI11 otherwise takes argv[0], which
  // under `bazel run` is the full runfiles path.
  CLI::App app{
      "Appends timestamped messages to a log file. Each message argument "
      "becomes\n"
      "one entry; with no message arguments, one entry is read per line from\n"
      "stdin. The log file is opened for append, so previous entries are "
      "kept.\n"
      "\n"
      "Each entry is written as:\n"
      "  [<timestamp>]<delim>[<LEVEL>]<delim><message><separator>\n"
      "\n"
      "The timestamp is UTC ISO 8601 (e.g. [2026-07-30T18:22:05Z]) and is "
      "read\n"
      "once per run, so every entry one run writes shares it.",
      "simple_logger"};

  logger::Format fmt;
  std::vector<std::string> positionals;
  std::string level_name{"info"};

  app.add_option("-l,--level", level_name, "debug, info, warning, or error")
      ->check(kLevel)
      ->capture_default_str();
  app.add_option("-d,--delimiter", fmt.delimiter, "text between fields")
      ->transform(kUnescape)
      ->default_str(std::string(logger::kDefaultDelimiter));
  app.add_option("-s,--separator", fmt.separator, "text after each entry")
      ->transform(kUnescape)
      ->default_str(std::string(kDefaultSeparatorEscaped));
  app.add_flag("!--no-timestamp", fmt.show_timestamp,
               "omit the [timestamp] field");
  app.add_flag("!--no-level", fmt.show_level, "omit the [LEVEL] field");
  app.add_option("logfile", positionals,
                 "log file to append to, then the messages to write");

  app.footer("STR values accept the escapes \\n, \\t, \\r, and \\\\; any other "
             "backslash\n"
             "escape is an error. Use -- before a message that begins with "
             "'-'.\n"
             "\n"
             "Environment:\n"
             "  " +
             std::string(logger::kFakeTimeVar) +
             "  epoch seconds to use instead of the real\n"
             "                           clock; used by the cross-port parity "
             "script.");

  // CLI11 word-wraps the description and footer by default, which strips the
  // leading spaces the entry-format line and the Environment block rely on.
  // The prose here is already laid out; print it verbatim.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  // kLevel already rejected anything else, so this cannot fail.
  fmt.level = logger::parse_level(level_name).value();

  // The logfile and the messages share one positional list so that a message
  // is never mistaken for a second logfile; splitting them here keeps the
  // "missing <logfile>" and "must not be empty" diagnostics ours, at exit 2.
  if (positionals.empty()) {
    std::cerr << "error: missing <logfile>\n";
    return kExitUsage;
  }

  const std::string_view path = positionals.front();
  if (path.empty()) {
    std::cerr << "error: <logfile> must not be empty\n";
    return kExitUsage;
  }
  const std::vector<std::string_view> message_views(positionals.begin() + 1,
                                                    positionals.end());
  const std::span<const std::string_view> messages{message_views};

  // One clock reading for the whole run, so a slow stdin pipe cannot spread
  // one invocation's entries across several seconds.
  const std::optional<std::time_t> when = logger::clock_now();
  if (!when) {
    // A bad override is the user's mistake; a failing system clock is not.
    if (const char *fake =
            std::getenv(std::string(logger::kFakeTimeVar).c_str())) {
      std::cerr << "error: invalid " << logger::kFakeTimeVar << " value '"
                << fake << "' (expected epoch seconds)\n";
      return kExitUsage;
    }
    std::cerr << "simple_logger: "
              << logger::describe(logger::LogStage::kBadTime) << "\n";
    return kExitFailure;
  }

  const std::optional<std::string> timestamp = logger::format_timestamp(*when);
  if (!timestamp) {
    std::cerr << "simple_logger: "
              << logger::describe(logger::LogStage::kBadTime) << "\n";
    return kExitFailure;
  }

  logger::LogResult result =
      messages.empty()
          ? logger::append_lines(path, fmt, *timestamp, std::cin)
          : logger::append_messages(path, fmt, *timestamp, messages);

  // write_lines reports a read failure from badbit, which is correct for a
  // real file or string stream but cannot see one on std::cin: that is backed
  // by a stdio_sync_filebuf whose underflow() returns EOF on error without
  // setting badbit, so a failed read is indistinguishable from a clean end of
  // input at the iostream level. The FILE* underneath does record it, and it is
  // the same flag the C port's ferror(in) checks. Without this the program
  // would exit 0 on unreadable stdin, silently dropping the rest of the input.
  if (result && messages.empty() && std::ferror(stdin))
    result = {logger::LogStage::kRead,
              std::error_code(errno, std::generic_category())};

  if (!result) {
    report(result, path);
    return kExitFailure;
  }
  return 0;
}
