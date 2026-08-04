#include <iostream>
#include <string>
#include <string_view>

#include <CLI/CLI.hpp>

#include "copyfile.hpp"

int main(int argc, char *argv[]) {
  // CLI11 is given an explicit program name because it otherwise takes argv[0],
  // which under `bazel run` is the full runfiles path.
  CLI::App app{"Copies a source file to a destination.", "copy_file"};

  bool use_fs = false;
  std::string source;
  std::string dest;

  // --fs selects the std::filesystem::copy_file implementation; the default is
  // the explicit stream copy.
  app.add_flag("--fs", use_fs, "copy with std::filesystem::copy_file");
  app.add_option("source", source, "file to copy")->required();
  app.add_option("destination", dest,
                 "path to copy it to; an existing directory receives the file "
                 "under its own name")
      ->required();

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

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
