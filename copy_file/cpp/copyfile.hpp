#ifndef COPY_FILE_CPP_COPYFILE_HPP
#define COPY_FILE_CPP_COPYFILE_HPP

#include <filesystem>
#include <istream>
#include <ostream>
#include <string_view>
#include <system_error>

namespace copyfile {

// The stage a copy reached. kOk means success; every other value names the step
// that failed so callers can report which file and action went wrong.
enum class CopyStage {
  kOk,
  kOpenSource, // opening the source for reading failed
  kOpenDest,   // opening/creating the destination for writing failed
  kRead,       // a read error occurred on the source
  kWrite,      // a write or flush error occurred on the destination
  kSameFile,   // source and destination are the same file
};

// Returns a short human-readable label for a stage, e.g. "cannot open source
// file".
std::string_view describe(CopyStage stage);

// Outcome of copy(). Carries the stage reached, an optional OS error detail
// (set for open failures), and the fully resolved destination path so callers
// can report where the file actually landed.
struct CopyResult {
  CopyStage stage = CopyStage::kOk;
  std::error_code ec{};
  std::filesystem::path destination;

  bool ok() const noexcept { return stage == CopyStage::kOk; }
  explicit operator bool() const noexcept { return ok(); }
};

// copy_stream - copies every byte from src to dst.
//
// The caller owns both streams. Returns CopyStage::kOk on success (including an
// empty source), kRead on a source read error, or kWrite on a destination write
// error. Copies raw bytes, so embedded NULs are preserved.
CopyStage copy_stream(std::istream &src, std::ostream &dst);

// expand_tilde - expands a leading ~ or ~user in a path.
//
// "~" or "~/..." expands using $HOME (falling back to the current user's home
// directory from the password database); "~user" or "~user/..." expands using
// that user's home directory. A tilde anywhere but the start, or a home that
// cannot be resolved, leaves the path unchanged so the eventual open reports a
// clear error. Pure and side-effect free.
std::filesystem::path expand_tilde(std::string_view path);

// resolve_destination - picks the final destination path for a copy.
//
// If dst names an existing directory, returns dst / src.filename() so the
// source is copied into the directory under its base name, like cp. Otherwise
// returns dst unchanged.
std::filesystem::path resolve_destination(const std::filesystem::path &dst,
                                          const std::filesystem::path &src);

// copy - copies the file named by source to dest.
//
// Both arguments are passed through expand_tilde; dest is then passed through
// resolve_destination, so naming an existing directory copies into it. The
// source is opened for reading, the resolved destination is opened (created or
// truncated) for writing, all bytes are copied, and both files are closed via
// RAII. The returned CopyResult always carries the resolved destination.
CopyResult copy(std::string_view source, std::string_view dest);

// copy_fs - an alternative to copy() that delegates the byte transfer to
// std::filesystem::copy_file (with overwrite_existing).
//
// Path handling is identical: the same expand_tilde and resolve_destination are
// applied, so relative paths, ~ expansion, and directory destinations all
// behave as in copy(). Only the copy step differs. Because copy_file reports a
// single error_code without a stage, a failure is attributed to the source when
// it is not a regular file (missing or a directory) and to the destination
// otherwise; the CopyResult never reports kRead or kWrite.
CopyResult copy_fs(std::string_view source, std::string_view dest);

} // namespace copyfile

#endif // COPY_FILE_CPP_COPYFILE_HPP
