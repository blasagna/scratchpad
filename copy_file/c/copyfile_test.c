#include <gtest/gtest.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "copyfile.h"
}

static FILE *make_read_stream(const char *data, size_t len) {
  return fmemopen((void *)data, len, "rb");
}

/* Builds a path under the test's writable temp dir. Bazel sets TEST_TMPDIR. */
static std::string tmp_path(const char *name) {
  const char *dir = getenv("TEST_TMPDIR");
  return std::string(dir ? dir : ".") + "/" + name;
}

static void write_file(const std::string &path, const char *data, size_t len) {
  FILE *f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(fwrite(data, 1, len, f), len);
  ASSERT_EQ(fclose(f), 0);
}

static std::string read_file(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  EXPECT_NE(f, nullptr);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  fclose(f);
  return out;
}

TEST(CopyStreamTest, CopiesNonEmptyContent) {
  const char text[] = "hello, world\n";
  FILE *src = make_read_stream(text, sizeof(text) - 1);
  ASSERT_NE(src, nullptr);

  char out[64] = {0};
  FILE *dst = fmemopen(out, sizeof(out), "wb");
  ASSERT_NE(dst, nullptr);

  EXPECT_EQ(copy_stream(src, dst), COPY_OK);
  fclose(src);
  ASSERT_EQ(fclose(dst), 0);

  EXPECT_STREQ(out, text);
}

TEST(CopyStreamTest, EmptySourceYieldsEmptyDestination) {
  FILE *src = make_read_stream("", 0);
  ASSERT_NE(src, nullptr);

  char out[8];
  FILE *dst = fmemopen(out, sizeof(out), "wb");
  ASSERT_NE(dst, nullptr);

  EXPECT_EQ(copy_stream(src, dst), COPY_OK);
  fclose(src);
  EXPECT_EQ(ftell(dst), 0); /* nothing written; check while dst is still open */
  ASSERT_EQ(fclose(dst), 0);
}

TEST(CopyStreamTest, CopiesBinaryDataWithEmbeddedNuls) {
  const char data[] = {'a', '\0', 'b', '\0', '\0', 'c'};
  FILE *src = make_read_stream(data, sizeof(data));
  ASSERT_NE(src, nullptr);

  char out[16] = {0};
  FILE *dst = fmemopen(out, sizeof(out), "wb");
  ASSERT_NE(dst, nullptr);

  EXPECT_EQ(copy_stream(src, dst), COPY_OK);
  fclose(src);
  ASSERT_EQ(fclose(dst), 0);

  EXPECT_EQ(memcmp(out, data, sizeof(data)), 0);
}

TEST(CopyPathTest, RoundTripThroughRealFiles) {
  const char content[] = "line one\nline two\n";
  std::string src = tmp_path("roundtrip_src.txt");
  std::string dst = tmp_path("roundtrip_dst.txt");
  write_file(src, content, sizeof(content) - 1);

  EXPECT_EQ(copy_path(src.c_str(), dst.c_str()), COPY_OK);
  EXPECT_EQ(read_file(dst), std::string(content));
}

TEST(CopyPathTest, MissingSourceReportsOpenSrc) {
  std::string src = tmp_path("does_not_exist.txt");
  std::string dst = tmp_path("unused_dst.txt");

  errno = 0;
  EXPECT_EQ(copy_path(src.c_str(), dst.c_str()), COPY_ERR_OPEN_SRC);
  EXPECT_NE(errno, 0); /* errno preserved from the failing fopen */
}

TEST(CopyPathTest, OverwritesExistingDestination) {
  std::string src = tmp_path("overwrite_src.txt");
  std::string dst = tmp_path("overwrite_dst.txt");
  write_file(src, "new", 3);
  write_file(dst, "old and much longer content", 27);

  EXPECT_EQ(copy_path(src.c_str(), dst.c_str()), COPY_OK);
  EXPECT_EQ(read_file(dst), std::string("new")); /* truncated, not appended */
}
