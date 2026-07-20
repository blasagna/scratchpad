#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "copyfile.hpp"

namespace {

namespace fs = std::filesystem;

using copyfile::copy;
using copyfile::copy_fs;
using copyfile::copy_stream;
using copyfile::CopyStage;
using copyfile::expand_tilde;
using copyfile::resolve_destination;

// A writable directory unique to this test run. Bazel sets TEST_TMPDIR.
fs::path tmp_dir() {
  const char *dir = std::getenv("TEST_TMPDIR");
  return fs::path(dir != nullptr ? dir : ".");
}

void write_file(const fs::path &path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  ASSERT_TRUE(out.good());
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// --- copy_stream ---------------------------------------------------------

TEST(CopyStreamTest, CopiesNonEmptyContent) {
  std::istringstream src("hello, world\n");
  std::ostringstream dst;
  EXPECT_EQ(copy_stream(src, dst), CopyStage::kOk);
  EXPECT_EQ(dst.str(), "hello, world\n");
}

TEST(CopyStreamTest, EmptySourceYieldsEmptyOutput) {
  std::istringstream src("");
  std::ostringstream dst;
  EXPECT_EQ(copy_stream(src, dst), CopyStage::kOk);
  EXPECT_TRUE(dst.str().empty());
}

TEST(CopyStreamTest, CopiesBinaryDataWithEmbeddedNuls) {
  const std::string data("a\0b\0\0c", 6);
  std::istringstream src(data);
  std::ostringstream dst;
  EXPECT_EQ(copy_stream(src, dst), CopyStage::kOk);
  EXPECT_EQ(dst.str(), data);
}

TEST(CopyStreamTest, AlreadyFailedSourceTerminatesAsReadError) {
  // A stream arriving with failbit set (but not eof/bad) must not spin forever.
  std::istringstream src("data");
  src.setstate(std::ios::failbit);
  std::ostringstream dst;
  EXPECT_EQ(copy_stream(src, dst), CopyStage::kRead);
}

// --- expand_tilde --------------------------------------------------------

TEST(ExpandTildeTest, NoTildeReturnsInput) {
  EXPECT_EQ(expand_tilde("/abs/path.txt"), fs::path("/abs/path.txt"));
  EXPECT_EQ(expand_tilde("rel/path.txt"), fs::path("rel/path.txt"));
}

TEST(ExpandTildeTest, BareTildeUsesHome) {
  ASSERT_EQ(setenv("HOME", "/home/testuser", 1), 0);
  EXPECT_EQ(expand_tilde("~"), fs::path("/home/testuser"));
}

TEST(ExpandTildeTest, TildeSlashExpandsToHome) {
  ASSERT_EQ(setenv("HOME", "/home/testuser", 1), 0);
  EXPECT_EQ(expand_tilde("~/sub/f.txt"), fs::path("/home/testuser/sub/f.txt"));
}

TEST(ExpandTildeTest, TildeOnlyExpandsAtStart) {
  ASSERT_EQ(setenv("HOME", "/home/testuser", 1), 0);
  EXPECT_EQ(expand_tilde("a/~/b"), fs::path("a/~/b"));
}

TEST(ExpandTildeTest, UnknownUserLeftUnchanged) {
  EXPECT_EQ(expand_tilde("~no_such_user_xyz123/f.txt"),
            fs::path("~no_such_user_xyz123/f.txt"));
}

// --- resolve_destination -------------------------------------------------

TEST(ResolveDestinationTest, NonDirectoryReturnedAsIs) {
  const fs::path dst = tmp_dir() / "plain_dst.txt"; // does not exist
  EXPECT_EQ(resolve_destination(dst, "/a/b/src.txt"), dst);
}

TEST(ResolveDestinationTest, ExistingDirectoryGetsBasename) {
  const fs::path dir = tmp_dir() / "resolve_dir";
  fs::create_directories(dir);
  EXPECT_EQ(resolve_destination(dir, "/a/b/src.txt"), dir / "src.txt");
}

// --- copy (end to end) ---------------------------------------------------

TEST(CopyTest, RoundTripThroughRealFiles) {
  const fs::path src = tmp_dir() / "roundtrip_src.txt";
  const fs::path dst = tmp_dir() / "roundtrip_dst.txt";
  write_file(src, "line one\nline two\n");

  const copyfile::CopyResult result = copy(src.string(), dst.string());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.destination, dst);
  EXPECT_EQ(read_file(dst), "line one\nline two\n");
}

TEST(CopyTest, MissingSourceReportsOpenSource) {
  const fs::path src = tmp_dir() / "does_not_exist.txt";
  const fs::path dst = tmp_dir() / "unused_dst.txt";
  const copyfile::CopyResult result = copy(src.string(), dst.string());
  EXPECT_EQ(result.stage, CopyStage::kOpenSource);
  EXPECT_TRUE(static_cast<bool>(result.ec)); // OS detail captured
}

TEST(CopyTest, DirectorySourceReportsReadErrorWithDetail) {
  // A directory opens as a stream on Linux but fails on read; the failure
  // should carry an OS detail (EISDIR) so the message is clear.
  const fs::path dir = tmp_dir() / "src_is_a_dir";
  fs::create_directories(dir);
  const fs::path dst = tmp_dir() / "from_dir_dst.txt";
  const copyfile::CopyResult result = copy(dir.string(), dst.string());
  EXPECT_EQ(result.stage, CopyStage::kRead);
  EXPECT_TRUE(static_cast<bool>(result.ec)); // errno captured
}

TEST(CopyTest, DirectoryDestinationCopiesBasenameInside) {
  const fs::path dir = tmp_dir() / "into_dir";
  fs::create_directories(dir);
  const fs::path src = tmp_dir() / "into_dir_src.txt";
  write_file(src, "payload\n");

  const copyfile::CopyResult result = copy(src.string(), dir.string());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.destination, dir / "into_dir_src.txt");
  EXPECT_EQ(read_file(dir / "into_dir_src.txt"), "payload\n");
}

