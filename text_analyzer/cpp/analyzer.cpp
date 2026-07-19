#include "analyzer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <format>
#include <iterator>
#include <unordered_map>

namespace text_analyzer {

namespace {

// Width of the label column in the text summary, sized for the longest label.
constexpr int kLabelWidth = 13;

// Printable ASCII range excluding space: '!' (33) through '~' (126). These are
// the only characters considered for the top-chars report.
constexpr char kPrintableAsciiMin = '!';
constexpr char kPrintableAsciiMax = '~';

// All possible byte values (0-255), used to size the character frequency table.
constexpr std::size_t kCharTableSize = UCHAR_MAX + 1;

// Buckets in the word length histogram, indexed by length. Lengths at or above
// the last index are clamped into it, so quantiles (but never the exactly
// tracked count/sum/min/max) lose resolution for words longer than
// kDefaultMaxWordLen characters.
constexpr std::size_t kLengthHistBuckets = kDefaultMaxWordLen + 1;

// Returns the length at the pct-th percentile by nearest rank, or 0 when there
// are no words. Integer arithmetic throughout so the three ports agree exactly.
long quantile(const std::array<long, kLengthHistBuckets> &hist, long count,
              long pct) {
  if (count <= 0)
    return 0;
  // 1-based rank of the target element: ceil(pct/100 * count), at least 1.
  const long rank = std::max(1L, (pct * count + 99) / 100);
  long cumulative = 0;
  for (std::size_t len = 0; len < hist.size(); len++) {
    cumulative += hist[len];
    if (cumulative >= rank)
      return static_cast<long>(len);
  }
  return 0;
}

// Returns the mean word length, or 0.0 when no words were seen.
double word_length_mean(const WordLengthStats &wl) {
  return wl.count > 0 ? static_cast<double>(wl.sum) /
                            static_cast<double>(wl.count)
                      : 0.0;
}

// Writes c as the contents of a JSON string, escaping the two characters that
// are otherwise illegal inside one. The char range used for top characters
// ('!'..'~') contains no control characters, so only '"' and '\\' can appear.
void write_json_char(std::ostream &os, char c) {
  if (c == '"' || c == '\\') {
    os << '\\';
  }
  os << c;
}

} // namespace

struct Analyzer::Impl {
  explicit Impl(const AnalyzerConfig &config) : config_(config) {
    word_counts_.reserve(config_.word_table_init_cap);
  }

  void feed(std::istream &in);
  TextStats finish();
  void flush_word();

