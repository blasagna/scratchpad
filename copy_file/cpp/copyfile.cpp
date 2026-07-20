#include "copyfile.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <string>

#include <pwd.h>
#include <unistd.h>

namespace copyfile {

namespace fs = std::filesystem;

namespace {

// Bytes moved per read()/write() call. Copying in blocks keeps the loop off the
// per-character path.
constexpr std::size_t kBufSize = 64 * 1024;

// Looks up a home directory: $HOME for an empty user name (falling back to the
// password database), or the named user's home otherwise. Returns an empty
// string when it cannot be resolved.
std::string home_dir(std::string_view user) {
  if (user.empty()) {
    if (const char *home = std::getenv("HOME");
        home != nullptr && *home != '\0')
      return home;
    if (const passwd *pw = getpwuid(getuid()); pw != nullptr)
      return pw->pw_dir;
    return {};
  }

  const std::string name(user);
  if (const passwd *pw = getpwnam(name.c_str()); pw != nullptr)
    return pw->pw_dir;
  return {};
}

} // namespace

std::string_view describe(CopyStage stage) {
  switch (stage) {
  case CopyStage::kOk:
    return "success";
  case CopyStage::kOpenSource:
    return "cannot open source file";
  case CopyStage::kOpenDest:
    return "cannot open destination file";
  case CopyStage::kRead:
    return "error reading source file";
  case CopyStage::kWrite:
    return "error writing destination file";
  case CopyStage::kSameFile:
    return "source and destination are the same file";
  }
  return "unknown error";
}

CopyStage copy_stream(std::istream &src, std::ostream &dst) {
  std::array<char, kBufSize> buf;

  while (true) {
    src.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const std::streamsize n = src.gcount();
    if (n > 0) {
      dst.write(buf.data(), n);
      if (!dst)
        return CopyStage::kWrite;
    }
    if (src.eof())
      return CopyStage::kOk;
    // Any other non-good state (badbit from a read error, or a failbit the
    // stream arrived with) is a read failure. Checking !src rather than bad()
    // also guarantees the loop terminates on an already-failed stream.
    if (!src)
      return CopyStage::kRead;
  }
}

fs::path expand_tilde(std::string_view path) {
  if (path.empty() || path.front() != '~')
    return fs::path(path);

  // The tilde prefix runs from '~' up to the first '/' (or the end).
  const std::size_t slash = path.find('/');
  const std::string_view prefix = path.substr(0, slash);
  const std::string_view rest =
      slash == std::string_view::npos ? std::string_view{} : path.substr(slash);

  const std::string home = home_dir(prefix.substr(1)); // drop the leading '~'
  if (home.empty())
    return fs::path(path); // unresolvable: leave as written

  return fs::path(home + std::string(rest));
}

fs::path resolve_destination(const fs::path &dst, const fs::path &src) {
  std::error_code ec;
  if (fs::is_directory(dst, ec))
    return dst / src.filename();
  return dst;
}

CopyResult copy(std::string_view source, std::string_view dest) {
  const fs::path src = expand_tilde(source);
  const fs::path dst = resolve_destination(expand_tilde(dest), src);

  CopyResult result;
  result.destination = dst;

  std::ifstream in(src, std::ios::binary);
  if (!in) {
    result.stage = CopyStage::kOpenSource;
    result.ec = std::error_code(errno, std::generic_category());
    return result;
  }

  // Refuse to copy a file onto itself: opening the destination with trunc would
  // empty it before a single byte is read, destroying the source. equivalent()
  // returns false (with eqec set) when dst does not yet exist, so a genuine new
  // destination proceeds normally.
  std::error_code eqec;
  if (fs::equivalent(src, dst, eqec)) {
    result.stage = CopyStage::kSameFile;
    return result;
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) {
    result.stage = CopyStage::kOpenDest;
    result.ec = std::error_code(errno, std::generic_category());
    return result;
  }

  result.stage = copy_stream(in, out);
  if (result.stage != CopyStage::kOk) {
    // Best-effort OS detail: the failing read/write syscall set errno, and
    // copy_stream returns straight to here without an intervening libc call.
    result.ec = std::error_code(errno, std::generic_category());
    return result;
  }

  // Flush explicitly so a buffered write error surfaces here rather than being
  // swallowed by the destructor.
  out.flush();
  if (!out) {
    result.stage = CopyStage::kWrite;
    result.ec = std::error_code(errno, std::generic_category());
  }

  return result;
}

CopyResult copy_fs(std::string_view source, std::string_view dest) {
  const fs::path src = expand_tilde(source);
  const fs::path dst = resolve_destination(expand_tilde(dest), src);

  CopyResult result;
  result.destination = dst;

  // Same as copy(): refuse a file-onto-itself copy. copy_file would also fail
  // here, but with a less clear "File exists" error, so report it explicitly.
  std::error_code eqec;
  if (fs::equivalent(src, dst, eqec)) {
    result.stage = CopyStage::kSameFile;
    return result;
  }

  std::error_code ec;
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    result.ec = ec;
    // copy_file bundles open/read/write into one error_code, so infer the
    // stage. Blame the source when it cannot actually be read as a regular file
    // (missing, a directory, or lacking read permission); otherwise the problem
    // is on the destination side. access() is best-effort attribution only.
    std::error_code probe;
    const bool source_readable =
        fs::is_regular_file(src, probe) && access(src.c_str(), R_OK) == 0;
    result.stage =
        source_readable ? CopyStage::kOpenDest : CopyStage::kOpenSource;
  }

  return result;
}

} // namespace copyfile