TEST(CopyTest, ExistingDestinationIsTruncated) {
  const fs::path src = tmp_dir() / "overwrite_src.txt";
  const fs::path dst = tmp_dir() / "overwrite_dst.txt";
  write_file(src, "new");
  write_file(dst, "old and much longer content");

  const copyfile::CopyResult result = copy(src.string(), dst.string());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(read_file(dst), "new"); // truncated, not appended
}

TEST(CopyTest, SameFileIsRefusedWithoutDataLoss) {
  const fs::path file = tmp_dir() / "same_file.txt";
  write_file(file, "keep me");

  const copyfile::CopyResult result = copy(file.string(), file.string());
  EXPECT_EQ(result.stage, CopyStage::kSameFile);
  EXPECT_EQ(read_file(file), "keep me"); // not truncated
}

// --- copy_fs (std::filesystem::copy_file alternative) --------------------

TEST(CopyFsTest, RoundTripThroughRealFiles) {
  const fs::path src = tmp_dir() / "fs_roundtrip_src.txt";
  const fs::path dst = tmp_dir() / "fs_roundtrip_dst.txt";
  write_file(src, "line one\nline two\n");

  const copyfile::CopyResult result = copy_fs(src.string(), dst.string());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.destination, dst);
  EXPECT_EQ(read_file(dst), "line one\nline two\n");
}

TEST(CopyFsTest, MissingSourceReportsOpenSource) {
  const fs::path src = tmp_dir() / "fs_does_not_exist.txt";
  const fs::path dst = tmp_dir() / "fs_unused_dst.txt";
  const copyfile::CopyResult result = copy_fs(src.string(), dst.string());
  EXPECT_EQ(result.stage, CopyStage::kOpenSource);
  EXPECT_TRUE(static_cast<bool>(result.ec));
}

TEST(CopyFsTest, DirectoryDestinationCopiesBasenameInside) {
  const fs::path dir = tmp_dir() / "fs_into_dir";
  fs::create_directories(dir);
  const fs::path src = tmp_dir() / "fs_into_dir_src.txt";
  write_file(src, "payload\n");

  const copyfile::CopyResult result = copy_fs(src.string(), dir.string());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.destination, dir / "fs_into_dir_src.txt");
  EXPECT_EQ(read_file(dir / "fs_into_dir_src.txt"), "payload\n");
}

TEST(CopyFsTest, ExistingDestinationIsOverwritten) {
  const fs::path src = tmp_dir() / "fs_overwrite_src.txt";
  const fs::path dst = tmp_dir() / "fs_overwrite_dst.txt";
  write_file(src, "new");
  write_file(dst, "old and much longer content");

  const copyfile::CopyResult result = copy_fs(src.string(), dst.string());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(read_file(dst), "new"); // overwrite_existing, not appended
}

TEST(CopyFsTest, SameFileIsRefusedWithoutDataLoss) {
  const fs::path file = tmp_dir() / "fs_same_file.txt";
  write_file(file, "keep me");

  const copyfile::CopyResult result = copy_fs(file.string(), file.string());
  EXPECT_EQ(result.stage, CopyStage::kSameFile);
  EXPECT_EQ(read_file(file), "keep me"); // not truncated
}

TEST(CopyFsTest, UnreadableRegularSourceIsBlamedOnSource) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root bypasses permission checks";

  const fs::path src = tmp_dir() / "fs_unreadable_src.txt";
  const fs::path dst = tmp_dir() / "fs_unreadable_dst.txt";
  write_file(src, "secret");
  fs::permissions(src, fs::perms::none);

  const copyfile::CopyResult result = copy_fs(src.string(), dst.string());
  fs::permissions(src, fs::perms::owner_read | fs::perms::owner_write);

  // The source is a regular file but unreadable, so the failure names the
  // source rather than being misattributed to the destination.
  EXPECT_EQ(result.stage, CopyStage::kOpenSource);
  EXPECT_TRUE(static_cast<bool>(result.ec));
}

} // namespace
