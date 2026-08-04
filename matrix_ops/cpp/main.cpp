#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
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

// The numeric options are bound to their real types and checked with CLI11's
// own CLI::Range, so both the grammar and the range are the library's. That
// makes this port accept spellings the C one refuses -- CLI11 converts integers
// with strtoull in base 0, strips '_' and '\'' group separators, and skips
// surrounding whitespace -- so `--rows 0x10`, `--rows 1_000` and `--rows " 2"`
// work here and are usage errors in C, and `--rows 010` even means eight rows
// here and ten there. The float side moves much less, since C's strtod already
// skipped leading whitespace: only a trailing space and the separators are new.
// The divergence is deliberate and tabulated in README.md; check_parity.sh
// cannot assert it, since it only ever asserts that the ports agree.
//
// The one contract rule no built-in can express is the exclusion of NaN:
// CLI::Range tests `val < min || val > max`, and both comparisons are false for
// a NaN, so it passes every range there is. Infinities are still caught, being
// greater than DBL_MAX. run() rejects a NaN --scalar by hand for that reason.
const CLI::Validator kPositive{
    CLI::Range(1, std::numeric_limits<int>::max(), "POSITIVE")};

const CLI::Validator kPrecision{CLI::Range(0, matrix_ops::kMaxPrecision)};

const CLI::Validator kFinite{CLI::Range(std::numeric_limits<double>::lowest(),
                                        std::numeric_limits<double>::max(),
                                        "FINITE")};

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
  std::vector<int> rows_vals;
  std::vector<int> cols_vals;
  std::vector<std::string> values_raw;
  std::vector<std::string> files_raw;
  double scalar_val = 0.0;
  int precision_val = matrix_ops::kDefaultPrecision;

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
  // option_text() overrides the whole "INT:POSITIVE" / "INT:INT in [0 - 1100]"
  // rendering a bound type plus a range would otherwise put in --help, which
  // wraps and reads worse than the placeholder the C port's help uses.
  const CLI::Option *rows_opt =
      app.add_option("-r,--rows", rows_vals, "rows for the next operand")
          ->check(kPositive)
          ->option_text("N ...")
          ->allow_extra_args(false);
  const CLI::Option *cols_opt =
      app.add_option("-c,--cols", cols_vals, "columns for the next operand")
          ->check(kPositive)
          ->option_text("N ...")
          ->allow_extra_args(false);
  const CLI::Option *scalar_opt =
      app.add_option("-k,--scalar", scalar_val, "the multiplier for 'scale'")
          ->check(kFinite)
          ->option_text("X")
          ->take_last();
  // option_text() replaces the default marker along with the type, so the
  // bound and the default are spelled out in the description instead -- which
  // is where the C port's help puts them anyway.
  app.add_option("-p,--precision", precision_val,
                 "decimal places in the output, trailing zeros trimmed (0 to " +
                     std::to_string(matrix_ops::kMaxPrecision) + ", default: " +
                     std::to_string(matrix_ops::kDefaultPrecision) + ")")
      ->check(kPrecision)
      ->option_text("N")
      ->take_last();

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
  fmt.precision = precision_val;

  std::optional<double> scalar;
  if (scalar_opt->count() > 0) {
    // The contract's one exclusion CLI::Range cannot state, checked by hand:
    // every comparison against a NaN is false, so a NaN is inside every range.
    if (std::isnan(scalar_val)) {
      std::cerr << "error: invalid value for --scalar (expected a finite "
                   "number)\n";
      return kExitUsage;
    }
    scalar = scalar_val;
  }

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
    // and that is CLI11's invariant to keep, not ours. It is also what pairs
    // the dimensions with their occurrences -- one value per occurrence means
    // the Nth element of rows_vals is the Nth --rows on the command line.
    const std::size_t at = cursor[opt]++;

    if (opt == rows_opt) {
      pending_rows = static_cast<std::size_t>(rows_vals.at(at));
    } else if (opt == cols_opt) {
      pending_cols = static_cast<std::size_t>(cols_vals.at(at));
    } else {
      const std::string &value = opt->results().at(at);
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