  AnalyzerConfig config_;
  TextStats stats_;
  std::array<long, kCharTableSize> char_counts_{};
  std::unordered_map<std::string, long> word_counts_;
  std::array<long, kLengthHistBuckets> length_hist_{};
  long length_sum_ = 0;
  long length_min_ = 0;
  long length_max_ = 0;
  std::string word_;      // spelling in progress, truncated
  long cur_word_len_ = 0; // true length of the word in progress
  bool in_word_ = false;
  bool line_has_content_ = false;
};

Analyzer::Analyzer(const AnalyzerConfig &config)
    : impl_(std::make_unique<Impl>(config)) {}

// Defined here, where Impl is complete, so unique_ptr can instantiate its
// deleter.
Analyzer::~Analyzer() = default;
Analyzer::Analyzer(Analyzer &&) noexcept = default;
Analyzer &Analyzer::operator=(Analyzer &&) noexcept = default;

void Analyzer::feed(std::istream &in) { impl_->feed(in); }
TextStats Analyzer::finish() { return impl_->finish(); }

void Analyzer::Impl::feed(std::istream &in) {
  char c;
  while (in.get(c)) {
    const auto uc = static_cast<unsigned char>(c);
    char_counts_[uc]++;
    stats_.char_count++;

    if (c == '\n') {
      stats_.line_count++;
      if (!line_has_content_)
        stats_.blank_line_count++;
      line_has_content_ = false;
    } else if (!std::isspace(uc)) {
      line_has_content_ = true;
    }

    if (std::isalpha(uc)) {
      // Keep at most max_word_len - 1 chars, matching the C buffer, but measure
      // the untruncated length.
      if (std::ssize(word_) <
          static_cast<std::ptrdiff_t>(config_.max_word_len) - 1) {
        word_.push_back(static_cast<char>(std::tolower(uc)));
      }
      cur_word_len_++;
      in_word_ = true;
    } else if (in_word_) {
      flush_word();
    }
  }
}

// Records the accumulated word and its length, then resets word state.
void Analyzer::Impl::flush_word() {
  word_counts_[word_]++;
  stats_.word_count++;

  const long len = cur_word_len_;
  length_sum_ += len;
  if (length_max_ == 0 || len > length_max_)
    length_max_ = len;
  if (length_min_ == 0 || len < length_min_)
    length_min_ = len;
  const auto bucket = std::min(static_cast<std::size_t>(len),
                               length_hist_.size() - 1);
  length_hist_[bucket]++;

  word_.clear();
  cur_word_len_ = 0;
  in_word_ = false;
}

TextStats Analyzer::Impl::finish() {
  if (in_word_)
    flush_word();

  for (std::size_t i = 0; i < char_counts_.size(); i++) {
    const auto uc = static_cast<unsigned char>(i);
    if (std::isdigit(uc))
      stats_.digit_count += char_counts_[i];
    else if (std::ispunct(uc))
      stats_.punct_count += char_counts_[i];
  }

  stats_.word_length = {
      .count = stats_.word_count,
      .sum = length_sum_,
      .min = length_min_,
      .max = length_max_,
      .p25 = quantile(length_hist_, stats_.word_count, 25),
      .p50 = quantile(length_hist_, stats_.word_count, 50),
      .p75 = quantile(length_hist_, stats_.word_count, 75),
  };

  // Top words: sort by count descending, ties broken by word ascending.
  stats_.top_words.reserve(word_counts_.size());
  for (auto &[w, count] : word_counts_) {
    stats_.top_words.push_back({w, count});
  }
  std::ranges::sort(stats_.top_words, [](const WordFreq &a, const WordFreq &b) {
    if (a.count != b.count)
      return a.count > b.count;
    return a.word < b.word;
  });
  if (stats_.top_words.size() > config_.top_n) {
    stats_.top_words.resize(config_.top_n);
  }

  // Top chars: only printable ASCII with a nonzero count, sorted by count
  // descending, ties broken by char ascending (lowest code point first).
  for (char ch = kPrintableAsciiMin; ch <= kPrintableAsciiMax; ch++) {
    const long count = char_counts_[static_cast<unsigned char>(ch)];
    if (count > 0)
      stats_.top_chars.push_back({ch, count});
  }
  std::ranges::sort(stats_.top_chars, [](const CharFreq &a, const CharFreq &b) {
    if (a.count != b.count)
      return a.count > b.count;
    return a.ch < b.ch;
  });
  if (stats_.top_chars.size() > config_.top_n) {
    stats_.top_chars.resize(config_.top_n);
  }

  return std::move(stats_);
}

TextStats analyze(std::istream &in, const AnalyzerConfig &config) {
  Analyzer analyzer(config);
  analyzer.feed(in);
  return analyzer.finish();
}

void print_stats(std::ostream &os, const TextStats &stats) {
  const auto row = [&os](std::string_view label, long value) {
    os << std::format("{:<{}} {}\n", label, kLabelWidth, value);
  };
  row("Lines:", stats.line_count);
  row("Blank lines:", stats.blank_line_count);
  row("Words:", stats.word_count);
  row("Characters:", stats.char_count);
  row("Digits:", stats.digit_count);
  row("Punctuation:", stats.punct_count);

  const WordLengthStats &wl = stats.word_length;
  os << std::format(
      "\n{:<{}} mean {:.1f}, min {}, max {}, p25 {}, p50 {}, p75 {}\n",
      "Word length:", kLabelWidth, word_length_mean(wl), wl.min, wl.max, wl.p25,
      wl.p50, wl.p75);

  os << "\nTop words:\n";
  for (std::size_t i = 0; i < stats.top_words.size(); i++) {
    const double pct = stats.word_count > 0
                           ? 100.0 *
                                 static_cast<double>(stats.top_words[i].count) /
                                 static_cast<double>(stats.word_count)
                           : 0.0;
    os << std::format("  {}. {} ({}, {:.1f}%)\n", i + 1,
                      stats.top_words[i].word, stats.top_words[i].count, pct);
  }

  os << "\nTop characters:\n";
  for (std::size_t i = 0; i < stats.top_chars.size(); i++) {
    const double pct = stats.char_count > 0
                           ? 100.0 *
                                 static_cast<double>(stats.top_chars[i].count) /
                                 static_cast<double>(stats.char_count)
                           : 0.0;
    os << std::format("  {}. '{}' ({}, {:.1f}%)\n", i + 1,
                      stats.top_chars[i].ch, stats.top_chars[i].count, pct);
  }
}

void print_stats_json(std::ostream &os, const TextStats &stats) {
  const WordLengthStats &wl = stats.word_length;

  os << "{\n";
  os << std::format("  \"lines\": {},\n", stats.line_count);
  os << std::format("  \"blank_lines\": {},\n", stats.blank_line_count);
  os << std::format("  \"words\": {},\n", stats.word_count);
  os << std::format("  \"characters\": {},\n", stats.char_count);
  os << std::format("  \"digits\": {},\n", stats.digit_count);
  os << std::format("  \"punctuation\": {},\n", stats.punct_count);

  os << "  \"word_length\": {\n";
  os << std::format("    \"mean\": {:.4f},\n", word_length_mean(wl));
  os << std::format("    \"min\": {},\n", wl.min);
  os << std::format("    \"max\": {},\n", wl.max);
  os << std::format("    \"p25\": {},\n", wl.p25);
  os << std::format("    \"p50\": {},\n", wl.p50);
  os << std::format("    \"p75\": {}\n", wl.p75);
  os << "  },\n";

  os << "  \"top_words\": [";
  for (std::size_t i = 0; i < stats.top_words.size(); i++) {
    const double freq = stats.word_count > 0
                            ? static_cast<double>(stats.top_words[i].count) /
                                  static_cast<double>(stats.word_count)
                            : 0.0;
    os << (i == 0 ? "" : ",");
    // Words are ASCII letters only; no escaping needed.
    os << std::format("\n    {{\n"
                      "      \"word\": \"{}\",\n"
                      "      \"count\": {},\n"
                      "      \"frequency\": {:.4f}\n"
                      "    }}",
                      stats.top_words[i].word, stats.top_words[i].count, freq);
  }
  os << (stats.top_words.empty() ? "" : "\n  ") << "],\n";

  os << "  \"top_characters\": [";
  for (std::size_t i = 0; i < stats.top_chars.size(); i++) {
    const double freq = stats.char_count > 0
                            ? static_cast<double>(stats.top_chars[i].count) /
                                  static_cast<double>(stats.char_count)
                            : 0.0;
    os << (i == 0 ? "" : ",");
    os << "\n    {\n      \"char\": \"";
    write_json_char(os, stats.top_chars[i].ch);
    os << std::format("\",\n"
                      "      \"count\": {},\n"
                      "      \"frequency\": {:.4f}\n"
                      "    }}",
                      stats.top_chars[i].count, freq);
  }
  os << (stats.top_chars.empty() ? "" : "\n  ") << "]\n";

  os << "}\n";
}

} // namespace text_analyzer
