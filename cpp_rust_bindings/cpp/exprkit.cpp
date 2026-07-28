#include "exprkit.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <system_error>
#include <utility>
#include <vector>

namespace exprkit {
namespace {

using Variables = std::map<std::string, double, std::less<>>;

// How deeply expressions may nest before the parser gives up.
//
// Recursive descent uses one set of stack frames per nesting level, so without
// a cap an input like "((((...1" is a stack overflow -- and a stack overflow is
// not an exception. It cannot be caught, it cannot become a Result, and it
// takes the process down through whichever language is on the stack. That would
// be a hole straight through this project's central claim, so the limit is
// deliberately far below anything the stack could not absorb rather than tuned
// to the largest value that happens to survive.
constexpr int kMaxDepth = 256;

// Increments a depth counter for as long as it is alive.
//
// The check runs before the increment, so the counter stays accurate when the
// throw unwinds -- a Parser is single-use, but a counter that only ever leaks
// upward is the kind of thing that becomes wrong later.
class DepthGuard {
public:
  explicit DepthGuard(int &depth) : depth_(depth) {
    if (depth_ >= kMaxDepth) {
      throw ExprError("expression nests too deeply");
    }
    depth_++;
  }

  ~DepthGuard() { depth_--; }

  DepthGuard(const DepthGuard &) = delete;
  DepthGuard &operator=(const DepthGuard &) = delete;

private:
  int &depth_;
};

bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
         c == '\f';
}

// check_finite - rejects a value that arithmetic has poisoned.
//
// Called after every operation rather than only at the end, so the error names
// the step that overflowed instead of surfacing much later as a stray nan.
double check_finite(double value) {
  if (!std::isfinite(value)) {
    throw ExprError("result is not a finite number");
  }
  return value;
}

// apply - evaluates a one-argument function call.
double apply(std::string_view name, double arg) {
  if (name == "sqrt") {
    return std::sqrt(arg);
  }
  if (name == "abs") {
    return std::fabs(arg);
  }
  if (name == "floor") {
    return std::floor(arg);
  }
  if (name == "ceil") {
    return std::ceil(arg);
  }
  if (name == "ln") {
    return std::log(arg);
  }
  if (name == "exp") {
    return std::exp(arg);
  }
  if (name == "sin") {
    return std::sin(arg);
  }
  if (name == "cos") {
    return std::cos(arg);
  }
  throw ExprError(std::format("unknown function: '{}'", name));
}

// A recursive-descent parser over one expression.
//
// It evaluates as it goes; there is no syntax tree, because nothing here needs
// one twice. The environment is the caller's, and assignments write through to
// it. One Parser handles one string and is then discarded.
class Parser {
public:
  Parser(std::string_view text, Variables &variables)
      : text_(text), variables_(variables) {}

  // parse - evaluates the whole input and insists all of it was consumed.
  //
  // Assignments are applied here, at the very end, and not before: a failure
  // anywhere -- including trailing input found after the assignment's value was
  // computed -- must leave the environment exactly as it was. Callers rely on
  // that, and an Evaluator that half-applied a failed line would be worse than
  // one that rejected it outright.
  double parse() {
    double value = parse_program();
    skip_space();
    if (!at_end()) {
      throw ExprError(
          std::format("unexpected trailing input: '{}'", text_.substr(pos_)));
    }

    for (const auto &assignment : pending_) {
      variables_[assignment.first] = assignment.second;
    }
    return value;
  }

private:
  double parse_program() {
    DepthGuard guard(depth_);
    skip_space();
    // An assignment is only recognizable one token in, so try it and rewind if
    // the "=" does not materialize. `x` and `x = 1` share their first token.
    std::size_t start = pos_;
    if (!at_end() && is_ident_start(peek())) {
      std::string_view name = take_ident();
      skip_space();
      if (eat('=')) {
        // Recursing rather than calling parse_expr makes `x = y = 1` chain.
        double value = parse_program();
        // Recorded, not applied. See parse(). Nothing later in this input can
        // read the name back -- assignments are a prefix of the grammar, and
        // the right-hand side was evaluated before this line -- so deferring
        // changes no result, only when the environment is touched.
        pending_.emplace_back(std::string(name), value);
        return value;
      }
    }
    pos_ = start;
    return parse_expr();
  }

  double parse_expr() {
    double value = parse_term();
    for (;;) {
      skip_space();
      if (eat('+')) {
        value = check_finite(value + parse_term());
      } else if (eat('-')) {
        value = check_finite(value - parse_term());
      } else {
        return value;
      }
    }
  }

  double parse_term() {
    double value = parse_unary();
    for (;;) {
      skip_space();
      if (eat('*')) {
        value = check_finite(value * parse_unary());
      } else if (eat('/')) {
        value = check_finite(value / nonzero(parse_unary()));
      } else if (eat('%')) {
        value = check_finite(std::fmod(value, nonzero(parse_unary())));
      } else {
        return value;
      }
    }
  }

