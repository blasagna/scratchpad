// Golden tests: render every case in testdata/ and compare against the
// committed expected output.
//
// The C and Rust ports run the same corpus against the same golden files, so
// all three agreeing with the goldens means all three agree with each other.
// That is the project's central invariant, and this is what enforces it.
//
// Regenerate the goldens with testdata/regenerate.sh after an intentional
// behavior change.

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "analyzer.hpp"

namespace {

using text_analyzer::AnalyzerConfig;
using text_analyzer::TextStats;

// Must stay in sync with regenerate.sh's ALT_FLAGS and the other two ports.
AnalyzerConfig alt_config() {
  AnalyzerConfig config;
  config.top_n = 3;
  config.max_word_len = 5;
  return config;
}

// Bazel guarantees TEST_SRCDIR and TEST_WORKSPACE for tests; using them avoids
// depending on the runner's working directory. Falls back to a source-relative
// path so the binary can also be run directly.
std::string testdata_dir() {
  const char *srcdir = std::getenv("TEST_SRCDIR");
  const char *workspace = std::getenv("TEST_WORKSPACE");
  if (srcdir != nullptr && workspace != nullptr) {
    return std::string(srcdir) + "/" + workspace + "/text_analyzer/testdata";
  }
  return "text_analyzer/testdata";
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot open " << path;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

std::vector<std::string> read_cases() {
  std::vector<std::string> cases;
  std::ifstream in(testdata_dir() + "/cases.txt");
  EXPECT_TRUE(in.good()) << "cannot open cases.txt in " << testdata_dir();
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      cases.push_back(line);
    }
  }
  return cases;
}

// Renders one input with the given config, as text or JSON.
std::string render(const std::string &input_path, const AnalyzerConfig &config,
                   bool json) {
  std::ifstream in(input_path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot open " << input_path;

  const TextStats stats = text_analyzer::analyze(in, config);
  std::ostringstream os;
  if (json) {
    text_analyzer::print_stats_json(os, stats);
  } else {
    text_analyzer::print_stats(os, stats);
  }
  return os.str();
}

} // namespace

TEST(GoldenTest, MatchesCommittedOutput) {
  const std::string dir = testdata_dir();
  const std::vector<std::string> cases = read_cases();

  // A golden suite that silently finds no data would pass while proving
  // nothing, so treat an empty corpus as a failure.
  ASSERT_FALSE(cases.empty()) << "no cases found in " << dir << "/cases.txt";

  const AnalyzerConfig defaults;
  const AnalyzerConfig alt = alt_config();

  for (const std::string &name : cases) {
    const std::string input = dir + "/" + name;
    SCOPED_TRACE(name);

    EXPECT_EQ(render(input, defaults, false), read_file(input + ".out"));
    EXPECT_EQ(render(input, defaults, true), read_file(input + ".json"));
    EXPECT_EQ(render(input, alt, false), read_file(input + ".alt.out"));
    EXPECT_EQ(render(input, alt, true), read_file(input + ".alt.json"));
  }
}
