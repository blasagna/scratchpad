//! End-to-end tests for the `mini_shell` binary.
//!
//! `mini_shell/check_parity.sh` runs this port alongside the C and C++ ones and
//! diffs them, but it can only assert that the ports *agree*. What it cannot
//! cover is pinned here: the command-line surface, which is clap's and so
//! deliberately differs from the other two, and the unbuffered read, which the
//! script checks against a fixed expectation for both ports but which is worth
//! catching from `cargo test` before it gets that far.

use std::io::Write;
use std::process::{Command, Output, Stdio};

const BIN: &str = env!("CARGO_BIN_EXE_mini_shell");

/// clap's code for a bad command line, which is also the C port's.
const EXIT_USAGE: i32 = 2;

fn run(args: &[&str]) -> Output {
    run_with_stdin(args, "")
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

fn stdout_of(args: &[&str], stdin: &str) -> String {
    let out = run_with_stdin(args, stdin);
    assert_eq!(
        out.status.code(),
        Some(0),
        "{args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout).expect("utf-8 stdout")
}

fn assert_usage_error(args: &[&str]) {
    let out = run(args);
    assert_eq!(out.status.code(), Some(EXIT_USAGE), "{args:?}");
    assert!(
        !out.stderr.is_empty(),
        "{args:?} was rejected with no diagnostic"
    );
    assert!(out.stdout.is_empty(), "{args:?} wrote to stdout on failure");
}

// --- the option surface, which is clap's ---

#[test]
fn help_exits_zero() {
    let out = run(&["--help"]);
    assert_eq!(out.status.code(), Some(0));
    let help = String::from_utf8(out.stdout).expect("utf-8 stdout");
    assert!(help.contains("--no-banner"));
    // No prompt and no banner: --help never reaches the loop.
    assert!(!help.contains("$ "));
}

#[test]
fn an_unknown_option_is_a_usage_error() {
    assert_usage_error(&["--nope"]);
    assert_usage_error(&["-z"]);
}

#[test]
fn a_stray_operand_is_a_usage_error() {
    // Commands come from stdin, never from argv, so this is a mistake worth
    // naming rather than something to ignore.
    assert_usage_error(&["ls"]);
}

#[test]
fn an_abbreviated_long_option_is_rejected() {
    // getopt_long matches unambiguous prefixes and the C port therefore accepts
    // this; clap does not infer them, so this port matches the C++ one. See the
    // divergence table in mini_shell/README.md.
    assert_usage_error(&["--no-ban"]);
}

// --- the loop, end to end ---

#[test]
fn the_banner_prints_unless_no_banner_is_given() {
    let with = stdout_of(&[], "exit\n");
    let without = stdout_of(&["--no-banner"], "exit\n");

    assert!(with.contains("type 'exit' to quit"));
    assert_eq!(without, "$ ");
    assert!(with.ends_with("$ "));
}

#[test]
fn a_failing_command_is_reported_but_does_not_change_the_exit_code() {
    let out = run_with_stdin(&["--no-banner"], "false\nexit\n");

    assert_eq!(out.status.code(), Some(0));
    assert_eq!(
        String::from_utf8_lossy(&out.stderr),
        "mini_shell: command exited with status 1\n"
    );
}

#[test]
fn a_missing_program_is_named_not_found() {
    // End to end, this is the message the exec errno pipe exists to produce,
    // and it must be the same bytes the C and C++ ports write.
    let out = run_with_stdin(&["--no-banner"], "nosuchcommand_xyzzy\nexit\n");

    assert_eq!(out.status.code(), Some(0));
    assert_eq!(
        String::from_utf8_lossy(&out.stderr),
        "mini_shell: nosuchcommand_xyzzy: command not found\n"
    );
}

#[test]
fn shell_metacharacters_reach_the_program_as_arguments() {
    // No interpreter in between, so the pipe is just a word echo prints.
    let out = stdout_of(&["--no-banner"], "echo a | wc\nexit\n");

    assert_eq!(out, "$ a | wc\n$ ");
}

#[test]
fn a_command_that_reads_stdin_gets_the_unconsumed_input() {
    // The one behavior no unit test can reach: the suite in src/lib.rs drives
    // the loop with an in-memory stream, where buffering is invisible. Reading
    // ahead would hand `cat` an empty stdin and leave mini_shell to run
    // `echo done` itself, so the output would say "done" instead of the
    // "echo done" that cat echoed.
    let out = stdout_of(&["--no-banner"], "cat\necho done\n");

    assert!(
        out.contains("echo done"),
        "mini_shell read ahead; cat never saw the second line: {out:?}"
    );
}