  // Every operand -- a parenthesized group, a function argument, a stacked
  // sign, the right side of a `^` -- reaches the parser through here, so one
  // guard at this point bounds every recursive path except the assignment
  // chaining in parse_program, which carries its own.
  double parse_unary() {
    DepthGuard guard(depth_);
    skip_space();
    if (eat('-')) {
      return -parse_unary();
    }
    if (eat('+')) {
      return parse_unary();
    }
    return parse_power();
  }

  double parse_power() {
    double base = parse_primary();
    skip_space();
    if (eat('^')) {
      // The right side is a unary, not a power, so it can be signed (2^-1) and
      // still chain right-associatively through parse_power below it (2^3^2).
      return check_finite(std::pow(base, parse_unary()));
    }
    return base;
  }

  double parse_primary() {
    skip_space();
    if (at_end()) {
      throw ExprError("unexpected end of input");
    }

    char c = peek();
    if (c == '(') {
      pos_++;
      double value = parse_expr();
      skip_space();
      if (!eat(')')) {
        throw ExprError("expected ')'");
      }
      return value;
    }
    if (is_ident_start(c)) {
      return parse_name();
    }
    if ((c >= '0' && c <= '9') || c == '.') {
      return parse_number();
    }
    throw ExprError(std::format("unexpected character: '{}'", c));
  }

  double parse_name() {
    std::string_view name = take_ident();
    skip_space();
    if (eat('(')) {
      double arg = parse_expr();
      skip_space();
      if (!eat(')')) {
        throw ExprError("expected ')'");
      }
      return check_finite(apply(name, arg));
    }

    auto it = variables_.find(name);
    if (it == variables_.end()) {
      throw ExprError(std::format("unknown name: '{}'", name));
    }
    return it->second;
  }

  double parse_number() {
    const char *first = text_.data() + pos_;
    const char *last = text_.data() + text_.size();

    // from_chars is the one number parser that takes a bounded range instead of
    // a NUL-terminated string, which is what lets this work on a string_view
    // into the middle of the input without copying.
    double value = 0.0;
    auto [end, ec] = std::from_chars(first, last, value);
    if (ec == std::errc::result_out_of_range) {
      throw ExprError(std::format("number is out of range: '{}'",
                                  std::string_view(first, end)));
    }
    if (ec != std::errc{}) {
      throw ExprError(
          std::format("not a number: '{}'", std::string_view(first, last)));
    }
    pos_ = static_cast<std::size_t>(end - text_.data());
    return value;
  }

  // nonzero - guards a divisor, so 1/0 is an error rather than an infinity.
  static double nonzero(double divisor) {
    if (divisor == 0.0) {
      throw ExprError("division by zero");
    }
    return divisor;
  }

  bool at_end() const { return pos_ >= text_.size(); }

  char peek() const { return text_[pos_]; }

  void skip_space() {
    while (!at_end() && is_space(peek())) {
      pos_++;
    }
  }

  // eat - consumes c if it is next. Callers skip whitespace first.
  bool eat(char c) {
    if (!at_end() && peek() == c) {
      pos_++;
      return true;
    }
    return false;
  }

  std::string_view take_ident() {
    std::size_t start = pos_;
    while (!at_end() && is_ident_char(peek())) {
      pos_++;
    }
    return text_.substr(start, pos_ - start);
  }

  std::string_view text_;
  Variables &variables_;
  std::size_t pos_ = 0;
  int depth_ = 0;
  // Assignments awaiting a successful parse; see parse(). Ordered inner-to-
  // outer for a chain like `a = b = 1`, which does not matter -- they all carry
  // the same value -- but is worth knowing if that ever stops being true.
  std::vector<std::pair<std::string, double>> pending_;
};

} // namespace

double evaluate(std::string_view text) {
  Evaluator evaluator;
  return evaluator.eval(text);
}

std::string format_value(double value) { return std::format("{}", value); }

Evaluator::Evaluator() { clear(); }

double Evaluator::eval(std::string_view text) {
  return Parser(text, variables_).parse();
}

bool Evaluator::has(std::string_view name) const {
  return variables_.find(name) != variables_.end();
}

double Evaluator::get(std::string_view name) const {
  auto it = variables_.find(name);
  if (it == variables_.end()) {
    throw ExprError(std::format("unknown name: '{}'", name));
  }
  return it->second;
}

void Evaluator::set(std::string_view name, double value) {
  if (!std::isfinite(value)) {
    throw ExprError(std::format("value is not finite: {}", value));
  }
  variables_[std::string(name)] = value;
}

std::vector<std::string> Evaluator::names() const {
  std::vector<std::string> out;
  out.reserve(variables_.size());
  for (const auto &entry : variables_) {
    out.push_back(entry.first); // std::map iterates in sorted key order
  }
  return out;
}

void Evaluator::clear() {
  variables_.clear();
  variables_["pi"] = std::numbers::pi;
  variables_["e"] = std::numbers::e;
}

} // namespace exprkit
