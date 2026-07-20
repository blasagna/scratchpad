#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

#include "copyfile.hpp"

int main(int argc, char *argv[]) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));

  // An optional leading --fs flag selects the std::filesystem::copy_file
  // implementation; the default is the explicit stream copy.
  bool use_fs = false;
  std::size_t pos = 1;
  if (pos < args.size() && std::string_view(args[pos]) == "--fs") {
    use_fs = true;
    ++pos;
  }

  if (args.size() - pos != 2) {
    std::cerr << "usage: copy_file [--fs] <source> <destination>\n";
    return 2;
  }

  const std::string_view source = args[pos];
  const std::string_view dest = args[pos + 1];

  const copyfile::CopyResult result =
      use_fs ? copyfile::copy_fs(source, dest) : copyfile::copy(source, dest);
  if (result) {
    // Show the resolved destination so a directory target reveals the real
    // path.
    std::cout << "copied '" << source << "' to '" << result.destination.string()
              << "'\n";
    return 0;
  }

  // A same-file failure concerns both paths, so name neither.
  if (result.stage == copyfile::CopyStage::kSameFile) {
    std::cerr << "copy_file: " << copyfile::describe(result.stage) << "\n";
    return 1;
  }

  // Errors about opening or writing the destination name it; the rest are about
  // the source.
  const std::string_view file =
      (result.stage == copyfile::CopyStage::kOpenDest ||
       result.stage == copyfile::CopyStage::kWrite)
          ? dest
          : source;

  std::cerr << "copy_file: " << copyfile::describe(result.stage) << ": "
            << file;
  if (result.ec)
    std::cerr << ": " << result.ec.message();
  std::cerr << "\n";
  return 1;
}
