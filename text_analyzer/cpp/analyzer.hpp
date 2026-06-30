#ifndef TEXT_ANALYZER_CPP_ANALYZER_HPP
#define TEXT_ANALYZER_CPP_ANALYZER_HPP

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace text_analyzer {

// Defaults for AnalyzerConfig, also surfaced to the CLI as flag defaults.
inline constexpr unsigned int kDefaultTopN = 5;
inline constexpr unsigned int kDefaultMaxWordLen = 256;
inline constexpr unsigned int kDefaultWordTableCap = 64;

// Printable ASCII range excluding space: '!' (33) through '~' (126). These are
// the only characters considered for the top-chars report.
inline constexpr char kPrintableAsciiMin = '!';
inline constexpr char kPrintableAsciiMax = '~';

struct WordFreq {
  std::string word;
  long count = 0;
};

struct CharFreq {
  char ch = 0;
  long count = 0;
};

// Runtime configuration for analyze(). Default-construct then override any
// field before calling analyze(). The fields are unsigned because they must be
// positive; callers are responsible for keeping them >= 1 (the CLI validates
// user-supplied values), so analyze() performs no further checking.
struct AnalyzerConfig {
  unsigned int max_word_len = kDefaultMaxWordLen; // chars kept per word
  unsigned int top_n = kDefaultTopN; // number of top words/chars to report
  unsigned int word_table_init_cap =
      kDefaultWordTableCap; // reserve hint for the word table
};

struct TextStats {
  long line_count = 0;
  long word_count = 0;
  long char_count = 0;
  std::vector<WordFreq> top_words; // sorted descending by count
  std::vector<CharFreq> top_chars; // sorted descending by count
};

// analyze - reads all bytes from in and returns the computed statistics.
//
// Input:  in     - a readable stream positioned at the start of the content.
//                  The caller retains ownership.
//         config - runtime options; defaults are used when omitted.
//
// Output: A TextStats with line/word/character totals plus top_words and
//         top_chars (up to config.top_n entries each, sorted descending by
//         count, ties broken ascending by word/char).
//
// Note:   config fields must be positive; passing zero yields degenerate but
//         well-defined results (e.g. no words kept). The CLI validates user
//         input.
TextStats analyze(std::istream &in, const AnalyzerConfig &config = {});

// print_stats - writes a formatted summary of stats to os:
// lines/words/characters totals followed by ranked lists of the top words and
// top characters, each with its count and a percentage of the relevant total.
void print_stats(std::ostream &os, const TextStats &stats);

// print_stats_json - writes stats to os as a single JSON object with line/word/
// character totals plus top_words and top_characters arrays. Each entry carries
// its count and a frequency expressed as a ratio in [0, 1].
void print_stats_json(std::ostream &os, const TextStats &stats);

} // namespace text_analyzer

#endif // TEXT_ANALYZER_CPP_ANALYZER_HPP
