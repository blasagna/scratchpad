//! The socket layer and the `--file` loader. Where this port differs most from
//! the other two, and almost all of it is subtraction - no `Fd`, no `dup`, no
//! `streambuf`, no `ReadErrorProbe`, no `SO_REUSEADDR` call. See the README.

use std::fmt;
use std::fs::File;
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddrV4, TcpListener, TcpStream};
use std::path::Path;
use std::time::Duration;

use crate::http::{PROG_NAME, Transaction, builtin_page, serve_connection};

/// Where the server binds when `--host` is not given; see the README for why
/// the default is not `0.0.0.0`.
pub const DEFAULT_HOST: Ipv4Addr = Ipv4Addr::LOCALHOST;

/// The port the exercise asks for.
pub const DEFAULT_PORT: u16 = 8080;

/// How long a connection may spend waiting for bytes, in either direction. Not
/// a command-line option: zero means "no timeout" in C and puts back the
/// wedged-loop bug, so this stays a constant the tests shorten via [`Options`].
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(5);

/// Largest `--file` page loaded. The page lives in memory for the process's
/// whole life, so an uncapped read is a memory bug wearing an option's clothes.
/// Exceeding this is a startup error, not a truncation.
pub const MAX_PAGE_BYTES: usize = 1024 * 1024;

/// The most a connection is drained of before its close. Unbounded would let a
/// client that keeps sending hold a one-at-a-time server forever.
const DRAIN_MAX: usize = 64 * 1024;

/// The same bound for the one connection owed a response it might not get: an
/// oversized header block, answered with a 431 that a close on unread data
/// would turn into an RST.
const DRAIN_OVERFLOW_MAX: usize = 1024 * 1024;

/// How much is read from the page file at a time.
const PAGE_CHUNK: usize = 8192;

/// `EPROTO`, spelled as its Linux number because this crate takes no `libc`
/// dependency and `io::ErrorKind` has no variant for it.
const EPROTO: i32 = 71;

/// Renders an `io::Error` the way `strerror` and `std::error_code::message` do.
/// Parity infrastructure, not style: Rust appends ` (os error N)` and nothing
/// else does. Reconstructed from `raw_os_error` rather than searched for.
pub fn os_message(err: &io::Error) -> String {
    let text = err.to_string();
    match err.raw_os_error() {
        Some(code) => text
            .strip_suffix(&format!(" (os error {code})"))
            .unwrap_or(&text)
            .to_owned(),
        None => text,
    }
}

/// A failure of the server's own, which ends it. A different type from
/// [`crate::ConnectionError`] because a client must not be able to end it.
#[derive(Debug)]
pub enum ServerError {
    /// The listening socket could not be created or bound. One variant where
    /// the other ports have three: `TcpListener::bind` is four syscalls in one
    /// call and returns one error. A recorded divergence; see the README.
    Bind(io::Error),
    /// The bound socket could not report the address it is listening on.
    Listen(io::Error),
    /// `accept` failed other than transiently.
    Accept(io::Error),
}

impl ServerError {
    /// Reports whether an accept failure is the client's doing. Both mean the
    /// peer vanished between the handshake and the accept, which is ordinary.
    /// `Interrupted` is not here because it is already retried.
    fn transient(&self) -> bool {
        match self {
            Self::Accept(err) => {
                err.kind() == io::ErrorKind::ConnectionAborted || err.raw_os_error() == Some(EPROTO)
            }
            _ => false,
        }
    }
}

impl fmt::Display for ServerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // The labels are logged, so they are shared with the other ports byte
        // for byte, and the errno text is appended the way they append it.
        let (label, err) = match self {
            Self::Bind(err) => ("cannot bind the listening socket", err),
            Self::Listen(err) => ("cannot listen on the socket", err),
            Self::Accept(err) => ("cannot accept a connection", err),
        };
        write!(f, "{label}: {}", os_message(err))
    }
}

impl std::error::Error for ServerError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Bind(err) | Self::Listen(err) | Self::Accept(err) => Some(err),
        }
    }
}

/// Why a `--file` page could not be loaded. Never a 500: the page is read
/// before the socket exists, which is what keeps `route` pure. `Display` omits
/// the path, since every caller prints it already.
#[derive(Debug)]
pub enum PageError {
    Open(io::Error),
    NotRegular,
    TooLarge,
}

impl fmt::Display for PageError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Open(err) => f.write_str(&os_message(err)),
            Self::NotRegular => f.write_str("not a regular file"),
            Self::TooLarge => write!(f, "larger than the {MAX_PAGE_BYTES} byte limit"),
        }
    }
}

impl std::error::Error for PageError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Open(err) => Some(err),
            Self::NotRegular | Self::TooLarge => None,
        }
    }
}

