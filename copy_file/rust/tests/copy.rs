//! Integration tests for the file copier, exercising real files under the
//! per-target temp dir that cargo provides via CARGO_TARGET_TMPDIR.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use copy_file::{CopyError, CopyMethod, copy, resolve_destination};

/// A unique writable path under the cargo-provided temp dir. The pid keeps
/// reruns from colliding with leftover files (e.g. a prior run's read-only
/// fixture), and the counter keeps parallel tests within a run distinct.
fn tmp_path(name: &str) -> PathBuf {
    static COUNTER: AtomicU32 = AtomicU32::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    let pid = std::process::id();
    Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("copy_{pid}_{n}_{name}"))
}

fn write(path: &Path, content: &[u8]) {
    fs::write(path, content).expect("write test fixture");
}

#[test]
fn round_trip_stream() {
    let src = tmp_path("rt_src.txt");
    let dst = tmp_path("rt_dst.txt");
    write(&src, b"line one\nline two\n");

    let out = copy(
        src.to_str().unwrap(),
        dst.to_str().unwrap(),
        CopyMethod::Stream,
    )
    .unwrap();
    assert_eq!(out, dst);
    assert_eq!(fs::read(&dst).unwrap(), b"line one\nline two\n");
}

#[test]
fn round_trip_fs() {
    let src = tmp_path("rt_fs_src.txt");
    let dst = tmp_path("rt_fs_dst.txt");
    write(&src, b"line one\nline two\n");

    let out = copy(src.to_str().unwrap(), dst.to_str().unwrap(), CopyMethod::Fs).unwrap();
    assert_eq!(out, dst);
    assert_eq!(fs::read(&dst).unwrap(), b"line one\nline two\n");
}

#[test]
fn missing_source_reports_open_source() {
    let src = tmp_path("does_not_exist.txt");
    let dst = tmp_path("unused_dst.txt");

    for method in [CopyMethod::Stream, CopyMethod::Fs] {
        let err = copy(src.to_str().unwrap(), dst.to_str().unwrap(), method).unwrap_err();
        assert!(
            matches!(err, CopyError::OpenSource { .. }),
            "expected OpenSource, got {err:?}"
        );
    }
}

#[test]
fn directory_destination_copies_basename_inside() {
    let dir = tmp_path("into_dir");
    fs::create_dir_all(&dir).unwrap();
    let src = tmp_path("into_dir_src.txt");
    write(&src, b"payload\n");

    let out = copy(
        src.to_str().unwrap(),
        dir.to_str().unwrap(),
        CopyMethod::Stream,
    )
    .unwrap();
    let expected = dir.join(src.file_name().unwrap()); // basename lands inside the dir
    assert_eq!(out, expected);
    assert_eq!(fs::read(&expected).unwrap(), b"payload\n");
}

#[test]
fn existing_destination_is_truncated() {
    let src = tmp_path("ov_src.txt");
    let dst = tmp_path("ov_dst.txt");
    write(&src, b"new");
    write(&dst, b"old and much longer content");

    copy(
        src.to_str().unwrap(),
        dst.to_str().unwrap(),
        CopyMethod::Stream,
    )
    .unwrap();
    assert_eq!(fs::read(&dst).unwrap(), b"new"); // truncated, not appended
}

#[test]
fn same_file_is_refused_without_data_loss() {
    for method in [CopyMethod::Stream, CopyMethod::Fs] {
        let file = tmp_path("same.txt");
        write(&file, b"keep me");
        let path = file.to_str().unwrap();

        let err = copy(path, path, method).unwrap_err();
        assert!(
            matches!(err, CopyError::SameFile { .. }),
            "expected SameFile, got {err:?}"
        );
        assert_eq!(fs::read(&file).unwrap(), b"keep me"); // not truncated
    }
}

#[test]
fn directory_source_is_refused_without_truncating_destination() {
    for method in [CopyMethod::Stream, CopyMethod::Fs] {
        let srcdir = tmp_path("dir_src");
        fs::create_dir_all(&srcdir).unwrap();
        let dst = tmp_path("dir_src_dst.txt");
        write(&dst, b"precious");

        let err = copy(srcdir.to_str().unwrap(), dst.to_str().unwrap(), method).unwrap_err();
        assert!(
            matches!(err, CopyError::OpenSource { .. }),
            "expected OpenSource for a directory source, got {err:?}"
        );
        // The invalid source must not have truncated the existing destination.
        assert_eq!(fs::read(&dst).unwrap(), b"precious");
    }
}

#[test]
fn resolve_destination_directory_gets_basename() {
    let dir = tmp_path("resolve_dir");
    fs::create_dir_all(&dir).unwrap();
    assert_eq!(
        resolve_destination(&dir, Path::new("/a/b/src.txt")),
        dir.join("src.txt")
    );
}

#[test]
fn resolve_destination_non_directory_returned_as_is() {
    let dst = tmp_path("plain_dst.txt"); // does not exist
    assert_eq!(resolve_destination(&dst, Path::new("/a/b/src.txt")), dst);
}

#[test]
fn unreadable_source_is_blamed_on_source() {
    use std::os::unix::fs::PermissionsExt;

    let src = tmp_path("noperm_src.txt");
    let dst = tmp_path("noperm_dst.txt");
    write(&src, b"secret");
    fs::set_permissions(&src, fs::Permissions::from_mode(0o000)).unwrap();

    // Root bypasses permission checks; skip the assertion in that case.
    if fs::File::open(&src).is_ok() {
        return;
    }

    let err = copy(src.to_str().unwrap(), dst.to_str().unwrap(), CopyMethod::Fs).unwrap_err();
    assert!(
        matches!(err, CopyError::OpenSource { .. }),
        "expected OpenSource, got {err:?}"
    );
}
