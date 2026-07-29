// CLI for the C++ exprkit library.
//
// ../rust/src/main.rs is the same program written against the Rust bindings.
// The two print byte-identical output for the same input -- structurally, not
// by convention: every number they print comes from exprkit::format_value.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "exprkit.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

constexpr std::string_view kUsage =
    "usage: exprkit [--names] [--help] [EXPRESSION ...]\n"
    "\n"
    "Evaluates arithmetic expressions.\n"
    "\n"
    "With one or more EXPRESSION arguments, each is evaluated in turn;\n"
    "otherwise expressions are read one per line from standard input.\n"
    "Variables assigned with `name = expr` persist across expressions.\n"
    "Blank lines and lines whose first non-blank character is '#' are\n"
    "skipped.\n"
    "\n"
    "  --names    after the last expression, print every defined name and\n"
    "             its value, one per line, as `name = value`\n"
    "  --help     print this message and exit\n";

// is_blank_or_comment - reports whether a line carries no expression.
bool is_blank_or_comment(std::string_view line) {
  for (char c : line) {
    if (c == ' ' || c == '\t' || c == '\r') {
      continue;
    }
    return c == '#';
  }
  return true;
}

// print_names - dumps the environment, one `name = value` per line.
void print_names(const exprkit::Evaluator &evaluator) {
  for (const std::string &name : evaluator.names()) {
    std::cout << name << " = " << exprkit::format_value(evaluator.get(name))
              << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  bool want_names = false;
  std::vector<std::string_view> expressions;

  for (int i = 1; i < argc; i++) {
    std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      return kExitOk;
    }
    if (arg == "--names") {
      want_names = true;
    } else if (arg.starts_with("--")) {
      // Note this catches a bare "--" too. clap, in the Rust CLI, treats "--"
      // as an end-of-options separator instead; that is one of the two places
      // the two CLIs deliberately differ (the other is --help text). Both are
      // argument-parser conventions rather than exprkit behavior.
      // A hint rather than the usage block: the Rust CLI gets its help text
      // from clap and could not reproduce kUsage byte for byte, and keeping
      // every non-help stream identical is worth more than the extra lines.
      std::cerr << "exprkit: unknown option: " << arg << '\n'
                << "Try 'exprkit --help' for more information.\n";
      return kExitUsage;
    } else {
      expressions.push_back(arg);
    }
  }

  exprkit::Evaluator evaluator;

  // One try around the whole run: the first failure stops everything, which is
  // what makes `exprkit 'x = 1/0' 'x'` fail instead of reporting twice.
  try {
    if (expressions.empty()) {
      std::string line;
      for (long number = 1; std::getline(std::cin, line); number++) {
        if (is_blank_or_comment(line)) {
          continue;
        }
        try {
          std::cout << exprkit::format_value(evaluator.eval(line)) << '\n';
        } catch (const exprkit::ExprError &err) {
          // Only stdin gets a line number; there is nothing to number when the
          // expressions came from the command line.
          std::cerr << "exprkit: line " << number << ": " << err.what() << '\n';
          return kExitError;
        }
      }
    } else {
      for (std::string_view expression : expressions) {
        std::cout << exprkit::format_value(evaluator.eval(expression)) << '\n';
      }
    }

    if (want_names) {
      print_names(evaluator);
    }
  } catch (const exprkit::ExprError &err) {
    std::cerr << "exprkit: " << err.what() << '\n';
    return kExitError;
  }

  return kExitOk;
}
