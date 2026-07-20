//! Library for copying one file to another, like the `cp` command.
//!
//! Path handling matches the C and C++ ports: relative paths resolve against the
//! current directory, a leading `~`/`~/…` expands via `$HOME`, and a destination
//! that is an existing directory receives the source under its base name. The
//! byte copy itself is done either by streaming [`std::io::copy`]
//! ([`CopyMethod::Stream`], the default) or the one-shot [`std::fs::copy`]
//! ([`CopyMethod::Fs`]).
//!
//! Note: `~user` is intentionally *not* expanded — Rust's std has no password
//! database access, and this crate takes no extra dependencies for it. A `~user`
//! prefix is passed through unchanged so the eventual open reports a clear error.

use std::ffi::OsStr;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

/// Which underlying mechanism [`copy`] uses to move the bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CopyMethod {
    /// Stream the bytes with [`std::io::copy`] between open [`File`] handles.
    Stream,
    /// Copy with the one-shot [`std::fs::copy`].
    Fs,
}

/// The stage at which a [`copy`] failed, with enough detail to report clearly.
#[derive(Debug)]
pub enum CopyError {
    /// Opening the source for reading failed.
    OpenSource { path: PathBuf, source: io::Error },
    /// Opening or creating the destination for writing failed.
    OpenDest { path: PathBuf, source: io::Error },
    /// Reading or writing bytes during the copy failed.
    Copy(io::Error),
    /// The source and destination are the same file; copying would destroy it.
    SameFile { path: PathBuf },
}

impl fmt::Display for CopyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CopyError::OpenSource { path, source } => {
                write!(f, "cannot open source file '{}': {source}", path.display())
            }
            CopyError::OpenDest { path, source } => {
                write!(
                    f,
                    "cannot open destination file '{}': {source}",
                    path.display()
                )
            }
            CopyError::Copy(source) => write!(f, "error copying data: {source}"),
            CopyError::SameFile { .. } => {
                write!(f, "source and destination are the same file")
            }
        }
    }
}

impl std::error::Error for CopyError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            CopyError::OpenSource { source, .. }
            | CopyError::OpenDest { source, .. }
            | CopyError::Copy(source) => Some(source),
            CopyError::SameFile { .. } => None,
        }
    }
}

/// Copies every byte from `reader` to `writer`, returning the count.
///
/// A thin wrapper over [`std::io::copy`], kept as a named function so it is easy
/// to unit-test with in-memory streams. Copies raw bytes, so an empty input and
/// embedded NULs are handled without special cases.
pub fn copy_stream<R: Read, W: Write>(reader: &mut R, writer: &mut W) -> io::Result<u64> {
    io::copy(reader, writer)
}

/// Expands a leading `~` or `~/…` to the user's home directory via `$HOME`.
///
/// A `~user` prefix, a `~` anywhere but the start, or an unset/empty `$HOME`
/// leaves the path unchanged.
pub fn expand_tilde(path: &str) -> PathBuf {
    expand_tilde_with(path, std::env::var_os("HOME").as_deref())
}

/// [`expand_tilde`] with the home directory supplied explicitly, so tests can
/// exercise it without mutating the process environment.
fn expand_tilde_with(path: &str, home: Option<&OsStr>) -> PathBuf {
    let Some(rest) = path.strip_prefix('~') else {
        return PathBuf::from(path);
    };

    // Only a bare "~" or a "~/..." prefix expands; "~user" is left as written.
    let sub = match rest {
        "" => None,                                     // bare "~"
        _ if rest.starts_with('/') => Some(&rest[1..]), // "~/sub" -> "sub"
        _ => return PathBuf::from(path),                // "~user" etc.
    };

    match home {
        Some(home) if !home.is_empty() => {
            let mut p = PathBuf::from(home);
            if let Some(sub) = sub {
                p.push(sub);
            }
            p
        }
        // No home to expand against: leave the path as written.
        _ => PathBuf::from(path),
    }
}

/// Resolves the final destination path for a copy.
///
/// If `dest` names an existing directory, returns `dest` joined with the source's
/// base name (like `cp`); otherwise returns `dest` unchanged.
pub fn resolve_destination(dest: &Path, source: &Path) -> PathBuf {
    if dest.is_dir()
        && let Some(name) = source.file_name()
    {
        return dest.join(name);
    }
    dest.to_path_buf()
}

/// Returns true if both paths refer to the same file (same device and inode).
///
/// Catches hard links and different spellings of one path (`./x` vs `x`). A path
/// that cannot be stat'd (e.g. a destination that does not exist yet) is treated
/// as distinct.
fn is_same_file(a: &Path, b: &Path) -> bool {
    use std::os::unix::fs::MetadataExt;
    match (fs::metadata(a), fs::metadata(b)) {
        (Ok(ma), Ok(mb)) => ma.dev() == mb.dev() && ma.ino() == mb.ino(),
        _ => false,
    }
}

