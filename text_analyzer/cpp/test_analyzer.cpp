#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <string_view>

#include "analyzer.hpp"

namespace {

using text_analyzer::analyze;
using text_analyzer::AnalyzerConfig;
using text_analyzer::TextStats;

TextStats analyze_str(std::string_view text,
                      const AnalyzerConfig &config = {}) {
  std::istringstream in{std::string(text)};
  return analyze(in, config);
}

} // namespace

TEST(AnalyzerTest, EmptyFile) {
  const TextStats stats = analyze_str("");
  EXPECT_EQ(stats.line_count, 0);
  EXPECT_EQ(stats.word_count, 0);
  EXPECT_EQ(stats.char_count, 0);
  EXPECT_TRUE(stats.top_words.empty());
  EXPECT_TRUE(stats.top_chars.empty());
}

TEST(AnalyzerTest, SingleLine) {
  const TextStats stats = analyze_str("hello world\n");
  EXPECT_EQ(stats.line_count, 1);
  EXPECT_EQ(stats.word_count, 2);
  EXPECT_EQ(stats.char_count, 12);
}

TEST(AnalyzerTest, MultiLine) {
  const TextStats stats = analyze_str("one\ntwo\nthree\n");
  EXPECT_EQ(stats.line_count, 3);
  EXPECT_EQ(stats.word_count, 3);
}

TEST(AnalyzerTest, WordFrequency) {
  const TextStats stats = analyze_str("the cat sat the cat the\n");
  ASSERT_GE(stats.top_words.size(), 2u);
  EXPECT_EQ(stats.top_words[0].word, "the");
  EXPECT_EQ(stats.top_words[0].count, 3);
  EXPECT_EQ(stats.top_words[1].word, "cat");
  EXPECT_EQ(stats.top_words[1].count, 2);
}

TEST(AnalyzerTest, WordNormalization) {
  const TextStats stats = analyze_str("The the THE tHe\n");
  EXPECT_EQ(stats.word_count, 4);
  ASSERT_GE(stats.top_words.size(), 1u);
  EXPECT_EQ(stats.top_words[0].word, "the");
  EXPECT_EQ(stats.top_words[0].count, 4);
}

TEST(AnalyzerTest, CharFrequency) {
  const TextStats stats = analyze_str("aaabbc\n");
  ASSERT_GE(stats.top_chars.size(), 1u);
  EXPECT_EQ(stats.top_chars[0].ch, 'a');
  EXPECT_EQ(stats.top_chars[0].count, 3);
}

TEST(AnalyzerTest, TrailingWordNoNewline) {
  const TextStats stats = analyze_str("hello world");
  EXPECT_EQ(stats.line_count, 0);
  EXPECT_EQ(stats.word_count, 2);
  EXPECT_EQ(stats.char_count, 11);
}

TEST(AnalyzerTest, ConfigTopN) {
  AnalyzerConfig config;
  config.top_n = 2;
  const TextStats stats = analyze_str("a b c d e f\n", config);
  EXPECT_EQ(stats.top_words.size(), 2u);
  EXPECT_EQ(stats.top_chars.size(), 2u);
}

TEST(AnalyzerTest, ConfigMaxWordLen) {
  AnalyzerConfig config;
  config.max_word_len = 3;
  const TextStats stats = analyze_str("hello hello hi\n", config);
  // "hello" truncated to "he" (max_word_len=3 means 2 chars + null).
  ASSERT_GE(stats.top_words.size(), 1u);
  EXPECT_EQ(stats.top_words[0].word, "he");
  EXPECT_EQ(stats.top_words[0].count, 2);
}

TEST(AnalyzerTest, ZeroTopNYieldsEmptyRankings) {
  // analyze() no longer validates config; a zero field is degenerate but
  // well-defined (no top entries) rather than an error.
  AnalyzerConfig config;
  config.top_n = 0;
  const TextStats stats = analyze_str("the the cat\n", config);
  EXPECT_EQ(stats.word_count, 3);
  EXPECT_TRUE(stats.top_words.empty());
  EXPECT_TRUE(stats.top_chars.empty());
}

TEST(AnalyzerTest, JsonOutput) {
  const TextStats stats = analyze_str("the the cat\n");

  std::ostringstream os;
  text_analyzer::print_stats_json(os, stats);
  const std::string out = os.str();

  EXPECT_NE(out.find("\"words\": 3"), std::string::npos);
  EXPECT_NE(out.find("\"word\": \"the\""), std::string::npos);
  EXPECT_NE(out.find("\"count\": 2"), std::string::npos);
  EXPECT_NE(out.find("\"top_characters\""), std::string::npos);
}
