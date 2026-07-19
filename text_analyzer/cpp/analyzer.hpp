#ifndef TEXT_ANALYZER_CPP_ANALYZER_HPP
#define TEXT_ANALYZER_CPP_ANALYZER_HPP

#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace text_analyzer {

// Defaults for AnalyzerConfig, also surfaced to the CLI as flag defaults.
inline constexpr unsigned int kDefaultTopN = 5;
inline constexpr unsigned int kDefaultMaxWordLen = 256;
inline constexpr unsigned int kDefaultWordTableCap = 64;

// Note on text handling: input is processed as bytes, in the "C" locale (no
// port calls setlocale), so classification is ASCII-only. char_count counts
// bytes rather than Unicode codepoints, and a word is a maximal run of ASCII
// letters [A-Za-z]. Any non-ASCII byte counts as a character, separates words,
// and is neither a digit nor a punctuation mark.

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

// Distribution of word lengths, measured in alphabetic characters.
//
// Lengths are the true lengths of each word in the input, unaffected by
// AnalyzerConfig::max_word_len truncation of the stored spelling. The mean is
// not stored: derive it as sum / count, guarding against count == 0. All fields
// are zero when no words were seen.
struct WordLengthStats {
  long count = 0; // number of words measured; equals TextStats::word_count
  long sum = 0;   // total of all word lengths
  long min = 0;
  long max = 0;
  long p25 = 0; // 25th, 50th, and 75th percentile lengths by nearest rank
  long p50 = 0;
  long p75 = 0;

  friend bool operator==(const WordLengthStats &,
                         const WordLengthStats &) = default;
};

struct TextStats {
  long line_count = 0;
  long blank_line_count = 0; // lines containing no non-whitespace characters
  long word_count = 0;
  long char_count = 0;
  long digit_count = 0;
  long punct_count = 0;
  WordLengthStats word_length;
  std::vector<WordFreq> top_words; // sorted descending by count
  std::vector<CharFreq> top_chars; // sorted descending by count
};

// Accumulates statistics across one or more streams.
//
// Feed any number of streams with feed(), then call finish() to rank and return
// the totals. Scan state persists between feeds, so feeding two streams is
// equivalent to feeding their concatenation: a word or line split across the
// boundary is counted once, not twice.
class Analyzer {
public:
  explicit Analyzer(const AnalyzerConfig &config = {});
  ~Analyzer();
  Analyzer(Analyzer &&) noexcept;
  Analyzer &operator=(Analyzer &&) noexcept;

  // Reads all bytes from in. The caller retains ownership of the stream.
  void feed(std::istream &in);

  // Flushes any trailing word, then ranks and returns the totals. The analyzer
  // must not be fed again afterward.
  TextStats finish();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// analyze - reads all bytes from in and returns the computed statistics.
//
// A convenience wrapper over one Analyzer::feed followed by Analyzer::finish
// for the single-stream case.
//
// Input:  in     - a readable stream positioned at the start of the content.
//                  The caller retains ownership.
//         config - runtime options; defaults are used when omitted.
//
// Output: A TextStats with the counts and word length distribution plus
//         top_words and top_chars (up to config.top_n entries each, sorted
//         descending by count, ties broken ascending by word/char).
//
// Note:   config fields must be positive; passing zero yields degenerate but
//         well-defined results (e.g. no words kept). The CLI validates user
//         input.
TextStats analyze(std::istream &in, const AnalyzerConfig &config = {});

// print_stats - writes a formatted summary of stats to os: the counts and word
// length distribution followed by ranked lists of the top words and top
// characters, each with its count and a percentage of the relevant total.
void print_stats(std::ostream &os, const TextStats &stats);

// print_stats_json - writes stats to os as a single JSON object with the
// counts, a word_length object, and top_words and top_characters arrays. Each
// ranked entry carries its count and a frequency expressed as a ratio in [0, 1].
void print_stats_json(std::ostream &os, const TextStats &stats);

} // namespace text_analyzer

#endif // TEXT_ANALYZER_CPP_ANALYZER_HPP
