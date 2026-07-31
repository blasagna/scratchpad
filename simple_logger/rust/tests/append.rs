//! Filesystem tests for the append path. Everything pure lives in `src/lib.rs`.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use simple_logger::{Format, LogError, append_lines, append_messages};

/// A unique path under the crate's test temp dir, so tests can run in parallel
/// and repeated runs never see a previous run's file.
fn tmp_path(name: &str) -> PathBuf {
    static COUNTER: AtomicU32 = AtomicU32::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    let pid = std::process::id();
    Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("log_{pid}_{n}_{name}"))
}

fn read(path: &Path) -> String {
    fs::read_to_string(path).expect("log file should be readable")
}

#[test]
fn creates_the_log_file_when_missing() {
    let path = tmp_path("creates.txt");
    append_messages(&path, &Format::default(), "TS", &["hello"]).unwrap();
    assert_eq!(read(&path), "[TS] [INFO] hello\n");
}

#[test]
fn appends_without_truncating_existing_entries() {
    let path = tmp_path("appends.txt");
    fs::write(&path, "existing\n").unwrap();

    append_messages(&path, &Format::default(), "TS", &["hello"]).unwrap();
    assert_eq!(read(&path), "existing\n[TS] [INFO] hello\n");
}

#[test]
fn two_runs_append_in_order() {
    let path = tmp_path("two_runs.txt");
    let fmt = Format::default();

    append_messages(&path, &fmt, "T1", &["first"]).unwrap();
    append_messages(&path, &fmt, "T2", &["second"]).unwrap();
    assert_eq!(read(&path), "[T1] [INFO] first\n[T2] [INFO] second\n");
}

#[test]
fn creates_the_log_file_even_with_no_entries() {
    let path = tmp_path("no_entries.txt");
    let none: [&str; 0] = [];

    append_messages(&path, &Format::default(), "TS", &none).unwrap();
    assert_eq!(read(&path), "");
    assert!(
        path.exists(),
        "the file is created before anything is written"
    );
}

#[test]
fn appends_one_entry_per_line_from_a_reader() {
    let path = tmp_path("lines.txt");
    let mut input = &b"a\r\nb\n\nc"[..];

    append_lines(&path, &Format::default(), "TS", &mut input).unwrap();
    assert_eq!(
        read(&path),
        "[TS] [INFO] a\n[TS] [INFO] b\n[TS] [INFO] \n[TS] [INFO] c\n"
    );
}

#[test]
fn empty_input_still_creates_the_file() {
    let path = tmp_path("empty_stdin.txt");
    let mut input = &b""[..];

    append_lines(&path, &Format::default(), "TS", &mut input).unwrap();
    assert_eq!(read(&path), "");
}

#[test]
fn reports_an_open_error_when_the_parent_directory_is_missing() {
    let path = tmp_path("no_such_dir").join("log.txt");
    let err = append_messages(&path, &Format::default(), "TS", &["hello"])
        .expect_err("a missing parent directory cannot be created implicitly");
    assert!(
        matches!(err, LogError::Open { .. }),
        "expected Open, got {err:?}"
    );
}

#[test]
fn reports_an_open_error_for_a_directory() {
    let path = tmp_path("a_directory");
    fs::create_dir_all(&path).unwrap();

    let err = append_messages(&path, &Format::default(), "TS", &["hello"])
        .expect_err("a directory cannot be appended to");
    assert!(
        matches!(err, LogError::Open { .. }),
        "expected Open, got {err:?}"
    );
}

#[test]
fn reports_an_open_error_for_an_unwritable_file() {
    use std::os::unix::fs::PermissionsExt;

    let path = tmp_path("unwritable.txt");
    fs::write(&path, "").unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o444)).unwrap();

    let result = append_messages(&path, &Format::default(), "TS", &["hello"]);
    // Root bypasses permission checks; skip the assertion in that case.
    if let Err(err) = result {
        assert!(
            matches!(err, LogError::Open { .. }),
            "expected Open, got {err:?}"
        );
    }
}
