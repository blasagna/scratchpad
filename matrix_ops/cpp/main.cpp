#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "matrix.hpp"
#include "matrix_io.hpp"

namespace {

using matrix_ops::Error;
using matrix_ops::Matrix;

// Argument that means "read stdin", and the label used for it in errors.
constexpr std::string_view kStdinArg = "-";
constexpr std::string_view kStdinLabel = "<stdin>";

// The most operands any operation takes.
constexpr std::size_t kMaxOperands = 2;

// Exit codes, matching the C port: 2 is the user's mistake, 1 is an operation
// that failed.
constexpr int kExitUsage = 2;
constexpr int kExitFailure = 1;

enum class Op { kAdd, kSub, kMul, kScale };

struct OpSpec {
  std::string_view name;
  Op op = Op::kAdd;
  std::size_t operands = 0;
  bool needs_scalar = false;
};

constexpr std::array<OpSpec, 4> kOperations{{
    {"add", Op::kAdd, 2, false},
    {"sub", Op::kSub, 2, false},
    {"mul", Op::kMul, 2, false},
    {"scale", Op::kScale, 1, true},
}};

void print_help() {
  std::cout
      << "usage: matrix_ops <add|sub|mul|scale> [operand...] [options]\n"
         "       matrix_ops -h | --help\n"
         "\n"
         "Performs an operation on 2D matrices of real numbers and prints the "
         "result.\n"
         "\n"
         "Operations:\n"
         "  add      element-wise sum of two matrices of the same shape\n"
         "  sub      element-wise difference of two matrices of the same "
         "shape\n"
         "  mul      matrix product; the first operand's column count must "
         "equal\n"
         "           the second operand's row count\n"
         "  scale    multiplies one matrix by --scalar\n"
         "\n"
         "Operands:\n"
         "  Each --values or --file introduces one operand, and any --rows or "
         "--cols\n"
         "  written before it describes that operand, so the two operands of "
         "a\n"
         "  product may have different shapes.\n"
         "\n"
         "  -v, --values \"...\"  values separated by whitespace or newlines\n"
         "  -f, --file PATH     read the values from a file ('"
      << kStdinArg
      << "' for stdin)\n"
         "  -r, --rows N        rows for the next operand (optional)\n"
         "  -c, --cols N        columns for the next operand (optional)\n"
         "\n"
         "Shape:\n"
         "  Dimensions are optional. Without them the layout decides: one "
         "line of\n"
         "  values is a row vector, and several lines are rows. Given both "
         "--rows\n"
         "  and --cols, the values are reshaped row-major and their count "
         "must be\n"
         "  exactly rows x cols; given only one, the other is derived. Rows "
         "of\n"
         "  differing length are always an error.\n"
         "\n"
         "Options:\n"
         "  -k, --scalar X      the multiplier for 'scale'\n"
         "  -p, --precision N   decimal places in the output, trailing zeros "
         "trimmed\n"
         "                      (default: "
      << matrix_ops::kDefaultPrecision
      << ")\n"
         "  -h, --help          show this help\n"
         "\n"
         "Examples:\n"
         "  matrix_ops add --values \"1 2 3\" --values \"4 5 6\"\n"
         "  matrix_ops mul --rows 2 --cols 3 --values \"1 2 3 4 5 6\" \\\n"
         "                 --rows 3 --cols 2 --file b.txt\n"
         "  matrix_ops scale --scalar 2.5 --file a.txt\n";
}

void print_usage_error() {
  std::cerr << "usage: matrix_ops <add|sub|mul|scale> [operand...] [options]\n"
               "       matrix_ops --help\n";
}

// Parses value as a positive integer in [1, INT_MAX]. Reports the problem
// against opt_name and returns nullopt on failure, rejecting empty input,
// trailing junk, and non-positive values.
//
// The INT_MAX ceiling is not cosmetic and is not just for parity with the C
// port's strtol: --precision feeds std::format("{:.{}f}"), which throws
// std::format_error for a negative width. Without the bound, a value above
// INT_MAX narrowed to a negative int and aborted the process, and a value
// above UINT_MAX wrapped to 0 and silently changed the output.
std::optional<int> parse_positive(std::string_view opt_name,
                                  std::string_view value) {
  unsigned long long n = 0;
  const char *first = value.data();
  const char *last = first + value.size();
  const auto [stop, ec] = std::from_chars(first, last, n);
  if (ec != std::errc{} || stop != last || n < 1 ||
      n > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
    std::cerr << "error: invalid value '" << value << "' for " << opt_name
              << " (expected a positive integer)\n";
    return std::nullopt;
  }
  return static_cast<int>(n);
}

// Parses value as a finite double, applying the same rule the matrix values
// themselves are held to: no trailing junk, and no nan or infinity. strtod
// rather than from_chars for the reasons given in matrix_io.cpp.
std::optional<double> parse_scalar(std::string_view opt_name,
                                   std::string_view value) {
  const std::string text(value);
  char *end = nullptr;
  const double n = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(n)) {
    std::cerr << "error: invalid value '" << value << "' for " << opt_name
              << " (expected a finite number)\n";
    return std::nullopt;
  }
  return n;
}

