// CLI for the C++ exprkit library.
//
// ../rust/src/main.rs is the same program written against the Rust bindings.
// The two print byte-identical output for the same input -- structurally, not
// by convention: every number they print comes from exprkit::format_value.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include "exprkit.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

// What split_options found while partitioning argv.
struct Split {
  // Tokens to hand to CLI11, argv[0] first. Only options end up here.
  std::vector<std::string> options;
  // Everything else, in the order typed.
  std::vector<std::string> expressions;
  // The unrecognized "--" argument, when there was one.
  std::string unknown;
};

// split_options - decides which arguments are options before CLI11 sees them.
//
// Only a "--" prefix marks an option; a single dash never does. That is the
// rule ../rust/src/main.rs uses in its own split_options, and it is the one
// this file used before it had a parser, because a leading minus is ordinary
// arithmetic here: `-2 ^ 2` is -4, `-e` is -2.718..., and `-x + 1` reads a
// variable. Left to itself CLI11 classifies any `-<non-digit>` as a short
// option, which rejects all but the first -- and worse, silently prints help
// for `-h + 1`, since -h is a real flag. Options are recognized wherever they
// appear, so `exprkit '1+1' --names` works.
Split split_options(int argc, char **argv) {
  Split split;
  split.options.emplace_back(argv[0]);

  bool options_done = false;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    // "--" ends option parsing, matching clap. This file used to report it as
    // an unknown option, which was the one place the two CLIs disagreed about
    // it.
    if (!options_done && arg == "--") {
      options_done = true;
      continue;
    }
    if (!options_done && (arg == "--names" || arg == "--help" || arg == "-h")) {
      split.options.push_back(std::move(arg));
      continue;
    }
    if (!options_done && arg.starts_with("--")) {
      split.unknown = std::move(arg);
      return split;
    }
    split.expressions.push_back(std::move(arg));
  }
  return split;
}

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
               "With one or more EXPRESSION arguments, each is evaluated in "
               "turn;\n"
               "otherwise expressions are read one per line from standard "
               "input.\n"
               "Variables assigned with `name = expr` persist across "
               "expressions.\n"
               "Blank lines and lines whose first non-blank character is '#' "
               "are\n"
               "skipped.",
               "exprkit"};

  bool want_names = false;

  app.add_flag("--names", want_names,
               "after the last expression, print every defined name and its "
               "value, one per line, as `name = value`");
  // Declared so it appears in --help; the expressions themselves never reach
  // CLI11, since split_options has already taken them out of argv.
  app.add_option("expression", "expressions to evaluate")
      ->type_name("EXPRESSION");

  app.footer(
      "An expression may begin with '-' -- `exprkit '-2 ^ 2'` is -4 and\n"
      "`exprkit -e` is -2.718... . Quote any expression containing "
      "spaces,\n"
      "or the shell splits it into several.");

  // CLI11 word-wraps the description and footer by default, rewrapping prose
  // that is already laid out to fit. Print both verbatim instead.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  Split split = split_options(argc, argv);
  if (!split.unknown.empty()) {
    // Kept byte-identical with the same path in ../rust/src/main.rs. A hint
    // rather than the help block, because the two CLIs cannot share help text.
    std::cerr << "exprkit: unknown option: " << split.unknown << '\n'
              << "Try 'exprkit --help' for more information.\n";
    return kExitUsage;
  }

  // CLI11 parses what split_options classified as options -- the flags, the
  // help text, and the diagnostics are still its job.
  std::vector<char *> option_argv;
  option_argv.reserve(split.options.size());
  for (std::string &option : split.options)
    option_argv.push_back(option.data());

  try {
    app.parse(static_cast<int>(option_argv.size()), option_argv.data());
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  const std::vector<std::string> &expressions = split.expressions;

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
