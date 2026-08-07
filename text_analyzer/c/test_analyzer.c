#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include "analyzer.h"
}

static FILE *make_stream(const char *text) {
  return fmemopen(const_cast<char *>(text), strlen(text), "r");
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

TEST(AnalyzerTest, CharFrequencyTieBreak) {
  /* Equal counts rank ascending by character, so 'a' precedes 'b'. qsort is not
   * stable, so this depends on the comparator's explicit tiebreak. */
  FILE *f = make_stream("ba\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  ASSERT_GE(stats.top_char_count, 2);
  EXPECT_EQ(stats.top_chars[0].ch, 'a');
  EXPECT_EQ(stats.top_chars[1].ch, 'b');
  EXPECT_EQ(stats.top_chars[0].count, 1);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, NonAsciiBytesSeparateWords) {
  /* Input is processed as bytes: the two bytes of UTF-8 'é' each count as a
   * character, neither is a letter, digit, nor punctuation, and together they
   * end the word. */
  FILE *f = make_stream("caf\xc3\xa9 x\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.char_count, 8);
  EXPECT_EQ(stats.word_count, 2);
  EXPECT_EQ(stats.digit_count, 0);
  EXPECT_EQ(stats.punct_count, 0);
  ASSERT_GE(stats.top_word_count, 1);
  EXPECT_STREQ(stats.top_words[0].word, "caf");
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

TEST(AnalyzerTest, ZeroTopNYieldsEmptyRankings) {
  AnalyzerConfig config = analyzer_config_default();
  config.top_n = 0;
  FILE *f = make_stream("the the cat\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, &config, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.word_count, 3);
  EXPECT_EQ(stats.top_word_count, 0);
  EXPECT_EQ(stats.top_char_count, 0);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, BlankLines) {
  /* Four terminated lines; the empty one and the whitespace-only one are both
   * blank. */
  FILE *f = make_stream("a\n\n  \nb\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.line_count, 4);
  EXPECT_EQ(stats.blank_line_count, 2);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, UnterminatedFinalLineIsNotCounted) {
  /* Consistent with line_count: a final line without '\n' is not a line, so it
   * is not a blank line either. */
  FILE *f = make_stream("a\n   ");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.line_count, 1);
  EXPECT_EQ(stats.blank_line_count, 0);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, DigitsAndPunctuation) {
  FILE *f = make_stream("ab 12, c!\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.digit_count, 2);
  EXPECT_EQ(stats.punct_count, 2);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, WordLengthStatistics) {
  /* Lengths 1, 2, 3, 4. Nearest rank picks element ceil(p/100 * 4):
   * p25 -> 1st (1), p50 -> 2nd (2), p75 -> 3rd (3). */
  FILE *f = make_stream("a bb ccc dddd\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, NULL, &stats), 0);
  fclose(f);
  EXPECT_EQ(stats.word_length.count, 4);
  EXPECT_EQ(stats.word_length.sum, 10);
  EXPECT_EQ(stats.word_length.min, 1);
  EXPECT_EQ(stats.word_length.max, 4);
  EXPECT_EQ(stats.word_length.p25, 1);
  EXPECT_EQ(stats.word_length.p50, 2);
  EXPECT_EQ(stats.word_length.p75, 3);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, WordLengthIgnoresTruncation) {
  AnalyzerConfig config = analyzer_config_default();
  config.max_word_len = 3;
  FILE *f = make_stream("hello\n");
  TextStats stats;
  ASSERT_EQ(analyze_file(f, &config, &stats), 0);
  fclose(f);
  /* Stored spelling is truncated, but the measured length is the real one. */
  ASSERT_GE(stats.top_word_count, 1);
  EXPECT_STREQ(stats.top_words[0].word, "he");
  EXPECT_EQ(stats.word_length.max, 5);
  EXPECT_EQ(stats.word_length.sum, 5);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, MultiFeedAggregates) {
  Analyzer a;
  ASSERT_EQ(analyzer_init(&a, NULL), 0);

  FILE *f1 = make_stream("the cat\n");
  ASSERT_EQ(analyzer_feed(&a, f1), 0);
  fclose(f1);
  FILE *f2 = make_stream("the dog\n");
  ASSERT_EQ(analyzer_feed(&a, f2), 0);
  fclose(f2);

  TextStats stats;
  ASSERT_EQ(analyzer_finish(&a, &stats), 0);
  analyzer_free(&a);

  EXPECT_EQ(stats.line_count, 2);
  EXPECT_EQ(stats.word_count, 4);
  ASSERT_GE(stats.top_word_count, 1);
  EXPECT_STREQ(stats.top_words[0].word, "the");
  EXPECT_EQ(stats.top_words[0].count, 2);
  text_stats_free(&stats);
}

TEST(AnalyzerTest, WordSplitAcrossFeeds) {
  Analyzer a;
  ASSERT_EQ(analyzer_init(&a, NULL), 0);

  FILE *f1 = make_stream("hel");
  ASSERT_EQ(analyzer_feed(&a, f1), 0);
  fclose(f1);
  FILE *f2 = make_stream("lo\n");
  ASSERT_EQ(analyzer_feed(&a, f2), 0);
  fclose(f2);

  TextStats stats;
  ASSERT_EQ(analyzer_finish(&a, &stats), 0);
  analyzer_free(&a);

  EXPECT_EQ(stats.word_count, 1);
  ASSERT_GE(stats.top_word_count, 1);
  EXPECT_STREQ(stats.top_words[0].word, "hello");
  EXPECT_EQ(stats.word_length.max, 5);
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
  EXPECT_NE(out.find("\"blank_lines\": 0"), std::string::npos);
  EXPECT_NE(out.find("\"word_length\""), std::string::npos);
  EXPECT_NE(out.find("\"p50\": 3"), std::string::npos);
  /* Floats carry four decimals and ranked entries are expanded across lines;
   * both are required for byte-identical JSON with the C++ and Rust ports. */
  EXPECT_NE(out.find("\"mean\": 3.0000"), std::string::npos);
  EXPECT_NE(out.find("  \"top_words\": [\n    {\n      \"word\": \"the\",\n"),
            std::string::npos);
}
