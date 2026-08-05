//! The one test that really forks.
//!
//! It pins the assumption behind the wait statuses the unit suite builds with
//! `ExitStatus::from_raw`: if a real process's result decoded differently from
//! a hand-built one, every status in `src/lib.rs`'s tests would be meaningless.
//! Kept out of the unit suite for the same reason the C and C++ ports isolate
//! `RealExec.EncodingMatchesTheMacros` — everything else runs against a
//! recording fake and spawns nothing.

use std::os::unix::process::ExitStatusExt;
use std::process::ExitStatus;

use mini_shell::{ExecRunner, Runner, Status, decode_status};

/// What the unit suite means by "exited with this code".
fn exited(code: i32) -> ExitStatus {
    ExitStatus::from_raw(code << 8)
}

/// What the unit suite means by "killed by this signal".
fn signaled(signal: i32) -> ExitStatus {
    ExitStatus::from_raw(signal)
}

/// Runs argv for real and decodes the result. These tests can hand the runner
/// an argv the splitter could never produce, which is how `exit 3` arrives as
/// one argument here.
fn run(argv: &[&str]) -> Status {
    let words: Vec<&[u8]> = argv.iter().map(|word| word.as_bytes()).collect();
    decode_status(ExecRunner.run(&words))
}

#[test]
fn a_real_process_encodes_a_wait_status_the_way_the_tests_build_one() {
    assert!(matches!(
        run(&["/bin/sh", "-c", "exit 3"]),
        Status::Exited(3)
    ));
    assert!(matches!(decode_status(Ok(exited(3))), Status::Exited(3)));

    assert!(matches!(
        run(&["/bin/sh", "-c", "exit 0"]),
        Status::Exited(0)
    ));
    assert!(matches!(decode_status(Ok(exited(0))), Status::Exited(0)));

    // A signal, not an exit code: read as one, this would report as
    // "exited with status 0".
    assert!(matches!(
        run(&["/bin/sh", "-c", "kill -9 $$"]),
        Status::Signaled(9)
    ));
    assert!(matches!(
        decode_status(Ok(signaled(9))),
        Status::Signaled(9)
    ));
}

#[test]
fn a_program_is_found_on_path() {
    // No absolute path: Command searches PATH the way execvp does, which is
    // what makes `ls` work at the prompt without the shell that used to do the
    // looking.
    assert!(matches!(run(&["true"]), Status::Exited(0)));
}

#[test]
fn a_missing_program_is_reported_rather_than_exiting_127() {
    // The exec fails in the child, and Command relays the errno back over a
    // close-on-exec pipe — the same mechanism the C and C++ ports spell out.
    // Without it the parent would see only an exit status of 127 and could not
    // tell it from a command that really exited 127.
    assert!(matches!(run(&["nosuchcommand_xyzzy"]), Status::NotFound));
}

#[test]
fn a_program_that_may_not_be_executed_is_reported() {
    assert!(matches!(run(&["/etc/passwd"]), Status::NotExecutable));
}
