#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>

#include "analyzer.hpp"

namespace {

using text_analyzer::AnalyzerConfig;

constexpr std::string_view kUsage = "usage: text_analyzer [options] <file>\n"
                                    "       text_analyzer -h | --help\n";

void print_help() {
  std::cout << kUsage
            << "\n"
               "Reads a text file and prints statistics:\n"
               "  - total line, word, and character counts\n"
               "  - top N most frequent words (case-insensitive)\n"
               "  - top N most frequent non-space characters\n"
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
  std::string_view filename;
};

} // namespace

int main(int argc, char *argv[]) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  Options opts;
  bool have_file = false;

  // Maps a "--flag" that expects an integer to the config field it sets.
  auto set_int_flag = [&opts](std::string_view name, unsigned int value) {
    if (name == "--top-n")
      opts.config.top_n = value;
    else if (name == "--max-word-len")
      opts.config.max_word_len = value;
    else if (name == "--word-table-cap")
      opts.config.word_table_init_cap = value;
  };

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
    if (arg == "--top-n" || arg == "--max-word-len" ||
        arg == "--word-table-cap") {
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
      set_int_flag(arg, *n);
      continue;
    }
    if (const std::size_t eq = arg.find('=');
        eq != std::string_view::npos && arg.starts_with("--")) {
      const std::string_view name = arg.substr(0, eq);
      const std::string_view value = arg.substr(eq + 1);
      if (name == "--top-n" || name == "--max-word-len" ||
          name == "--word-table-cap") {
        const std::optional<unsigned int> n = parse_positive(value);
        if (!n) {
          std::cerr << "error: invalid value '" << value << "' for " << name
                    << " (expected a positive integer)\n";
          print_usage_error();
          return 1;
        }
        set_int_flag(name, *n);
        continue;
      }
    }

    if (arg.starts_with("-")) {
      std::cerr << "error: unknown option '" << arg << "'\n";
      print_usage_error();
      return 1;
    }

    // Positional argument: the input file.
    if (have_file) {
      std::cerr << "error: unexpected extra argument '" << arg << "'\n";
      print_usage_error();
      return 1;
    }
    opts.filename = arg;
    have_file = true;
  }

  if (!have_file) {
    std::cerr << "error: no file specified\n";
    print_usage_error();
    return 1;
  }

  const std::string filename(opts.filename);
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    std::cerr << filename << ": cannot open file\n";
    return 1;
  }

  const text_analyzer::TextStats stats =
      text_analyzer::analyze(in, opts.config);
  if (opts.json) {
    text_analyzer::print_stats_json(std::cout, stats);
  } else {
    text_analyzer::print_stats(std::cout, stats);
  }

  return 0;
}
