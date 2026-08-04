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
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>

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

// Exit codes for what this program reports itself, matching the C port: 2 is
// the user's mistake, 1 is an operation that failed. Argument errors come from
// CLI11 and carry its codes instead.
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

// Echoes CLI11's own hint after an error this program reports itself, so a
// usage failure points at --help however it was detected.
void print_usage_hint() {
  std::cerr << "Run with --help for more information.\n";
}

// Parses value as an unsigned decimal integer, accepting an optional leading
// '+' and then digits, and nothing else.
//
// The spelling is written down rather than inherited, the same way the value
// contract's number set is. '+3' is accepted because the contract accepts it
// for matrix values; leading whitespace is not, even though the C port's
// strtol would have skipped it, because ' 3' as an option value is a typo
// rather than a request. The C port checks the identical shape by hand.
std::optional<unsigned long long> parse_uint(std::string_view value) {
  std::string_view digits = value;
  if (digits.starts_with('+'))
    digits.remove_prefix(1);

  unsigned long long n = 0;
  const char *first = digits.data();
  const char *last = first + digits.size();
  const auto [stop, ec] = std::from_chars(first, last, n);
  if (ec != std::errc{} || stop != last)
    return std::nullopt;
  return n;
}

// Parses value as a positive integer in [1, INT_MAX], or nullopt.
//
// The INT_MAX ceiling is not cosmetic and is not just for parity with the C
// port's strtol: a dimension becomes a std::size_t, and a value above INT_MAX
// narrowed to a negative int before it got one.
std::optional<int> parse_positive(std::string_view value) {
  const std::optional<unsigned long long> n = parse_uint(value);
  if (!n || *n < 1 ||
      *n > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
    return std::nullopt;
  return static_cast<int>(*n);
}

// Parses value as a precision: the same integer spelling, but zero is
// meaningful here -- it rounds to integers -- and the ceiling is
// kMaxPrecision rather than INT_MAX, since past that every digit is a zero the
// trimming removes and the rendering alone would want gigabytes.
std::optional<int> parse_precision(std::string_view value) {
  const std::optional<unsigned long long> n = parse_uint(value);
  if (!n || *n > static_cast<unsigned long long>(matrix_ops::kMaxPrecision))
    return std::nullopt;
  return static_cast<int>(*n);
}

// Parses value as a finite double, applying the same rule the matrix values
// themselves are held to: no trailing junk, and no nan or infinity. strtod
// rather than from_chars for the reasons given in matrix_io.cpp.
std::optional<double> parse_scalar(std::string_view value) {
  const std::string text(value);
  char *end = nullptr;
  const double n = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(n))
    return std::nullopt;
  return n;
}

// The three validators above, as CLI11 checks. They are written here rather
// than reached for as CLI::PositiveNumber / CLI::Range / a numeric bind because
// CLI11's own conversion skips leading whitespace and accepts nan and inf: with
// the stock validators `--rows " 2"` and `--scalar inf` would succeed here and
// remain usage errors in the C port, which hand-checks these same shapes. CLI11
// owns the grammar; the accepted value set stays the contract's.
const CLI::Validator kPositive{[](const std::string &value) -> std::string {
                                 return parse_positive(value)
                                            ? std::string{}
                                            : "expected a positive integer";
                               },
                               "N", "positive integer"};

const CLI::Validator kPrecision{
    [](const std::string &value) -> std::string {
      return parse_precision(value)
                 ? std::string{}
                 : "expected 0 to " + std::to_string(matrix_ops::kMaxPrecision);
    },
    "N", "precision"};

const CLI::Validator kFinite{[](const std::string &value) -> std::string {
                               return parse_scalar(value)
                                          ? std::string{}
                                          : "expected a finite number";
                             },
                             "X", "finite number"};

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
  // Out of memory is about the machine rather than about this operand, so like
  // the C port it is reported without naming one.
  if (error == Error::kNoMem) {
    std::cerr << "matrix_ops: " << matrix_ops::describe(error) << "\n";
    return kExitFailure;
  }
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

