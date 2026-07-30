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

#include "logger.hpp"

namespace {

// The default separator as the user would type it, for the help text.
constexpr std::string_view kDefaultSeparatorEscaped = "\\n";

// Exit codes, matching the C and Rust ports: 2 is the user's mistake, 1 is an
// operation that failed.
constexpr int kExitUsage = 2;
constexpr int kExitFailure = 1;

void print_help() {
  std::cout
      << "usage: simple_logger [options] <logfile> [message...]\n"
         "       simple_logger -h | --help\n"
         "\n"
         "Appends timestamped messages to a log file. Each message argument "
         "becomes\n"
         "one entry; with no message arguments, one entry is read per line "
         "from\n"
         "stdin. The log file is opened for append, so previous entries are "
         "kept.\n"
         "\n"
         "Each entry is written as:\n"
         "  [<timestamp>]<delim>[<LEVEL>]<delim><message><separator>\n"
         "\n"
         "The timestamp is UTC ISO 8601 (e.g. [2026-07-30T18:22:05Z]) and is "
         "read\n"
         "once per run, so every entry one run writes shares it.\n"
         "\n"
         "Options:\n"
         "  -l, --level LEVEL    debug, info, warning, or error (default: "
         "info)\n"
         "  -d, --delimiter STR  text between fields (default: \""
      << logger::kDefaultDelimiter
      << "\")\n"
         "  -s, --separator STR  text after each entry (default: \""
      << kDefaultSeparatorEscaped
      << "\")\n"
         "      --no-timestamp   omit the [timestamp] field\n"
         "      --no-level       omit the [LEVEL] field\n"
         "  -h, --help           show this help\n"
         "\n"
         "STR values accept the escapes \\n, \\t, \\r, and \\\\; any other "
         "backslash\n"
         "escape is an error. Use -- before a message that begins with '-'.\n"
         "\n"
         "Environment:\n"
         "  "
      << logger::kFakeTimeVar
      << "  epoch seconds to use instead of the real\n"
         "                           clock; used by the cross-port parity "
         "script.\n";
}

void print_usage_error() {
  std::cerr << "usage: simple_logger [options] <logfile> [message...]\n"
               "       simple_logger --help\n";
}

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
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));

  logger::Format fmt;
  std::vector<std::string_view> positionals;

  // A hand-rolled loop rather than getopt_long, but it permutes the same way:
  // an option is recognized wherever it appears, so `simple_logger log.txt
  // --level error msg` works in every port. "--" ends option parsing, which is
  // how a message beginning with '-' gets through.
  bool options_done = false;
  for (std::size_t i = 1; i < args.size(); i++) {
    const std::string_view arg = args[i];

    if (!options_done && arg == "--") {
      options_done = true;
      continue;
    }
    // A lone "-" is an ordinary argument, not an option.
    if (options_done || arg.size() < 2 || arg[0] != '-') {
      positionals.push_back(arg);
      continue;
    }

    const bool takes_value = arg == "-l" || arg == "--level" || arg == "-d" ||
                             arg == "--delimiter" || arg == "-s" ||
                             arg == "--separator";
    std::string_view value;
    if (takes_value) {
      if (i + 1 >= args.size()) {
        std::cerr << "error: option '" << arg << "' requires a value\n";
        print_usage_error();
        return kExitUsage;
      }
      value = args[++i];
    }

    if (arg == "-h" || arg == "--help") {
      print_help();
      return 0;
    } else if (arg == "-l" || arg == "--level") {
      const std::optional<logger::Level> level = logger::parse_level(value);
      if (!level) {
        std::cerr << "error: invalid value '" << value
                  << "' for --level (expected debug, info, warning, or "
                     "error)\n";
        return kExitUsage;
      }
      fmt.level = *level;
    } else if (arg == "-d" || arg == "--delimiter" || arg == "-s" ||
               arg == "--separator") {
      const bool is_delimiter = arg == "-d" || arg == "--delimiter";
      const std::optional<std::string> text = logger::unescape(value);
      if (!text) {
        std::cerr << "error: invalid value '" << value << "' for "
                  << (is_delimiter ? "--delimiter" : "--separator")
                  << " (only \\n, \\t, \\r, and \\\\ are recognized)\n";
        return kExitUsage;
      }
      (is_delimiter ? fmt.delimiter : fmt.separator) = *text;
    } else if (arg == "--no-timestamp") {
      fmt.show_timestamp = false;
    } else if (arg == "--no-level") {
      fmt.show_level = false;
    } else {
      std::cerr << "error: unknown option '" << arg << "'\n";
      print_usage_error();
      return kExitUsage;
    }
  }

  if (positionals.empty()) {
    std::cerr << "error: missing <logfile>\n";
    print_usage_error();
    return kExitUsage;
  }

  const std::string_view path = positionals.front();
  if (path.empty()) {
    std::cerr << "error: <logfile> must not be empty\n";
    return kExitUsage;
  }
  const std::span<const std::string_view> messages =
      std::span(positionals).subspan(1);

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