/// How the server behaves.
#[derive(Debug)]
pub struct Options<'a> {
    pub host: Ipv4Addr,
    pub port: u16,
    /// Serve exactly one request, then return. One request and not one
    /// connection: a browser's silent preconnect is a connection, and stopping
    /// on it exits having served nothing.
    pub serve_once: bool,
    /// Seconds a connection may spend waiting for bytes, in either direction.
    /// Not a command-line option; it lives here so the tests can shorten it.
    pub io_timeout: Duration,
    /// What is served at a known path: the built-in page, or `--file`'s bytes.
    pub page: &'a [u8],
}

impl Default for Options<'_> {
    fn default() -> Self {
        Self {
            host: DEFAULT_HOST,
            port: DEFAULT_PORT,
            serve_once: false,
            io_timeout: DEFAULT_TIMEOUT,
            page: builtin_page(),
        }
    }
}

/// Whether a connection got far enough to be answered, which is what `--once`
/// is waiting for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Answered,
    Unanswered,
}

/// Reads a `--file` page into memory before the socket exists, so an unreadable
/// one is a startup failure rather than a 500 later. Over `max_bytes` is
/// refused, not truncated; empty is a page of zero bytes.
pub fn load_page(path: &Path, max_bytes: usize) -> Result<Vec<u8>, PageError> {
    let mut file = File::open(path).map_err(PageError::Open)?;

    // Opening a directory succeeds and only fails inside the read with EISDIR,
    // reported as "cannot read the page file". The open file's metadata, not
    // fs::metadata(path), which would answer about a different file.
    if !file
        .metadata()
        .map_err(PageError::Open)?
        .file_type()
        .is_file()
    {
        return Err(PageError::NotRegular);
    }

    // Read in chunks rather than trusting a size: the file may change between
    // the stat and the read, and a Content-Length from the stat would then
    // describe a page that is no longer the one being served.
    let mut data = Vec::new();
    let mut chunk = [0u8; PAGE_CHUNK];
    loop {
        match file.read(&mut chunk) {
            Ok(0) => return Ok(data),
            Ok(n) => {
                data.extend_from_slice(&chunk[..n]);
                if data.len() > max_bytes {
                    return Err(PageError::TooLarge);
                }
            }
            Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
            Err(err) => return Err(PageError::Open(err)),
        }
    }
}

/// Creates the listening socket. A `SocketAddrV4` and never a string, which
/// keeps a blocking DNS lookup out of startup. `SO_REUSEADDR` needs no code
/// because `TcpListener::bind` sets it, and std does not set `SO_REUSEPORT`.
pub fn listen(opts: &Options<'_>) -> Result<TcpListener, ServerError> {
    TcpListener::bind(SocketAddrV4::new(opts.host, opts.port)).map_err(ServerError::Bind)
}

/// Applies the read and write timeouts to an accepted connection. The receive
/// timeout keeps a browser's silent preconnect from wedging a server that serves
/// one at a time; the send timeout covers a client that stops reading.
fn set_timeouts(stream: &TcpStream, timeout: Duration) -> io::Result<()> {
    stream.set_read_timeout(Some(timeout))?;
    stream.set_write_timeout(Some(timeout))
}

/// Reads and discards what the client sent and we never asked for, so the close
/// sends a FIN rather than an RST. Non-blocking everywhere but the 431 path,
/// where the rest of the request is still in flight; see the README.
fn drain(stream: &TcpStream, blocking: bool, max: usize) {
    if !blocking && stream.set_nonblocking(true).is_err() {
        return;
    }

    let mut source = stream;
    let mut scrap = [0u8; 4096];
    let mut total = 0;
    while total < max {
        match source.read(&mut scrap) {
            Ok(0) => break,
            Ok(n) => total += n,
            Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
            // WouldBlock is the non-blocking drain finding nothing left and
            // TimedOut is the blocking one running out its clock; neither is
            // worth reporting on a connection about to be closed.
            Err(_) => break,
        }
    }
}

