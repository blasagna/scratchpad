// CLI for the C++ exprkit library.
//
// ../rust/src/main.rs is the same program written against the Rust bindings.
// Every number the two print comes from exprkit::format_value, so results agree
// structurally rather than by convention. They no longer agree on how many
// expressions they take: this one accepts a single quoted argument, the Rust
// one accepts a list. See ../README.md.

#include <iostream>
#include <string>
#include <string_view>

#include <CLI/CLI.hpp>

#include "exprkit.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;

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
  // The name is passed explicitly because CLI11 otherwise takes argv[0], which
  // under `bazel run` is the full runfiles path.
  CLI::App app{"Evaluates arithmetic expressions.\n"
               "\n"
               "With an EXPRESSION argument it is evaluated and printed;\n"
               "otherwise expressions are read one per line from standard "
               "input,\n"
               "and variables assigned with `name = expr` persist from one "
               "line\n"
               "to the next. Blank lines and lines whose first non-blank\n"
               "character is '#' are skipped.",
               "exprkit"};

  // Only "--help", with no "-h" alias. CLI11 classifies any "-<non-digit>" as a
  // short option, so with -h declared `exprkit '-h + 1'` matched the help flag
  // and printed help with exit 0 -- a wrong answer that looks like success.
  // Undeclared, the same argument is an ordinary unknown-option rejection.
  app.set_help_flag("--help", "Print this help message and exit");

  bool want_names = false;
  std::string expression;

  app.add_flag("--names", want_names,
               "after the expression, print every defined name and its "
               "value, one per line, as `name = value`");
  const CLI::Option *expression_opt =
      app.add_option("expression", expression, "the expression to evaluate")
          ->type_name("EXPRESSION");

  app.footer(
      "EXPRESSION is one argument, so quote it: `exprkit '1 + 2'`. Unquoted,\n"
      "the shell splits it and the extra words are rejected.\n"
      "\n"
      "An expression starting with '-' looks like an option, so write\n"
      "`exprkit -- '-e'` or `exprkit -- '-x + 1'`. A leading negative number,\n"
      "as in `exprkit '-2 ^ 2'`, needs no separator.");

  // CLI11 word-wraps the description and footer by default, rewrapping prose
  // that is already laid out to fit. Print both verbatim instead.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  exprkit::Evaluator evaluator;

  // One try around the whole run, so a failure reports once and stops.
  try {
    if (expression_opt->count() == 0) {
      std::string line;
      for (long number = 1; std::getline(std::cin, line); number++) {
        if (is_blank_or_comment(line)) {
          continue;
        }
        try {
          std::cout << exprkit::format_value(evaluator.eval(line)) << '\n';
        } catch (const exprkit::ExprError &err) {
          // Only stdin gets a line number; there is nothing to number when the
          // expression came from the command line.
          std::cerr << "exprkit: line " << number << ": " << err.what() << '\n';
          return kExitError;
        }
      }
    } else {
      std::cout << exprkit::format_value(evaluator.eval(expression)) << '\n';
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
