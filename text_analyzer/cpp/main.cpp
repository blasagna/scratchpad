#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer.hpp"

namespace {

using text_analyzer::AnalyzerConfig;

constexpr std::string_view kUsage =
    "usage: text_analyzer [options] [file...]\n"
    "       text_analyzer -h | --help\n";

// Argument that means "read stdin", and the label used for it in errors.
constexpr std::string_view kStdinArg = "-";
constexpr std::string_view kStdinLabel = "<stdin>";

// The integer options, each paired with the config field it sets. Listing them
// once keeps the "--flag N" and "--flag=N" paths in sync.
constexpr std::array<std::pair<std::string_view, unsigned int AnalyzerConfig::*>,
                     3>
    kIntFlags = {{
        {"--top-n", &AnalyzerConfig::top_n},
        {"--max-word-len", &AnalyzerConfig::max_word_len},
        {"--word-table-cap", &AnalyzerConfig::word_table_init_cap},
    }};

// Returns the config field an integer flag sets, or nullptr if name is not one.
unsigned int AnalyzerConfig::*int_flag_field(std::string_view name) {
  for (const auto &[flag, field] : kIntFlags) {
    if (flag == name)
      return field;
  }
  return nullptr;
}

void print_help() {
  std::cout << kUsage
            << "\n"
               "Reads text files and prints statistics:\n"
               "  - total line, blank line, word, character, digit, and "
               "punctuation counts\n"
               "  - word length distribution (mean, min, max, quartiles)\n"
               "  - top N most frequent words (case-insensitive)\n"
               "  - top N most frequent non-space characters\n"
               "\n"
               "Multiple files are analyzed as a single concatenated stream. "
               "Reads\n"
               "stdin when no file is given or when the file is '-'.\n"
               "\n"
               "Options:\n";
  std::cout
      << "  --top-n N           number of top words/chars to report (default: "
      << text_analyzer::kDefaultTopN << ")\n";
  std::cout << "  --max-word-len N    max characters per word before "
               "truncation (default: "
            << text_analyzer::kDefaultMaxWordLen << ")\n";
  std::cout << "  --word-table-cap N  initial word frequency table capacity "
               "(default: "
            << text_analyzer::kDefaultWordTableCap << ")\n";
  std::cout
      << "  --json              print the summary as JSON instead of text\n"
         "  -h, --help          show this help\n";
}

void print_usage_error() { std::cerr << kUsage; }

// Parses an entire string_view as a positive integer. Returns nullopt on any
// junk, a leading sign, or a value of zero (the config fields must be >= 1).
std::optional<unsigned int> parse_positive(std::string_view sv) {
  unsigned int value = 0;
  const char *end = sv.data() + sv.size();
  auto [ptr, ec] = std::from_chars(sv.data(), end, value);
  if (ec != std::errc{} || ptr != end || value == 0)
    return std::nullopt;
  return value;
}

struct Options {
  AnalyzerConfig config;
  bool json = false;
  std::vector<std::string_view> filenames;
};

// Feeds one named input into analyzer, where "-" means stdin. Returns false
// after reporting the failure against the input's display name.
bool feed_named(text_analyzer::Analyzer &analyzer, std::string_view name) {
  if (name == kStdinArg) {
    analyzer.feed(std::cin);
    if (std::cin.bad()) {
      std::cerr << kStdinLabel << ": failed to read input\n";
      return false;
    }
    return true;
  }

  const std::string filename(name);
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    std::cerr << filename << ": cannot open file\n";
    return false;
  }
  analyzer.feed(in);
  if (in.bad()) {
    std::cerr << filename << ": failed to read input\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char *argv[]) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  Options opts;

  for (std::size_t i = 1; i < args.size(); i++) {
    const std::string_view arg = args[i];

    if (arg == "-h" || arg == "--help") {
      print_help();
      return 0;
    }
    if (arg == "--json") {
      opts.json = true;
      continue;
    }

    // Integer options, accepting both "--flag N" and "--flag=N".
    if (auto field = int_flag_field(arg); field != nullptr) {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing value for " << arg << "\n";
        print_usage_error();
        return 1;
      }
      const std::string_view value = args[++i];
      const std::optional<unsigned int> n = parse_positive(value);
      if (!n) {
        std::cerr << "error: invalid value '" << value << "' for " << arg
                  << " (expected a positive integer)\n";
        print_usage_error();
        return 1;
      }
      opts.config.*field = *n;
      continue;
    }
    if (const std::size_t eq = arg.find('=');
        eq != std::string_view::npos && arg.starts_with("--")) {
      const std::string_view name = arg.substr(0, eq);
      const std::string_view value = arg.substr(eq + 1);
      if (auto field = int_flag_field(name); field != nullptr) {
        const std::optional<unsigned int> n = parse_positive(value);
        if (!n) {
          std::cerr << "error: invalid value '" << value << "' for " << name
                    << " (expected a positive integer)\n";
          print_usage_error();
          return 1;
        }
        opts.config.*field = *n;
        continue;
      }
    }

    // "-" is a positional meaning stdin, so check it before rejecting options.
    if (arg != kStdinArg && arg.starts_with("-")) {
      std::cerr << "error: unknown option '" << arg << "'\n";
      print_usage_error();
      return 1;
    }

    opts.filenames.push_back(arg);
  }

  text_analyzer::Analyzer analyzer(opts.config);
  if (opts.filenames.empty()) {
    if (!feed_named(analyzer, kStdinArg))
      return 1;
  } else {
    for (const std::string_view name : opts.filenames) {
      if (!feed_named(analyzer, name))
        return 1;
    }
  }

  const text_analyzer::TextStats stats = analyzer.finish();
  if (opts.json) {
    text_analyzer::print_stats_json(std::cout, stats);
  } else {
    text_analyzer::print_stats(std::cout, stats);
  }

  return 0;
}
