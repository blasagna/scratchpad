//! The one test that really forks.
//!
//! It pins the assumption behind the wait statuses the unit suite builds with
//! `ExitStatus::from_raw`: if a real `/bin/sh` result decoded differently from a
//! hand-built one, every status in `src/lib.rs`'s tests would be meaningless.
//! Kept out of the unit suite for the same reason the C and C++ ports isolate
//! `RealSystem.EncodingMatchesTheMacros` — everything else runs against a
//! recording fake and spawns nothing.

use std::os::unix::process::ExitStatusExt;
use std::process::ExitStatus;

use mini_shell::{Runner, Status, SystemRunner, decode_status, interpreter_available};

/// What the unit suite means by "exited with this code".
fn exited(code: i32) -> ExitStatus {
    ExitStatus::from_raw(code << 8)
}

/// What the unit suite means by "killed by this signal".
fn signaled(signal: i32) -> ExitStatus {
    ExitStatus::from_raw(signal)
}

fn run(command: &str) -> Status {
    decode_status(SystemRunner.run(command.as_bytes()))
}

#[test]
fn a_real_shell_encodes_a_wait_status_the_way_the_tests_build_one() {
    assert!(matches!(run("exit 3"), Status::Exited(3)));
    assert!(matches!(decode_status(Ok(exited(3))), Status::Exited(3)));

    assert!(matches!(run("exit 0"), Status::Exited(0)));
    assert!(matches!(decode_status(Ok(exited(0))), Status::Exited(0)));

    // A signal, not an exit code: read as one, this would report as
    // "exited with status 0".
    assert!(matches!(run("kill -9 $$"), Status::Signaled(9)));
    assert!(matches!(
        decode_status(Ok(signaled(9))),
        Status::Signaled(9)
    ));
}

#[test]
fn a_command_not_found_is_an_ordinary_exit_of_127() {
    // The interpreter prints its own message and exits 127; mini_shell reports
    // the status after it rather than special-casing the value.
    assert!(matches!(run("nosuchcommand_xyzzy"), Status::Exited(127)));
}

#[test]
fn the_interpreter_probe_finds_the_shell_this_machine_has() {
    assert!(interpreter_available());
}