const OpSpec *find_operation(std::string_view name) {
  const auto it = std::ranges::find(kOperations, name, &OpSpec::name);
  return it == kOperations.end() ? nullptr : &*it;
}

// Loads one operand from a file, where "-" means stdin.
matrix_ops::ParseResult load_file(std::string_view path, std::size_t rows,
                                  std::size_t cols) {
  if (path == kStdinArg)
    return matrix_ops::read_stream(std::cin, rows, cols);

  std::ifstream in{std::string(path), std::ios::binary};
  if (!in)
    return {{}, Error::kRead};
  return matrix_ops::read_stream(in, rows, cols);
}

// Reports the failure of an operand named by `source` and returns the exit
// code: an I/O failure is operational, a bad value or shape is the user's.
int report_operand_error(Error error, std::string_view source) {
  if (error == Error::kRead) {
    std::cerr << "matrix_ops: " << source << ": "
              << std::error_code(errno, std::generic_category()).message()
              << "\n";
    return kExitFailure;
  }
  std::cerr << "error: " << source << ": " << matrix_ops::describe(error)
            << "\n";
  return kExitUsage;
}

} // namespace

int main(int argc, char *argv[]) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));

  std::vector<Matrix> operands;
  std::vector<std::string_view> positionals;

  // Dimensions seen since the last operand closed; they attach to the next
  // --values or --file.
  std::size_t pending_rows = matrix_ops::kDimUnspecified;
  std::size_t pending_cols = matrix_ops::kDimUnspecified;

  matrix_ops::Format fmt;
  std::optional<double> scalar;

  // A hand-rolled loop rather than getopt_long, but it reproduces the parts of
  // getopt_long's behavior the C port gets for free, because the contract is
  // byte-for-byte parity and check_parity.sh compares stderr too:
  //
  //   - it permutes, so an option is recognized wherever it appears and the
  //     operation name may come before or after the operands;
  //   - "--" ends option parsing, and a lone "-" is an ordinary argument;
  //   - a value may be attached (--rows=2, -r2) or separate (--rows 2).
  //
  // Abbreviated long options (--row for --rows) are the one thing it does not
  // reproduce: getopt_long accepts any unambiguous prefix, and matching its
  // ambiguity diagnostics exactly costs more than the feature is worth. That
  // is a known divergence, recorded in README.md; check_parity.sh only ever
  // asserts agreement, so it cannot pin a difference and does not try.
  bool options_done = false;
  for (std::size_t i = 1; i < args.size(); i++) {
    const std::string_view arg = args[i];

    if (!options_done && arg == "--") {
      options_done = true;
      continue;
    }
    // A lone "-" is an ordinary argument, not an option.
    if (options_done || arg.size() < 2 || arg[0] != '-') {
      positionals.push_back(arg);
      continue;
    }

    // Split an attached value off the option name: "--rows=2" and "-r2" both
    // carry their value in the same argv element, the way getopt_long accepts
    // them in the C port.
    std::string_view name = arg;
    std::string_view value;
    bool attached = false;
    if (arg.starts_with("--")) {
      if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
        name = arg.substr(0, eq);
        value = arg.substr(eq + 1);
        attached = true;
      }
    } else if (arg.size() > 2) {
      name = arg.substr(0, 2);
      value = arg.substr(2);
      attached = true;
    }

    const bool takes_value =
        name == "-r" || name == "--rows" || name == "-c" || name == "--cols" ||
        name == "-v" || name == "--values" || name == "-f" ||
        name == "--file" || name == "-k" || name == "--scalar" ||
        name == "-p" || name == "--precision";

    if (!takes_value && attached && name.starts_with("--")) {
      std::cerr << "error: option '" << name << "' does not take a value\n";
      print_usage_error();
      return kExitUsage;
    }
    if (takes_value && !attached) {
      if (i + 1 >= args.size()) {
        std::cerr << "error: option '" << name << "' requires a value\n";
        print_usage_error();
        return kExitUsage;
      }
      value = args[++i];
    }

    const std::string_view arg_ = name;
    if (arg_ == "-h" || arg_ == "--help") {
      print_help();
      return 0;
    } else if (arg_ == "-r" || arg_ == "--rows") {
      const std::optional<int> n = parse_positive("--rows", value);
      if (!n)
        return kExitUsage;
      pending_rows = static_cast<std::size_t>(*n);
    } else if (arg_ == "-c" || arg_ == "--cols") {
      const std::optional<int> n = parse_positive("--cols", value);
      if (!n)
        return kExitUsage;
      pending_cols = static_cast<std::size_t>(*n);
    } else if (arg_ == "-v" || arg_ == "--values" || arg_ == "-f" ||
               arg_ == "--file") {
      if (operands.size() == kMaxOperands) {
        std::cerr << "error: at most " << kMaxOperands
                  << " matrices may be given\n";
        return kExitUsage;
      }
      const bool from_file = arg_ == "-f" || arg_ == "--file";
      matrix_ops::ParseResult parsed =
          from_file ? load_file(value, pending_rows, pending_cols)
                    : matrix_ops::parse_text(value, pending_rows, pending_cols);
      if (!parsed) {
        const std::string_view source = !from_file             ? "--values"
                                        : (value == kStdinArg) ? kStdinLabel
                                                               : value;
        return report_operand_error(parsed.error, source);
      }
      operands.push_back(std::move(parsed.matrix));
      // The dimensions described this operand only; the next one states its
      // own or infers them.
      pending_rows = matrix_ops::kDimUnspecified;
      pending_cols = matrix_ops::kDimUnspecified;
    } else if (arg_ == "-k" || arg_ == "--scalar") {
      const std::optional<double> k = parse_scalar("--scalar", value);
      if (!k)
        return kExitUsage;
      scalar = *k;
    } else if (arg_ == "-p" || arg_ == "--precision") {
      // Zero decimal places is meaningful -- it rounds to integers -- so this
      // is the one numeric option that is not required to be positive.
      if (value == "0") {
        fmt.precision = 0;
      } else {
        const std::optional<int> n = parse_positive("--precision", value);
        if (!n)
          return kExitUsage;
        fmt.precision = *n;
      }
    } else {
      std::cerr << "error: unknown option '" << name << "'\n";
      print_usage_error();
      return kExitUsage;
    }
  }

  if (pending_rows != matrix_ops::kDimUnspecified ||
      pending_cols != matrix_ops::kDimUnspecified) {
    std::cerr << "error: --rows/--cols given with no matrix to apply them to "
                 "(they must come before a --values or --file)\n";
    return kExitUsage;
  }

  if (positionals.empty()) {
    std::cerr << "error: missing operation\n";
    print_usage_error();
    return kExitUsage;
  }
  if (positionals.size() > 1) {
    std::cerr << "error: unexpected argument '" << positionals[1] << "'\n";
    print_usage_error();
    return kExitUsage;
  }

  const OpSpec *spec = find_operation(positionals.front());
  if (spec == nullptr) {
    std::cerr << "error: unknown operation '" << positionals.front()
              << "' (expected add, sub, mul, or scale)\n";
    return kExitUsage;
  }

  if (operands.size() != spec->operands) {
    std::cerr << "error: '" << spec->name << "' takes " << spec->operands << " "
              << (spec->operands == 1 ? "matrix" : "matrices") << ", but "
              << operands.size() << " "
              << (operands.size() == 1 ? "was" : "were") << " given\n";
    return kExitUsage;
  }

  if (spec->needs_scalar && !scalar) {
    std::cerr << "error: '" << spec->name << "' requires --scalar\n";
    return kExitUsage;
  }
  if (!spec->needs_scalar && scalar) {
    std::cerr << "error: --scalar applies only to 'scale'\n";
    return kExitUsage;
  }

  std::optional<Matrix> result;
  switch (spec->op) {
  case Op::kAdd:
    result = matrix_ops::add(operands[0], operands[1]);
    break;
  case Op::kSub:
    result = matrix_ops::sub(operands[0], operands[1]);
    break;
  case Op::kMul:
    result = matrix_ops::mul(operands[0], operands[1]);
    break;
  case Op::kScale:
    result = matrix_ops::scale(operands[0], *scalar);
    break;
  }

  if (!result) {
    // Naming both shapes turns "incompatible dimensions" into something the
    // user can act on without re-reading their own command line.
    std::cerr << "error: cannot " << spec->name << " a " << operands[0].rows()
              << "x" << operands[0].cols() << " matrix and a "
              << operands[1].rows() << "x" << operands[1].cols() << " one\n";
    return kExitUsage;
  }

  Error error = matrix_ops::write(std::cout, *result, fmt);
  if (error == Error::kOk) {
    std::cout.flush();
    if (!std::cout)
      error = Error::kWrite;
  }
  if (error != Error::kOk) {
    std::cerr << "matrix_ops: " << matrix_ops::describe(error) << ": "
              << std::error_code(errno, std::generic_category()).message()
              << "\n";
    return kExitFailure;
  }
  return 0;
}
