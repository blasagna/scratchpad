#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <system_error>

#include <CLI/CLI.hpp>

#include "shell.hpp"

namespace {

// Exit code for anything the shell reports about itself. A usage error is 2,
// but that one is CLI11's to report and carries its own code.
constexpr int kExitFailure = 1;

void report(const shell::Result &result) {
  std::cerr << shell::kProgName << ": " << shell::describe(result.stage);
  if (result.ec)
    std::cerr << ": " << result.ec.message();
  std::cerr << "\n";
}

} // namespace

int main(int argc, char *argv[]) {
  // The name is passed explicitly because CLI11 otherwise takes argv[0], which
  // under `bazel run` is the full runfiles path.
  CLI::App app{
      "A prototype shell. Prints a '$' prompt, reads one command per line,\n"
      "hands it to the system command interpreter, and reports the exit "
      "status\n"
      "of any command that does not succeed. Repeats until you type 'exit' "
      "or\n"
      "close the input.\n"
      "\n"
      "Every command runs in a fresh subshell, so state a command sets --\n"
      "the working directory, an environment variable -- is gone by the next\n"
      "prompt. 'cd' therefore appears to do nothing.",
      "mini_shell"};

  shell::Options opts;
  opts.runner = shell::system_runner;

  // The '!' prefix is CLI11's negated flag: --no-banner sets show_banner to
  // false. Same spelling as simple_logger's --no-timestamp.
  app.add_flag("!--no-banner", opts.show_banner, "skip the startup banner");

  app.footer("Commands come from stdin, so the prompt and banner are printed "
             "whether\nor not that is a terminal.");

  // CLI11 word-wraps the description and footer by default, which would rewrap
  // prose that is already laid out. Print it verbatim.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  try {
    // There are no operands: commands are read from stdin, never from argv, so
    // a stray one is a mistake worth naming. CLI11 rejects it as an extra.
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  // std::system(nullptr) reports whether an interpreter exists at all. Asking
  // once is worth it: without one, every command would fail the same way, one
  // line of errno noise at a time.
  if (std::system(nullptr) == 0) {
    report({shell::Stage::kNoShell, {}});
    return kExitFailure;
  }

  // Read stdin unbuffered, so a command inherits the input mini_shell has not
  // consumed yet. Buffered, stdio pulls the whole pipe in before the first fork
  // and `printf 'cat\necho done\n' | mini_shell` hands `cat` an empty stdin.
  // POSIX requires exactly this of a shell ("It shall not read ahead in such a
  // manner that any characters intended to be read by the invoked command are
  // consumed by the shell"), and bash honors it; dash does not, so do not take
  // /bin/sh as the reference here.
  //
  // This reaches std::cin because sync_with_stdio is true by default, which
  // makes std::cin read through this very FILE*. Never call
  // std::ios::sync_with_stdio(false) here: that swaps in a filebuf with its own
  // read-ahead and puts the bug straight back. No unit test can catch either
  // mistake -- the suite drives run() with a std::istringstream, where
  // buffering is invisible -- so the guard is mini_shell/check_parity.sh.
  if (std::setvbuf(stdin, nullptr, _IONBF, 0) != 0) {
    std::cerr << shell::kProgName << ": cannot unbuffer stdin: "
              << std::error_code(errno, std::generic_category()).message()
              << "\n";
    return kExitFailure;
  }

  shell::Result result;
  try {
    result = shell::run(std::cin, std::cout, std::cerr, opts);
  } catch (const std::bad_alloc &) {
    // The C port gets this as a getline failure and returns SHELL_ERR_NOMEM;
    // here it arrives as an exception, and is reported the same way.
    report({shell::Stage::kNoMem, {}});
    return kExitFailure;
  }

  // run() reports a read failure from badbit, which is correct for a string
  // stream but cannot see one on std::cin: that is backed by a
  // stdio_sync_filebuf whose underflow() returns EOF on error without setting
  // badbit, so a failed read is indistinguishable from a clean end of input at
  // the iostream level. The FILE* underneath does record it, and it is the same
  // flag the C port's ferror(in) checks. Without this the program would exit 0
  // on unreadable stdin, silently stopping short of the rest of the input.
  if (result && std::ferror(stdin))
    result = {shell::Stage::kRead,
              std::error_code(errno, std::generic_category())};

  if (!result) {
    report(result);
    return kExitFailure;
  }
  return 0;
}