int run(int argc, char *argv[]) {
  // The name is passed explicitly because CLI11 otherwise takes argv[0], which
  // under `bazel run` is the full runfiles path.
  CLI::App app{"Performs an operation on 2D matrices of real numbers and "
               "prints the result.\n"
               "\n"
               "Operations:\n"
               "  add      element-wise sum of two matrices of the same shape\n"
               "  sub      element-wise difference of two matrices of the same "
               "shape\n"
               "  mul      matrix product; the first operand's column count "
               "must equal\n"
               "           the second operand's row count\n"
               "  scale    multiplies one matrix by --scalar\n"
               "\n"
               "Operands:\n"
               "  Each --values or --file introduces one operand, and any "
               "--rows or --cols\n"
               "  written before it describes that operand, so the two "
               "operands of a\n"
               "  product may have different shapes.\n"
               "\n"
               "Shape:\n"
               "  Dimensions are optional. Without them the layout decides: "
               "one line of\n"
               "  values is a row vector, and several lines are rows. Given "
               "both --rows\n"
               "  and --cols, the values are reshaped row-major and their "
               "count must be\n"
               "  exactly rows x cols; given only one, the other is derived. "
               "Rows of\n"
               "  differing length are always an error.",
               "matrix_ops"};

  std::vector<std::string> positionals;
  std::vector<std::string> rows_raw;
  std::vector<std::string> cols_raw;
  std::vector<std::string> values_raw;
  std::vector<std::string> files_raw;
  std::string scalar_raw;
  std::string precision_raw;

  app.add_option("operation", positionals, "add, sub, mul, or scale");
  const CLI::Option *values_opt =
      app.add_option("-v,--values", values_raw,
                     "values separated by whitespace or newlines")
          ->type_name("\"...\"")
          ->allow_extra_args(false);
  const CLI::Option *file_opt =
      app.add_option("-f,--file", files_raw,
                     "read the values from a file ('" + std::string(kStdinArg) +
                         "' for stdin)")
          ->type_name("PATH")
          ->allow_extra_args(false);
  const CLI::Option *rows_opt =
      app.add_option("-r,--rows", rows_raw, "rows for the next operand")
          ->check(kPositive)
          ->allow_extra_args(false);
  const CLI::Option *cols_opt =
      app.add_option("-c,--cols", cols_raw, "columns for the next operand")
          ->check(kPositive)
          ->allow_extra_args(false);
  app.add_option("-k,--scalar", scalar_raw, "the multiplier for 'scale'")
      ->check(kFinite)
      ->take_last();
  app.add_option("-p,--precision", precision_raw,
                 "decimal places in the output, trailing zeros trimmed")
      ->check(kPrecision)
      ->take_last()
      ->default_str(std::to_string(matrix_ops::kDefaultPrecision));

  app.footer("Examples:\n"
             "  matrix_ops add --values \"1 2 3\" --values \"4 5 6\"\n"
             "  matrix_ops mul --rows 2 --cols 3 --values \"1 2 3 4 5 6\" \\\n"
             "                 --rows 3 --cols 2 --file b.txt\n"
             "  matrix_ops scale --scalar 2.5 --file a.txt");

  // CLI11 word-wraps the description and footer by default, which strips the
  // leading spaces the Operations block and the Examples continuation lines
  // rely on. The prose here is already laid out; print it verbatim.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  std::vector<Matrix> operands;

  // Dimensions seen since the last operand closed; they attach to the next
  // --values or --file.
  std::size_t pending_rows = matrix_ops::kDimUnspecified;
  std::size_t pending_cols = matrix_ops::kDimUnspecified;

  matrix_ops::Format fmt;
  std::optional<double> scalar;

  if (!scalar_raw.empty())
    scalar = parse_scalar(scalar_raw);
  if (!precision_raw.empty())
    fmt.precision = parse_precision(precision_raw).value();

  // CLI11 hands back one result vector per option, which loses the order the
  // four operand options were interleaved in -- and that order is the contract:
  // --rows/--cols bind to the *next* --values or --file, so `--rows 2 --values
  // A --rows 3 --values B` shapes two operands differently. parse_order() gives
  // the options back in the order they were typed, one entry per occurrence, so
  // walking it with a cursor into each option's results() replays the command
  // line exactly. (The Rust port could not do this under clap and pairs
  // dimensions with operands by index instead; see README.md.)
  std::map<const CLI::Option *, std::size_t> cursor;
  for (const CLI::Option *opt : app.parse_order()) {
    if (opt != rows_opt && opt != cols_opt && opt != values_opt &&
        opt != file_opt)
      continue;

    // at() rather than []: the index is only in range because
    // allow_extra_args(false) makes each occurrence carry exactly one value,
    // and that is CLI11's invariant to keep, not ours.
    const std::string &value = opt->results().at(cursor[opt]++);

    if (opt == rows_opt) {
      pending_rows = static_cast<std::size_t>(parse_positive(value).value());
    } else if (opt == cols_opt) {
      pending_cols = static_cast<std::size_t>(parse_positive(value).value());
    } else {
      if (operands.size() == kMaxOperands) {
        std::cerr << "error: at most " << kMaxOperands
                  << " matrices may be given\n";
        return kExitUsage;
      }
      const bool from_file = opt == file_opt;
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
    print_usage_hint();
    return kExitUsage;
  }
  if (positionals.size() > 1) {
    std::cerr << "error: unexpected argument '" << positionals[1] << "'\n";
    print_usage_hint();
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

} // namespace

int main(int argc, char *argv[]) try {
  return run(argc, argv);
} catch (const std::bad_alloc &) {
  // The backstop for the allocations run() does not funnel through matrix_io
  // -- a result matrix, the operand vector -- which in the C port are
  // MATRIX_ERR_NOMEM returns. Running out of memory is an operational failure
  // with a message and exit 1 in the shared contract; without this it was an
  // uncaught exception and a SIGABRT, which is neither.
  std::cerr << "matrix_ops: " << matrix_ops::describe(Error::kNoMem) << "\n";
  return kExitFailure;
}
