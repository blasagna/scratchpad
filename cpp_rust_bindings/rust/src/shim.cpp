#include "shim.hpp"

#include <string>
#include <string_view>

namespace exprkit::bridge {
namespace {

// view - borrows a rust::Str as a std::string_view.
//
// rust::Str is a pointer/length pair over UTF-8 bytes, which is exactly what
// string_view wraps, so this copies nothing. The borrow is valid for the
// duration of the call: Rust guarantees the &str outlives it, and none of these
// functions store the view anywhere.
std::string_view view(rust::Str text) {
  return std::string_view(text.data(), text.size());
}

} // namespace

// Every call below qualifies the library function it forwards to. Unqualified
// name lookup inside `exprkit::bridge` finds the bridge overload first, and
// `evaluate(view(text))` would quietly recurse until the stack ran out.

double evaluate(rust::Str text) { return exprkit::evaluate(view(text)); }

rust::String format_value(double value) {
  return rust::String(exprkit::format_value(value));
}

std::unique_ptr<Evaluator> new_evaluator() {
  // cxx's UniquePtr<Evaluator> is std::unique_ptr<exprkit::Evaluator>, so
  // ownership moves to Rust with no wrapper object in between. The Rust side
  // drops it by calling ~Evaluator through a generated deleter.
  return std::make_unique<Evaluator>();
}

double eval(Evaluator &evaluator, rust::Str text) {
  return evaluator.eval(view(text));
}

bool has(const Evaluator &evaluator, rust::Str name) {
  return evaluator.has(view(name));
}

double get(const Evaluator &evaluator, rust::Str name) {
  return evaluator.get(view(name));
}

void set(Evaluator &evaluator, rust::Str name, double value) {
  evaluator.set(view(name), value);
}

rust::Vec<rust::String> names(const Evaluator &evaluator) {
  // rust::Vec is a real Rust Vec, allocated by Rust's allocator through a
  // generated hook. Building it here and returning it by value hands Rust an
  // owned Vec<String> with no second copy on the far side.
  rust::Vec<rust::String> out;
  for (const std::string &name : evaluator.names()) {
    out.push_back(rust::String(name));
  }
  return out;
}

void clear(Evaluator &evaluator) { evaluator.clear(); }

} // namespace exprkit::bridge
