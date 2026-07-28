#ifndef CPP_RUST_BINDINGS_CPP_EXPRKIT_HPP
#define CPP_RUST_BINDINGS_CPP_EXPRKIT_HPP

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// A small arithmetic expression evaluator.
//
// This is the *core* library: plain C++20, with no idea that Rust exists. The
// binding layer lives in ../rust/src/shim.{hpp,cpp} and depends on this header,
// never the other way round. Keeping the split means this code stays an
// ordinary C++ library -- buildable and testable with Bazel alone -- and the
// glue stays visibly thin.
namespace exprkit {

// Every failure this library reports.
//
// Deriving from std::runtime_error is what lets the Rust bindings turn it into
// an Err without naming the type: cxx wraps each fallible call in a try/catch
// for std::exception and forwards what().
class ExprError : public std::runtime_error {
public:
  explicit ExprError(const std::string &message)
      : std::runtime_error(message) {}
};

// evaluate - evaluates one expression in a scratch environment.
//
// Equivalent to Evaluator{}.eval(text): the constants pi and e are in scope,
// and an assignment is accepted but its effect is discarded when the temporary
// environment dies. Throws ExprError.
double evaluate(std::string_view text);

// format_value - renders a result the way the CLIs print it.
//
// Uses std::format's default for double, the shortest representation that
// round-trips: 1.0 prints as "1", 0.1 + 0.2 as "0.30000000000000004". Both the
// C++ CLI and the Rust CLI call this one function, which is why their output
// cannot drift apart -- there is no second formatter to keep in sync.
std::string format_value(double value);

// A calculator with a persistent set of named variables.
//
// Freshly constructed -- and after clear() -- it holds the two constants "pi"
// and "e". They are ordinary variables and may be reassigned.
class Evaluator {
public:
  Evaluator();

  // eval - evaluates text against this environment, updating it.
  //
  // The grammar is, loosest binding to tightest:
  //
  //   program := (name "=")* expr
  //   expr    := term (("+" | "-") term)*
  //   term    := unary (("*" | "/" | "%") unary)*
  //   unary   := ("+" | "-") unary | power
  //   power   := primary ("^" unary)?      // right associative
  //   primary := number | name | name "(" expr ")" | "(" expr ")"
  //
  // Placing unary above power is what makes -2^2 evaluate to -4 rather than 4,
  // and letting power's right side be a unary is what allows 2^-1. A name
  // followed by "(" is always a call, so a variable cannot be juxtaposed with a
  // parenthesized group; there is no implicit multiplication.
  //
  // The recognized functions are sqrt, abs, floor, ceil, ln, exp, sin, and cos.
  //
  // Throws ExprError on a syntax error, an unknown name, division by zero, an
  // intermediate result that is not finite, or nesting more than about 250
  // levels deep (a cap that exists so deep input is an error rather than a
  // stack overflow, which no amount of care on the caller's part can catch).
  //
  // Strongly exception-safe: a throw leaves the environment exactly as it was,
  // including when the input assigns before it fails ("x = 1 2" defines
  // nothing). Assignments are applied only once the whole input has parsed.
  double eval(std::string_view text);

  // has - reports whether name is currently defined.
  bool has(std::string_view name) const;

  // get - returns the value bound to name. Throws ExprError if it is unset.
  double get(std::string_view name) const;

  // set - binds name to value. Throws ExprError if value is not finite, so the
  // environment can never hold a value that would poison later arithmetic.
  void set(std::string_view name, double value);

  // names - returns every defined name, sorted.
  std::vector<std::string> names() const;

  // clear - forgets every variable and restores the pi and e constants.
  void clear();

private:
  // std::less<> makes the map heterogeneously comparable, so a lookup keyed by
  // string_view does not have to allocate a std::string first.
  std::map<std::string, double, std::less<>> variables_;
};

} // namespace exprkit

#endif // CPP_RUST_BINDINGS_CPP_EXPRKIT_HPP
