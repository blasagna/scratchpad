#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
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

// The three numeric options are bound to their real type and checked with
// CLI11's own CLI::Range, so the library owns the grammar as well as the range.
// CLI::Range is the right shape for this and CLI::PositiveNumber is not: the
// latter is a Range<double>, so --top-n 2.5 would clear the check and fail
// afterwards in the conversion, reporting the wrong kind of error.
//
// Handing over the grammar widens what this port accepts relative to C, whose
// strtol reads base 10 and stops at the first junk character, while CLI11 runs
// strtoull in base 0, strips '_' and '\'' group separators, and trims trailing
// whitespace. So `--top-n 0x10`, `1_000` and `"5 "` work here and are usage
// errors there, and `--top-n 010` even means eight here and ten there. It
// narrows nothing: CLI11 skips leading whitespace and honours a leading '+'
// just as strtol does, and rejects a leading '-' before ever reaching
// strtoull. The divergence is deliberate and tabulated in README.md; the golden
// tests cannot assert it, since each port renders the same committed goldens
// from the same flags.
const CLI::Validator kPositive{
    CLI::Range(1u, std::numeric_limits<unsigned int>::max(), "POSITIVE")};

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
  } else if (std::any_of(filenames.begin(), filenames.end(),
                         [&analyzer](const std::string &name) {
                           return !feed_named(analyzer, name);
                         })) {
    return 1;
  }

  const text_analyzer::TextStats stats = analyzer.finish();
  if (json) {
    text_analyzer::print_stats_json(std::cout, stats);
  } else {
    text_analyzer::print_stats(std::cout, stats);
  }

  return 0;
}
