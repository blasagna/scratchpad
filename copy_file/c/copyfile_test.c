#include <gtest/gtest.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

TEST(CopyPathTest, SameFileIsRefusedWithoutDataLoss) {
  std::string file = tmp_path("same_file.txt");
  write_file(file, "keep me", 7);

  /* Copying a file onto itself must not truncate it to empty. */
  EXPECT_EQ(copy_path(file.c_str(), file.c_str()), COPY_ERR_SAME_FILE);
  EXPECT_EQ(read_file(file), std::string("keep me"));
}

TEST(CopyPathTest, DirectoryDestinationCopiesBasenameInside) {
  std::string dir = tmp_path("destdir");
  ASSERT_EQ(mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST, true);
  std::string src = tmp_path("into_dir_src.txt");
  write_file(src, "payload\n", 8);

  EXPECT_EQ(copy_path(src.c_str(), dir.c_str()), COPY_OK);
  /* The source's base name lands inside the directory. */
  EXPECT_EQ(read_file(dir + "/into_dir_src.txt"), std::string("payload\n"));
}

/* Wraps a char* returned by the path helpers: copies it out and frees it. */
static std::string take(char *p) {
  std::string s = p ? p : "";
  free(p);
  return s;
}

TEST(ExpandTildeTest, NoTildeReturnsCopy) {
  EXPECT_EQ(take(copy_expand_tilde("/abs/path.txt")), "/abs/path.txt");
  EXPECT_EQ(take(copy_expand_tilde("rel/path.txt")), "rel/path.txt");
}

TEST(ExpandTildeTest, BareTildeUsesHome) {
  ASSERT_EQ(setenv("HOME", "/home/testuser", 1), 0);
  EXPECT_EQ(take(copy_expand_tilde("~")), "/home/testuser");
}

TEST(ExpandTildeTest, TildeSlashExpandsToHome) {
  ASSERT_EQ(setenv("HOME", "/home/testuser", 1), 0);
  EXPECT_EQ(take(copy_expand_tilde("~/sub/f.txt")), "/home/testuser/sub/f.txt");
}

TEST(ExpandTildeTest, TildeOnlyExpandsAtStart) {
  ASSERT_EQ(setenv("HOME", "/home/testuser", 1), 0);
  EXPECT_EQ(take(copy_expand_tilde("a/~/b")), "a/~/b");
}

TEST(ExpandTildeTest, UnknownUserLeftUnchanged) {
  /* No such user, so the path can't be resolved and is returned verbatim. */
  EXPECT_EQ(take(copy_expand_tilde("~no_such_user_xyz123/f.txt")),
            "~no_such_user_xyz123/f.txt");
}

TEST(ResolveDestTest, NonDirectoryReturnsCopy) {
  std::string dst = tmp_path("plain_dst.txt"); /* does not exist */
  EXPECT_EQ(take(copy_resolve_dest(dst.c_str(), "/a/b/src.txt")), dst);
}

TEST(ResolveDestTest, ExistingDirectoryAppendsBasename) {
  std::string dir = tmp_path("resolve_dir");
  ASSERT_EQ(mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST, true);
  EXPECT_EQ(take(copy_resolve_dest(dir.c_str(), "/a/b/src.txt")),
            dir + "/src.txt");
}

TEST(ResolveDestTest, TrailingSlashIsNotDoubled) {
  std::string dir = tmp_path("resolve_dir_slash");
  ASSERT_EQ(mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST, true);
  std::string with_slash = dir + "/";
  EXPECT_EQ(take(copy_resolve_dest(with_slash.c_str(), "src.txt")),
            dir + "/src.txt");
}
