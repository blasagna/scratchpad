#ifndef CPP_RUST_BINDINGS_RUST_SRC_SHIM_HPP
#define CPP_RUST_BINDINGS_RUST_SRC_SHIM_HPP

#include <memory>

#include "exprkit.hpp"
#include "rust/cxx.h"

// The binding layer: the only code in this project that knows both languages
// exist. It is the C++ counterpart of the `#[cxx::bridge]` module in lib.rs.
//
// ../../cpp/exprkit.hpp is deliberately cxx-free, which leaves two mismatches
// for this file to absorb:
//
//   * Strings. cxx passes borrowed strings as rust::Str and owned ones as
//     rust::String; the library speaks std::string_view and std::string. There
//     is no implicit conversion in either direction, so every signature that
//     carries a string needs an adapter.
//
//   * Member functions. cxx can bind a C++ method directly, but only when its
//     signature already matches the Rust one it is declared with.
//     Evaluator::eval(std::string_view) does not, so the methods are
//     re-exported here as free functions taking the object by reference -- the
//     shape cxx maps to `Pin<&mut Evaluator>` and `&Evaluator`.
//
// Errors need no adapter at all. cxx wraps every function declared to return
// `Result` in a try/catch for std::exception and hands what() to Rust, and
// ExprError derives from std::runtime_error. Note that this holds for *any*
// std::exception, so a std::bad_alloc from deep inside the library also arrives
// as an Err rather than crossing the boundary and aborting.
namespace exprkit::bridge {

double evaluate(rust::Str text);
rust::String format_value(double value);

std::unique_ptr<Evaluator> new_evaluator();
double eval(Evaluator &evaluator, rust::Str text);
bool has(const Evaluator &evaluator, rust::Str name);
double get(const Evaluator &evaluator, rust::Str name);
void set(Evaluator &evaluator, rust::Str name, double value);
rust::Vec<rust::String> names(const Evaluator &evaluator);
void clear(Evaluator &evaluator);

} // namespace exprkit::bridge

#endif // CPP_RUST_BINDINGS_RUST_SRC_SHIM_HPP
