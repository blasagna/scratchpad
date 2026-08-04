#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include "analyzer.hpp"

namespace {

using text_analyzer::AnalyzerConfig;

// Argument that means "read stdin", and the label used for it in errors.
constexpr std::string_view kStdinArg = "-";
constexpr std::string_view kStdinLabel = "<stdin>";

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

// A CLI11 validator over the spelling above, rather than CLI::PositiveNumber.
// CLI11's own numeric conversion skips leading whitespace, so --top-n " 5"
// would be accepted here and rejected by the C port, which hand-checks the same
// shape parse_positive does. The library owns the grammar; the accepted value
// set stays ours.
const CLI::Validator kPositive{[](const std::string &value) {
                                 return parse_positive(value)
                                            ? std::string{}
                                            : "expected a positive integer";
                               },
                               "POSITIVE", "positive integer"};

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
  // The name is passed explicitly because CLI11 otherwise takes argv[0], which
  // under `bazel run` is the full runfiles path.
  CLI::App app{
      "Reads text files and prints statistics:\n"
      "  - total line, blank line, word, character, digit, and punctuation "
      "counts\n"
      "  - word length distribution (mean, min, max, quartiles)\n"
      "  - top N most frequent words (case-insensitive)\n"
      "  - top N most frequent non-space characters\n"
      "\n"
      "Multiple files are analyzed as a single concatenated stream. Reads\n"
      "stdin when no file is given or when the file is '-'.\n"
      "\n"
      "Input is treated as ASCII bytes: characters are counted as bytes, not\n"
      "Unicode codepoints, and any non-ASCII byte separates words.",
      "text_analyzer"};

  AnalyzerConfig config;
  bool json = false;
  std::vector<std::string> filenames;

  app.add_option("--top-n", config.top_n, "number of top words/chars to report")
      ->check(kPositive)
      ->capture_default_str();
  app.add_option("--max-word-len", config.max_word_len,
                 "max characters per word before truncation")
      ->check(kPositive)
      ->capture_default_str();
  app.add_option("--word-table-cap", config.word_table_init_cap,
                 "initial word frequency table capacity")
      ->check(kPositive)
      ->capture_default_str();
  app.add_flag("--json", json, "print the summary as JSON instead of text");
  app.add_option("files", filenames, "files to analyze ('-' for stdin)");

  // CLI11 word-wraps the description by default, which strips the leading
  // spaces the bulleted list of statistics relies on. The prose here is already
  // laid out; print it verbatim.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  text_analyzer::Analyzer analyzer(config);
  if (filenames.empty()) {
    if (!feed_named(analyzer, kStdinArg))
      return 1;
  } else {
    for (const std::string &name : filenames) {
      if (!feed_named(analyzer, name))
        return 1;
    }
  }

  const text_analyzer::TextStats stats = analyzer.finish();
  if (json) {
    text_analyzer::print_stats_json(std::cout, stats);
  } else {
    text_analyzer::print_stats(std::cout, stats);
  }

  return 0;
}