/// Copies the file named by `source` to `dest`.
///
/// Both arguments are tilde-expanded; `dest` is then run through
/// [`resolve_destination`], so naming an existing directory copies into it. On
/// success returns the resolved destination path.
///
/// Refuses to copy a file onto itself: both copy methods truncate the
/// destination before reading, which would destroy the source.
pub fn copy(source: &str, dest: &str, method: CopyMethod) -> Result<PathBuf, CopyError> {
    let src = expand_tilde(source);
    let dst = resolve_destination(&expand_tilde(dest), &src);

    // Validate the source is a regular file up front, before the destination is
    // opened or truncated. Both File::open and fs::copy's internal open succeed
    // on a directory, so without this a directory source would truncate an
    // existing destination before failing (data loss), and the --fs path would
    // misattribute the failure to the destination. Matches `cp`, which refuses a
    // directory source outright.
    match fs::metadata(&src) {
        Ok(meta) if meta.is_file() => {}
        Ok(_) => {
            return Err(CopyError::OpenSource {
                path: src,
                source: io::Error::new(io::ErrorKind::InvalidInput, "not a regular file"),
            });
        }
        Err(source) => return Err(CopyError::OpenSource { path: src, source }),
    }

    if is_same_file(&src, &dst) {
        return Err(CopyError::SameFile { path: dst });
    }

    match method {
        CopyMethod::Stream => {
            let mut reader = File::open(&src).map_err(|source| CopyError::OpenSource {
                path: src.clone(),
                source,
            })?;
            let mut writer = File::create(&dst).map_err(|source| CopyError::OpenDest {
                path: dst.clone(),
                source,
            })?;
            copy_stream(&mut reader, &mut writer).map_err(CopyError::Copy)?;
        }
        CopyMethod::Fs => {
            // fs::copy bundles open/read/write into one error, so infer the
            // stage: blame the source when it cannot be opened (missing or
            // unreadable), otherwise the destination.
            if let Err(source) = fs::copy(&src, &dst) {
                return Err(if File::open(&src).is_ok() {
                    CopyError::OpenDest { path: dst, source }
                } else {
                    CopyError::OpenSource { path: src, source }
                });
            }
        }
    }

    Ok(dst)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn copy_stream_copies_non_empty_content() {
        let mut src = Cursor::new(b"hello, world\n".to_vec());
        let mut dst: Vec<u8> = Vec::new();
        assert_eq!(copy_stream(&mut src, &mut dst).unwrap(), 13);
        assert_eq!(dst, b"hello, world\n");
    }

    #[test]
    fn copy_stream_empty_source_yields_empty_output() {
        let mut src = Cursor::new(Vec::new());
        let mut dst: Vec<u8> = Vec::new();
        assert_eq!(copy_stream(&mut src, &mut dst).unwrap(), 0);
        assert!(dst.is_empty());
    }

    #[test]
    fn copy_stream_copies_binary_data_with_nuls() {
        let data = b"a\0b\0\0c".to_vec();
        let mut src = Cursor::new(data.clone());
        let mut dst: Vec<u8> = Vec::new();
        copy_stream(&mut src, &mut dst).unwrap();
        assert_eq!(dst, data);
    }

    #[test]
    fn expand_tilde_no_tilde_returns_input() {
        let home = OsStr::new("/home/testuser");
        assert_eq!(
            expand_tilde_with("/abs/path.txt", Some(home)),
            PathBuf::from("/abs/path.txt")
        );
        assert_eq!(
            expand_tilde_with("rel/path.txt", Some(home)),
            PathBuf::from("rel/path.txt")
        );
    }

    #[test]
    fn expand_tilde_bare_uses_home() {
        let home = OsStr::new("/home/testuser");
        assert_eq!(
            expand_tilde_with("~", Some(home)),
            PathBuf::from("/home/testuser")
        );
    }

    #[test]
    fn expand_tilde_slash_expands_to_home() {
        let home = OsStr::new("/home/testuser");
        assert_eq!(
            expand_tilde_with("~/sub/f.txt", Some(home)),
            PathBuf::from("/home/testuser/sub/f.txt")
        );
    }

    #[test]
    fn expand_tilde_user_left_unchanged() {
        let home = OsStr::new("/home/testuser");
        assert_eq!(
            expand_tilde_with("~other/f.txt", Some(home)),
            PathBuf::from("~other/f.txt")
        );
    }

    #[test]
    fn expand_tilde_only_expands_at_start() {
        let home = OsStr::new("/home/testuser");
        assert_eq!(
            expand_tilde_with("a/~/b", Some(home)),
            PathBuf::from("a/~/b")
        );
    }

    #[test]
    fn expand_tilde_unset_home_left_unchanged() {
        assert_eq!(expand_tilde_with("~/x", None), PathBuf::from("~/x"));
    }
}
