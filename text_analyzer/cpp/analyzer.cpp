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

// All possible byte values (0-255), used to size the character frequency table.
constexpr int kCharTableSize = UCHAR_MAX + 1;

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

TextStats analyze(std::istream &in, const AnalyzerConfig &config) {
  TextStats stats;

  std::array<long, kCharTableSize> char_counts{};
  std::unordered_map<std::string, long> word_counts;
  word_counts.reserve(config.word_table_init_cap);

  std::string word;
  bool in_word = false;

  char c;
  while (in.get(c)) {
    const auto uc = static_cast<unsigned char>(c);
    char_counts[uc]++;
    stats.char_count++;

    if (c == '\n')
      stats.line_count++;

    if (std::isalpha(uc)) {
      // Keep at most max_word_len - 1 chars, matching the C buffer.
      if (std::ssize(word) <
          static_cast<std::ptrdiff_t>(config.max_word_len) - 1) {
        word.push_back(static_cast<char>(std::tolower(uc)));
      }
      in_word = true;
    } else if (in_word) {
      word_counts[word]++;
      stats.word_count++;
      word.clear();
      in_word = false;
    }
  }
  if (in_word) {
    word_counts[word]++;
    stats.word_count++;
  }

  // Top words: sort by count descending, ties broken by word ascending.
  stats.top_words.reserve(word_counts.size());
  for (auto &[w, count] : word_counts) {
    stats.top_words.push_back({w, count});
  }
  std::ranges::sort(stats.top_words, [](const WordFreq &a, const WordFreq &b) {
    if (a.count != b.count)
      return a.count > b.count;
    return a.word < b.word;
  });
  if (stats.top_words.size() > config.top_n) {
    stats.top_words.resize(config.top_n);
  }

  // Top chars: only printable ASCII with a nonzero count, sorted by count
  // descending, ties broken by char ascending (lowest code point first).
  for (char ch = kPrintableAsciiMin; ch <= kPrintableAsciiMax; ch++) {
    const long count = char_counts[static_cast<unsigned char>(ch)];
    if (count > 0)
      stats.top_chars.push_back({ch, count});
  }
  std::ranges::sort(stats.top_chars, [](const CharFreq &a, const CharFreq &b) {
    if (a.count != b.count)
      return a.count > b.count;
    return a.ch < b.ch;
  });
  if (stats.top_chars.size() > config.top_n) {
    stats.top_chars.resize(config.top_n);
  }

  return stats;
}

void print_stats(std::ostream &os, const TextStats &stats) {
  os << std::format("Lines:      {}\n", stats.line_count);
  os << std::format("Words:      {}\n", stats.word_count);
  os << std::format("Characters: {}\n", stats.char_count);

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
  os << "{\n";
  os << std::format("  \"lines\": {},\n", stats.line_count);
  os << std::format("  \"words\": {},\n", stats.word_count);
  os << std::format("  \"characters\": {},\n", stats.char_count);

  os << "  \"top_words\": [";
  for (std::size_t i = 0; i < stats.top_words.size(); i++) {
    const double freq = stats.word_count > 0
                            ? static_cast<double>(stats.top_words[i].count) /
                                  static_cast<double>(stats.word_count)
                            : 0.0;
    os << (i == 0 ? "" : ",");
    // Words are alpha-only; no escaping needed.
    os << std::format(
        "\n    {{\"word\": \"{}\", \"count\": {}, \"frequency\": {:.4f}}}",
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
    os << "\n    {\"char\": \"";
    write_json_char(os, stats.top_chars[i].ch);
    os << std::format("\", \"count\": {}, \"frequency\": {:.4f}}}",
                      stats.top_chars[i].count, freq);
  }
  os << (stats.top_chars.empty() ? "" : "\n  ") << "]\n";

  os << "}\n";
}

} // namespace text_analyzer
