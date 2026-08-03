//! End-to-end tests for the `matrix_ops` binary.
//!
//! The C and C++ ports lean on `matrix_ops/check_parity.sh` to pin their
//! command-line behavior against each other. This port is not in that script —
//! it uses clap and so its diagnostics differ deliberately — so the surface the
//! script would have covered is pinned here instead.

use std::io::Write;
use std::process::{Command, Output, Stdio};

const BIN: &str = env!("CARGO_BIN_EXE_matrix_ops");

const EXIT_USAGE: i32 = 2;
const EXIT_FAILURE: i32 = 1;

fn run(args: &[&str]) -> Output {
    Command::new(BIN).args(args).output().expect("spawn")
}

fn run_with_stdin(args: &[&str], stdin: &str) -> Output {
    let mut child = Command::new(BIN)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn");
    child
        .stdin
        .take()
        .expect("stdin")
        .write_all(stdin.as_bytes())
        .expect("write stdin");
    child.wait_with_output().expect("wait")
}

/// Asserts a successful run and returns its stdout.
fn stdout_of(args: &[&str]) -> String {
    let out = run(args);
    assert!(
        out.status.success(),
        "{args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout).expect("utf-8 stdout")
}

fn assert_exit(args: &[&str], code: i32) {
    let out = run(args);
    assert_eq!(out.status.code(), Some(code), "{args:?}");
    if code != 0 {
        assert!(
            !out.stderr.is_empty(),
            "{args:?} exited {code} with no diagnostic"
        );
        assert!(out.stdout.is_empty(), "{args:?} wrote to stdout on failure");
    }
}

// --- the four operations ---

#[test]
fn adds_and_subtracts() {
    assert_eq!(
        stdout_of(&["add", "--values", "1 2 3", "--values", "4 5 6"]),
        "5  7  9\n"
    );
    assert_eq!(
        stdout_of(&["sub", "--values", "1 2", "--values", "10 20"]),
        " -9  -18\n"
    );
}

#[test]
fn multiplies_operands_of_different_shapes() {
    let out = stdout_of(&[
        "mul",
        "--rows",
        "2",
        "--cols",
        "3",
        "--rows",
        "3",
        "--cols",
        "2",
        "--values",
        "1 2 3 4 5 6",
        "--values",
        "7 8 9 10 11 12",
    ]);
    assert_eq!(out, " 58   64\n139  154\n");
}

#[test]
fn scales_by_the_scalar() {
    assert_eq!(
        stdout_of(&["scale", "--scalar", "2.5", "--values", "1 2\n3 4"]),
        "2.5    5\n7.5   10\n"
    );
}

// --- operand sources ---

#[test]
fn reads_an_operand_from_stdin() {
    let out = run_with_stdin(&["scale", "-k", "-1", "--file", "-"], "1 2\n3 4\n");
    assert!(out.status.success());
    assert_eq!(String::from_utf8_lossy(&out.stdout), "-1  -2\n-3  -4\n");
}

#[test]
fn a_missing_file_is_an_operational_failure() {
    assert_exit(
        &["scale", "-k", "2", "--file", "/nonexistent/matrix"],
        EXIT_FAILURE,
    );
}

// --- argument spellings ---

#[test]
fn values_may_begin_with_a_minus() {
    // `allow_hyphen_values` is what keeps clap from reading "-1 2" as a flag.
    assert_eq!(
        stdout_of(&["scale", "-k", "1", "--values", "-1 2 -3"]),
        "-1   2  -3\n"
    );
}

#[test]
fn the_scalar_may_be_negative() {
    assert_eq!(
        stdout_of(&["scale", "--scalar", "-2.5", "--values", "2"]),
        "-5\n"
    );
}

#[test]
fn short_attached_and_separate_spellings_agree() {
    let separate = stdout_of(&["scale", "--rows", "3", "--values", "1 2 3 4 5 6", "-k", "1"]);
    let attached = stdout_of(&["scale", "--rows=3", "--values", "1 2 3 4 5 6", "-k", "1"]);
    let short = stdout_of(&["scale", "-r3", "--values", "1 2 3 4 5 6", "-k", "1"]);
    assert_eq!(separate, attached);
    assert_eq!(separate, short);
}

#[test]
fn dimensions_pair_with_operands_by_index() {
    // The Nth --rows describes the Nth operand, wherever it appears on the
    // command line. This is the port's documented divergence from C, where the
    // dimensions must precede the operand they describe.
    let before = stdout_of(&["scale", "--rows", "3", "--values", "1 2 3 4 5 6", "-k", "1"]);
    let after = stdout_of(&["scale", "--values", "1 2 3 4 5 6", "--rows", "3", "-k", "1"]);
    assert_eq!(before, after);
    assert_eq!(before, "1  2\n3  4\n5  6\n");
}

#[test]
fn inline_operands_are_ordered_before_file_ones() {
    // mul is not commutative, so this is observable: the inline operand is the
    // left-hand one even though --file was written first.
    let dir = std::env::temp_dir().join(format!("matrix_ops_cli_{}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("temp dir");
    let path = dir.join("b.txt");
    std::fs::write(&path, "0 1\n0 0\n").expect("write fixture");

    let out = stdout_of(&[
        "mul",
        "--file",
        path.to_str().unwrap(),
        "--values",
        "1 2\n3 4",
    ]);
    // [[1,2],[3,4]] * [[0,1],[0,0]] = [[0,1],[0,3]]
    assert_eq!(out, "0  1\n0  3\n");

    std::fs::remove_dir_all(&dir).ok();
}

// --- precision ---

#[test]
fn precision_controls_the_decimal_places() {
    assert_eq!(
        stdout_of(&["scale", "-k", "1", "--values", "0.25 0.45", "-p", "1"]),
        "0.2  0.5\n"
    );
    assert_eq!(
        stdout_of(&["scale", "-k", "1", "--values", "1.5 2.4", "-p", "0"]),
        "2  2\n"
    );
}

#[test]
fn precision_is_capped() {
    assert!(
        run(&["scale", "-k", "1", "--values", "1", "-p", "1100"])
            .status
            .success()
    );
    assert_exit(
        &["scale", "-k", "1", "--values", "1", "-p", "1101"],
        EXIT_USAGE,
    );
}

// --- usage errors ---

#[test]
fn the_operand_count_must_match_the_operation() {
    assert_exit(&["add", "--values", "1"], EXIT_USAGE);
    assert_exit(
        &["add", "--values", "1", "--values", "2", "--values", "3"],
        EXIT_USAGE,
    );
    assert_exit(
        &["scale", "-k", "2", "--values", "1", "--values", "2"],
        EXIT_USAGE,
    );
}

#[test]
fn scale_requires_a_scalar_and_the_others_reject_one() {
    assert_exit(&["scale", "--values", "1"], EXIT_USAGE);
    assert_exit(
        &["add", "--values", "1", "--values", "2", "-k", "2"],
        EXIT_USAGE,
    );
}

#[test]
fn mismatched_shapes_are_a_usage_error() {
    // Always traceable to what was typed, so exit 2 rather than 1 — the same
    // call the C and C++ ports make.
    assert_exit(
        &[
            "add", "--rows", "2", "--values", "1 2 3 4", "--values", "1 2 3 4",
        ],
        EXIT_USAGE,
    );
    assert_exit(&["mul", "--values", "1 2", "--values", "3 4"], EXIT_USAGE);
}

#[test]
fn bad_input_is_a_usage_error() {
    assert_exit(&["scale", "-k", "1", "--values", "abc"], EXIT_USAGE);
    assert_exit(&["scale", "-k", "1", "--values", ""], EXIT_USAGE);
    assert_exit(&["scale", "-k", "1", "--values", "1 2 3\n4 5"], EXIT_USAGE);
    assert_exit(&["scale", "-k", "1", "--values", "inf"], EXIT_USAGE);
    assert_exit(&["scale", "-k", "1", "--values", "1e400"], EXIT_USAGE);
    assert_exit(&["bogus", "--values", "1"], EXIT_USAGE);
    assert_exit(
        &["scale", "-k", "1", "--values", "1", "--rows", "0"],
        EXIT_USAGE,
    );
    assert_exit(
        &["scale", "-k", "1", "--values", "1 2 3", "--rows", "2"],
        EXIT_USAGE,
    );
}

#[test]
fn more_dimensions_than_operands_is_a_usage_error() {
    assert_exit(
        &[
            "scale", "-k", "1", "--values", "1 2", "--rows", "1", "--rows", "1",
        ],
        EXIT_USAGE,
    );
}

#[test]
fn help_exits_zero() {
    let out = run(&["--help"]);
    assert!(out.status.success());
    assert!(String::from_utf8_lossy(&out.stdout).contains("--values"));
}
