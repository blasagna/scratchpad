//! Integration tests: the crate as an outside consumer sees it.
//!
//! These link against `exprkit` as a dependency rather than compiling it, so
//! they catch anything that is visible from inside `src/lib.rs` but not from
//! outside it -- a type left private, a trait impl that never made it into the
//! public API, a link error that only shows up in a downstream binary.
//!
//! As in the unit tests, the arithmetic is not re-checked here; that belongs to
//! //cpp_rust_bindings/cpp:test_exprkit.

use std::error::Error;

use exprkit::{Evaluator, ExprError, evaluate, format_value};

#[test]
fn the_public_api_is_reachable_and_the_cpp_links() {
    assert_eq!(evaluate("6 * 7").unwrap(), 42.0);
    assert_eq!(format_value(42.0), "42");
    assert_eq!(Evaluator::new().names(), ["e", "pi"]);
    assert_eq!(Evaluator::default().names(), ["e", "pi"]);
}

#[test]
fn expr_error_is_a_usable_error_type() {
    let err: ExprError = evaluate("1 / 0").unwrap_err();

    // Display, Debug, Clone, PartialEq, and std::error::Error all have to be
    // present for callers to handle this like any other Rust error.
    assert_eq!(err.to_string(), "division by zero");
    assert_eq!(err.clone(), err);
    assert!(format!("{err:?}").contains("division by zero"));
    assert!(err.source().is_none());

    let boxed: Box<dyn Error> = Box::new(err);
    assert_eq!(boxed.to_string(), "division by zero");
}

#[test]
fn an_evaluator_is_a_normal_owned_rust_value() {
    // Moving one out of a function, into a Vec, and back out again exercises
    // the UniquePtr wrapper: the C++ object never moves, only the pointer does.
    fn primed() -> Evaluator {
        let mut evaluator = Evaluator::new();
        evaluator.eval("x = 21").unwrap();
        evaluator
    }

    let mut evaluators = vec![primed(), primed()];
    evaluators[0].eval("x = 1").unwrap();

    assert_eq!(evaluators[0].get("x").unwrap(), 1.0);
    assert_eq!(evaluators[1].get("x").unwrap(), 21.0);

    let mut moved = evaluators.pop().unwrap();
    assert_eq!(moved.eval("x * 2").unwrap(), 42.0);
}

#[test]
fn many_evaluators_are_created_and_dropped_without_leaking() {
    // Not a leak *check* -- there is no allocator hook here -- but it does run
    // the C++ constructor and destructor a few thousand times, which is enough
    // for a double free or a missing deleter to abort the test binary.
    for i in 0..5_000 {
        let mut evaluator = Evaluator::new();
        evaluator.set("i", f64::from(i)).unwrap();
        assert_eq!(evaluator.eval("i + 1").unwrap(), f64::from(i) + 1.0);
    }
}

#[test]
fn a_failed_call_leaves_the_environment_untouched() {
    let mut evaluator = Evaluator::new();
    evaluator.eval("x = 1").unwrap();

    assert!(evaluator.eval("y = 1 / 0").is_err());
    assert!(evaluator.eval("z = nope").is_err());

    // These two fail only *after* the assignment's value is computed, so they
    // are what actually exercises the deferral in the C++ Parser rather than
    // passing because the parse died early.
    assert!(evaluator.eval("w = 1 2").is_err());
    assert!(evaluator.eval("p = q = 1 2").is_err());

    // A C++ exception unwound through the binding without corrupting the
    // object it was thrown from, which is the property that matters after an
    // Err: the Evaluator is still usable.
    for name in ["y", "z", "w", "p", "q"] {
        assert!(!evaluator.has(name), "`{name}` should not be defined");
    }
    assert_eq!(evaluator.get("x").unwrap(), 1.0);
    assert_eq!(evaluator.eval("x + 1").unwrap(), 2.0);
}

#[test]
fn deeply_nested_input_is_an_err_not_a_crash() {
    // Without the depth cap in the C++ parser this overflows the stack, which
    // is an abort no `Result` can intercept -- the one failure mode that would
    // defeat the whole "every error arrives as an Err" premise. Reachable from
    // the library API, not just the CLI, so it belongs in the crate's tests.
    assert!(evaluate(&("(".repeat(100_000) + "1")).is_err());
    assert!(evaluate(&("-".repeat(100_000) + "1")).is_err());

    let mut evaluator = Evaluator::new();
    assert!(evaluator.eval(&("a=".repeat(100_000) + "1")).is_err());

    // Still usable afterwards, and ordinary nesting still works.
    assert_eq!(evaluator.eval("((((1 + 2))))").unwrap(), 3.0);
}

#[test]
fn the_two_clis_share_one_formatter() {
    // //cpp_rust_bindings/cpp:test_exprkit pins these same four strings against
    // the C++ format_value. Both sides calling one implementation is what makes
    // the CLIs byte-identical; these assertions are the tripwire for anyone who
    // "simplifies" this crate by formatting in Rust instead.
    assert_eq!(format_value(1.0), "1");
    assert_eq!(format_value(-0.5), "-0.5");
    assert_eq!(format_value(0.1 + 0.2), "0.30000000000000004");
    assert_eq!(format_value(1e21), "1e+21");
}
