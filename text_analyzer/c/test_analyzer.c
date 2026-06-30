#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include "analyzer.h"
}

static FILE *make_stream(const char *text) {
  return fmemopen((void *)text, strlen(text), "r");
}

TEST(AnalyzerTest, EmptyFile) {
  FILE *f = make_stream("");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.line_count, 0);
  EXPECT_EQ(stats.word_count, 0);
  EXPECT_EQ(stats.char_count, 0);
  EXPECT_EQ(stats.top_word_count, 0);
  EXPECT_EQ(stats.top_char_count, 0);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, SingleLine) {
  FILE *f = make_stream("hello world\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.line_count, 1);
  EXPECT_EQ(stats.word_count, 2);
  EXPECT_EQ(stats.char_count, 12);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, MultiLine) {
  FILE *f = make_stream("one\ntwo\nthree\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.line_count, 3);
  EXPECT_EQ(stats.word_count, 3);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, WordFrequency) {
  FILE *f = make_stream("the cat sat the cat the\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_GE(stats.top_word_count, 1);
  EXPECT_STREQ(stats.top_words[0].word, "the");
  EXPECT_EQ(stats.top_words[0].count, 3);
  EXPECT_STREQ(stats.top_words[1].word, "cat");
  EXPECT_EQ(stats.top_words[1].count, 2);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, WordNormalization) {
  FILE *f = make_stream("The the THE tHe\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.word_count, 4);
  EXPECT_GE(stats.top_word_count, 1);
  EXPECT_STREQ(stats.top_words[0].word, "the");
  EXPECT_EQ(stats.top_words[0].count, 4);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, CharFrequency) {
  FILE *f = make_stream("aaabbc\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_GE(stats.top_char_count, 1);
  EXPECT_EQ(stats.top_chars[0].ch, 'a');
  EXPECT_EQ(stats.top_chars[0].count, 3);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, TrailingWordNoNewline) {
  FILE *f = make_stream("hello world");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.line_count, 0);
  EXPECT_EQ(stats.word_count, 2);
  EXPECT_EQ(stats.char_count, 11);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, ConfigTopN) {
  AnalyzerConfig config = analyzer_config_default();
  config.top_n = 2;
  FILE *f = make_stream("a b c d e f\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, &config, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.top_word_count, 2);
  EXPECT_EQ(stats.top_char_count, 2);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, ConfigMaxWordLen) {
  AnalyzerConfig config = analyzer_config_default();
  config.max_word_len = 3;
  FILE *f = make_stream("hello hello hi\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, &config, &stats), 0);
  fclose(f);
  /* "hello" truncated to "he" (max_word_len=3 means 2 chars + null) */
  EXPECT_STREQ(stats.top_words[0].word, "he");
  EXPECT_EQ(stats.top_words[0].count, 2);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, JsonOutput) {
  FILE *f = make_stream("the the cat\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);

  testing::internal::CaptureStdout();
  print_stats_json(&stats);
  std::string out = testing::internal::GetCapturedStdout();
  text_stats_free(&stats);

  EXPECT_NE(out.find("\"words\": 3"), std::string::npos);
  EXPECT_NE(out.find("\"word\": \"the\""), std::string::npos);
  EXPECT_NE(out.find("\"count\": 2"), std::string::npos);
  EXPECT_NE(out.find("\"top_characters\""), std::string::npos);
}