/// Accepts one connection, serves it, and closes it with a shutdown plus a
/// bounded drain. Everything a client can do comes back as `Ok`; only `accept`
/// is worth ending the server over, and its error travels in the value.
pub fn accept_once<L: Write>(
    listener: &TcpListener,
    opts: &Options<'_>,
    log: &mut L,
) -> Result<Outcome, ServerError> {
    // Nothing here installs a handler, so nothing should interrupt this - but a
    // profiler's SIGPROF would, and an unexplained server exit is a bad way to
    // find that out.
    let (stream, peer) = loop {
        match listener.accept() {
            Ok(accepted) => break accepted,
            Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
            Err(err) => return Err(ServerError::Accept(err)),
        }
    };

    // The address comes back from the accept itself, so there is no reverse DNS
    // lookup to avoid, and SocketAddr's Display is already `127.0.0.1:54012`.
    let _ = writeln!(log, "{PROG_NAME}: connection from {peer}");

    if let Err(err) = set_timeouts(&stream, opts.io_timeout) {
        let _ = writeln!(
            log,
            "{PROG_NAME}: cannot set the connection timeouts: {}",
            os_message(&err)
        );
        drop(stream);
        let _ = writeln!(log, "{PROG_NAME}: connection closed");
        return Ok(Outcome::Unanswered);
    }

    let tx: Transaction = {
        // Two borrows of one socket, which is why there is no dup here: stdio's
        // read-then-write positioning rule is not Rust's. Buffering is free and
        // dropping the reader discards the body past the blank line for free.
        let mut input = BufReader::new(&stream);
        let mut output = BufWriter::new(&stream);
        serve_connection(&mut input, &mut output, log, opts.page)
    };

    // A lingering close, not a bare one. The oversized-header case waits for the
    // rest of a request still on its way; see drain.
    let _ = stream.shutdown(Shutdown::Write);
    drain(
        &stream,
        tx.left_unread,
        if tx.left_unread {
            DRAIN_OVERFLOW_MAX
        } else {
            DRAIN_MAX
        },
    );
    drop(stream);

    let _ = writeln!(log, "{PROG_NAME}: connection closed");

    Ok(match tx.result {
        Ok(()) => Outcome::Answered,
        Err(failure) if failure.answered() => Outcome::Answered,
        Err(_) => Outcome::Unanswered,
    })
}

/// Binds, then serves one connection at a time until something fatal happens. A
/// client cannot end the server. Returns `Ok` only when `opts.serve_once`
/// answered a request; a connection that sent nothing does not count.
pub fn run<L: Write>(opts: &Options<'_>, log: &mut L) -> Result<(), ServerError> {
    let listener = listen(opts)?;
    // Unconditionally, not only for port 0: one code path, and the logged port
    // is always the one really in use.
    let bound = listener.local_addr().map_err(ServerError::Listen)?;
    let _ = writeln!(log, "{PROG_NAME}: listening on {bound}");

    loop {
        match accept_once(&listener, opts, log) {
            Ok(outcome) => {
                if opts.serve_once && outcome == Outcome::Answered {
                    return Ok(());
                }
            }
            Err(failure) if failure.transient() => {
                let _ = writeln!(log, "{PROG_NAME}: {failure}");
            }
            Err(fatal) => return Err(fatal),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Only what is pure or private lives here; the socket layer and the --file
    // loader both touch the world, so their tests are in tests/socket.rs.

    #[test]
    fn strips_the_os_error_suffix_the_other_ports_do_not_print() {
        // Parity infrastructure: C's strerror and C++'s ec.message() both stop
        // at the message.
        assert_eq!(
            os_message(&io::Error::from_raw_os_error(2)),
            "No such file or directory"
        );
        assert_eq!(
            os_message(&io::Error::from_raw_os_error(13)),
            "Permission denied"
        );
    }

    #[test]
    fn leaves_an_error_that_is_not_the_os_alone() {
        let err = io::Error::other("something (os error 2) shaped");
        assert_eq!(os_message(&err), "something (os error 2) shaped");
    }

    #[test]
    fn a_fatal_failure_names_the_stage_and_the_errno() {
        assert_eq!(
            ServerError::Bind(io::Error::from_raw_os_error(98)).to_string(),
            "cannot bind the listening socket: Address already in use"
        );
        assert_eq!(
            ServerError::Listen(io::Error::from_raw_os_error(9)).to_string(),
            "cannot listen on the socket: Bad file descriptor"
        );
        assert_eq!(
            ServerError::Accept(io::Error::from_raw_os_error(24)).to_string(),
            "cannot accept a connection: Too many open files"
        );
    }

    #[test]
    fn only_a_transient_accept_failure_is_survivable() {
        // Both of these mean the peer vanished between the handshake and the
        // accept, which is ordinary.
        assert!(ServerError::Accept(io::Error::from(io::ErrorKind::ConnectionAborted)).transient());
        assert!(ServerError::Accept(io::Error::from_raw_os_error(EPROTO)).transient());
        // EMFILE is the one that matters: logging and continuing on it spins at
        // 100% CPU forever, which is far worse than exiting with it.
        assert!(!ServerError::Accept(io::Error::from_raw_os_error(24)).transient());
        assert!(!ServerError::Bind(io::Error::from_raw_os_error(98)).transient());
    }

    #[test]
    fn the_page_cap_message_names_the_limit() {
        assert_eq!(
            PageError::TooLarge.to_string(),
            "larger than the 1048576 byte limit"
        );
        assert_eq!(PageError::NotRegular.to_string(), "not a regular file");
    }
}
