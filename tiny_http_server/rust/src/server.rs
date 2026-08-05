//! The socket layer and the `--file` loader.
//!
//! This is where the port differs most from the other two, and almost all of
//! the difference is subtraction. `TcpListener::bind` already sets
//! `SO_REUSEADDR`; `TcpStream` already owns its descriptor, so there is no `Fd`
//! type and no `fdopen` cleanup table; `&TcpStream` is both `Read` and `Write`,
//! so there is no `dup` and no second stream and no hand-written `streambuf`;
//! and `io::Error` says *why* a read stopped, so there is no `ReadErrorProbe`.
//!
//! What is left is the part that is genuinely this program's: which failures end
//! the server and which are one connection's, and the lingering close.

use std::fmt;
use std::fs::File;
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddrV4, TcpListener, TcpStream};
use std::path::Path;
use std::time::Duration;

use crate::http::{PROG_NAME, Transaction, builtin_page, serve_connection};

/// Where the server binds when `--host` is not given. The exercise says to open
/// a browser at localhost, which loopback satisfies; see the README for why the
/// default is not `0.0.0.0`.
pub const DEFAULT_HOST: Ipv4Addr = Ipv4Addr::LOCALHOST;

/// The port the exercise asks for.
pub const DEFAULT_PORT: u16 = 8080;

/// How long a connection may spend waiting for bytes, in either direction.
///
/// Not a command-line option: its most obvious setting, zero, means "no
/// timeout" in C and puts back the wedged-loop bug the timeout exists to
/// prevent. Rust cannot even spell that mistake - `set_read_timeout(Some(ZERO))`
/// is an error - but the option would still be one more thing to get wrong for
/// no gain, so this stays a constant the tests shorten through [`Options`].
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(5);

/// Largest `--file` page loaded, in bytes. The page lives in memory for the
/// process's whole life and `Content-Length` is derived from it, so an uncapped
/// read is a memory bug wearing an option's clothes. Exceeding this is a startup
/// error rather than a truncation - half a page served as a whole one is worse
/// than a refusal that says why.
pub const MAX_PAGE_BYTES: usize = 1024 * 1024;

/// The most a connection is drained of before its close. Unbounded is not an
/// option: this server handles one connection at a time, so a client that keeps
/// sending would hold it forever. The receive timeout bounds the wall clock and
/// this bounds the bytes.
const DRAIN_MAX: usize = 64 * 1024;

/// The same bound for the one connection that is owed a response it might not
/// get: an oversized header block, answered with a 431 that a close on unread
/// data would turn into an RST. A request that reaches this and keeps going has
/// stopped being a request.
const DRAIN_OVERFLOW_MAX: usize = 1024 * 1024;

/// How much is read from the page file at a time.
const PAGE_CHUNK: usize = 8192;

/// `EPROTO`, spelled as its Linux number because this crate takes no `libc`
/// dependency - the same call, for the same reason, as `mini_shell`'s tests
/// spelling `EAGAIN` as 11. `io::ErrorKind` has no variant for it.
const EPROTO: i32 = 71;

/// Renders an `io::Error` the way `strerror` and `std::error_code::message` do.
///
/// This is parity infrastructure, not style. Rust's `io::Error` displays an OS
/// error as `No such file or directory (os error 2)` where C's `strerror` and
/// C++'s `ec.message()` both stop at the message - and two of the cases
/// `check_parity.sh` diffs directly are a startup log line built from one
/// (`--file` that does not exist, `--port 80` as a non-root user). `mini_shell`
/// answered the same problem by writing its two reachable messages out by hand;
/// that does not scale here, because every errno `bind` and `open` can produce
/// is reachable.
///
/// The suffix is reconstructed from `raw_os_error` rather than searched for, so
/// a message that happens to contain the words is left alone, and anything that
/// is not an OS error passes through whole.
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

/// A failure of the server's own, which ends it.
///
/// Deliberately a different type from [`crate::ConnectionError`]: everything a
/// client can do is one of those and none of them belongs here, because a client
/// must not be able to end the server.
#[derive(Debug)]
pub enum ServerError {
    /// The listening socket could not be created or bound.
    ///
    /// One variant where the other two ports have three. `TcpListener::bind` is
    /// `socket`, `SO_REUSEADDR`, `bind`, and `listen` in a single call, so there
    /// is no way to say which of them failed - and every failure reachable
    /// without exhausting file descriptors really is the bind's (`EACCES` on a
    /// privileged port, `EADDRINUSE` when the server is already running). A
    /// recorded divergence; see the README.
    Bind(io::Error),
    /// The bound socket could not report the address it is listening on.
    Listen(io::Error),
    /// `accept` failed other than transiently.
    Accept(io::Error),
}

impl ServerError {
    /// Reports whether an accept failure is the client's doing rather than the
    /// server's. Both of these mean the peer vanished between the handshake and
    /// the accept, which is ordinary. `Interrupted` is not here because it is
    /// already retried.
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
        // for byte, and the errno text is appended the way C's
        // `"%s: %s: %s"` and C++'s `describe(stage) << ": " << ec.message()` do.
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

/// Why a `--file` page could not be loaded.
///
/// A startup failure with a message, never a 500: the page is read before the
/// socket exists, which is what keeps `route` pure and infallible and is why
/// this server has no 500 anywhere.
///
/// `Display` deliberately omits the path, because every caller prints it
/// already - the message reads `tiny_http_server: <path>: <this>`.
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
    /// Serve exactly one request, then return. What an end-to-end check uses
    /// instead of backgrounding the server and killing it by PID. One request
    /// and not one connection: a browser's silent preconnect is a connection,
    /// and stopping on it exits having served nothing.
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

/// Reads a `--file` page into memory, once, at startup.
///
/// Called before the socket exists, so an unreadable page is a startup failure
/// with a message rather than a 500 that shows up later depending on which path
/// somebody visits. The cost is that the page cannot change while the server
/// runs; restart to change it.
///
/// A file larger than `max_bytes` is refused, not truncated. An empty file is
/// not an error: it is a page of zero bytes and is served as one.
///
/// `Vec<u8>` and not `String`: a `--file` is served as `text/html` whatever its
/// bytes are, so a PNG renders as garbage rather than failing to load.
pub fn load_page(path: &Path, max_bytes: usize) -> Result<Vec<u8>, PageError> {
    let mut file = File::open(path).map_err(PageError::Open)?;

    // Opening a directory succeeds on Linux and only fails later inside the
    // read, with EISDIR, which would be reported as "cannot read the page file"
    // for what is really "that is a directory". Asking first also turns down
    // /dev/zero and FIFOs before they churn through the whole cap. It is the
    // open file's metadata rather than fs::metadata(path), which would be
    // answering about a different file than the one being read.
    if !file
        .metadata()
        .map_err(PageError::Open)?
        .file_type()
        .is_file()
    {
        return Err(PageError::NotRegular);
    }

    // Read in chunks rather than trusting a size. The file may change between
    // the stat and the read, in which case a Content-Length taken from the stat
    // would describe a page that is no longer the one being served.
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

/// Creates the listening socket.
///
/// A `SocketAddrV4` and never a string, which is what keeps `getaddrinfo` out of
/// this: resolving names would put DNS - a blocking network lookup - into the
/// startup of a program that binds exactly one socket, and would hand back a
/// list of candidates to choose between.
///
/// `SO_REUSEADDR` needs no code here because `TcpListener::bind` already sets
/// it, and without it restarting inside about a minute fails `EADDRINUSE` on the
/// `TIME_WAIT` remnants of the connections just served - which is every Ctrl-C
/// and rerun. It is **not** `SO_REUSEPORT`, which std does not set either, so a
/// second live listener is still refused and `EADDRINUSE` keeps meaning "the
/// server is already running".
pub fn listen(opts: &Options<'_>) -> Result<TcpListener, ServerError> {
    TcpListener::bind(SocketAddrV4::new(opts.host, opts.port)).map_err(ServerError::Bind)
}

/// Applies the read and write timeouts to an accepted connection.
///
/// The receive timeout is what keeps a browser's speculative connection -
/// connected, then silent - from wedging a server that serves one at a time. The
/// send timeout covers the mirror image, which only bites with a large `--file`:
/// a client that stops reading blocks the write once the socket's send buffer
/// fills.
fn set_timeouts(stream: &TcpStream, timeout: Duration) -> io::Result<()> {
    stream.set_read_timeout(Some(timeout))?;
    stream.set_write_timeout(Some(timeout))
}

/// Reads and discards what the client sent and we never asked for, so the close
/// below sends a FIN rather than an RST.
///
/// Reads the socket directly instead of going back through the request reader:
/// bytes sitting in that reader's buffer are already off the socket, and
/// dropping it discards them for free.
///
/// Every ordinary connection drains non-blocking, so this takes what has already
/// arrived and never waits for more. A blocking drain everywhere would sit here
/// until the peer closed, which costs the accept loop a round trip on every
/// connection and lets a client that reads its response but keeps the socket
/// open stall the whole server for the timeout. What is left is a narrow race -
/// a body that arrives between the drain and the close still provokes an RST -
/// and that is the right trade for a server that handles one connection at a
/// time.
///
/// The exception, and the only caller that passes `blocking`, is a header block
/// over `REQUEST_MAX`. There the server stopped reading at 8 KiB with the rest
/// of a much larger request still in flight, so what has already arrived is
/// nowhere near all of it - and unlike a body nobody read, that client *was*
/// answered, with a 431 the RST would throw away. The read timeout bounds the
/// wait exactly as it bounds every other read on this socket.
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
            // WouldBlock is the non-blocking drain finding nothing left, and
            // TimedOut is the blocking one running out its clock. Neither is
            // worth reporting: this connection is about to be closed.
            Err(_) => break,
        }
    }
}

/// Accepts one connection, serves it, and closes it.
///
/// The connection is closed with a shutdown and a bounded drain rather than a
/// bare close. Linux sends an RST instead of a FIN when a socket still holds
/// unread inbound data, and a peer may discard data it already received when it
/// gets an RST - so `curl -d x` would report a connection reset instead of
/// showing the 405 that was really sent, since a request body is deliberately
/// never read.
///
/// Everything a client can do comes back as `Ok`: a connection that could not be
/// set up is that connection's failure and is logged here. **Only `accept` is
/// worth ending the server over**, and folding the setup failures into it would
/// let a one-off `ENOMEM` on one connection end the server. `run` decides which
/// accept failures are fatal, and the error travels in the value rather than in
/// a global, so - unlike the C port, which must return from the statement
/// immediately after the failing `accept` because even a successful log write
/// may set `errno` - there is no rule left here to break.
pub fn accept_once<L: Write>(
    listener: &TcpListener,
    opts: &Options<'_>,
    log: &mut L,
) -> Result<Outcome, ServerError> {
    // Nothing here installs a signal handler, so nothing should interrupt this -
    // but a profiler's SIGPROF would, and an unexplained server exit is a bad
    // way to find that out.
    let (stream, peer) = loop {
        match listener.accept() {
            Ok(accepted) => break accepted,
            Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
            Err(err) => return Err(ServerError::Accept(err)),
        }
    };

    // The address comes back from the accept itself, so there is no reverse DNS
    // lookup to avoid here the way there is with getnameinfo - and SocketAddr's
    // own Display is already `127.0.0.1:54012`.
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
        // Two borrows of one socket, which is the whole reason there is no dup
        // here: C needs a file-positioning call between a read and a following
        // write on one stream and a socket has none, so it opens "r" on the
        // accepted descriptor and "w" on a dup of it. That rule is stdio's.
        //
        // The reader is buffered, which is the exact opposite of mini_shell:
        // nothing is forked here, so buffering is free - and the buffer holding
        // bytes past the blank line is desirable, since those are a request body
        // nobody reads and dropping the reader throws them away for free.
        let mut input = BufReader::new(&stream);
        let mut output = BufWriter::new(&stream);
        serve_connection(&mut input, &mut output, log, opts.page)
    };

    // A lingering close, not a bare one. An oversized header block is the one
    // case where what is left unread is not a body the client already finished
    // sending but the remainder of a request still on its way, so that one waits
    // for it; see drain.
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

/// Binds, then serves connections until something fatal happens.
///
/// One connection at a time: accept, read, respond, close, accept. Nothing here
/// forks and nothing threads, so there is no reaping and no shared state, and a
/// slow client stalls the next one - which is what the receive timeout bounds.
///
/// A client cannot end the server. Every failure a peer can cause is logged
/// against its connection and the loop continues; only the listening socket's
/// own failures, and an accept error that is not transient, come back from here.
/// Logging and continuing on *every* accept error was the alternative and was
/// rejected: `EMFILE` or `EBADF` would then spin at 100% CPU writing the same
/// line forever, which is far worse than exiting with it.
///
/// Returns `Ok` only when `opts.serve_once` answered a request; a connection
/// that sent nothing does not count, or `--once` against a browser exits on the
/// preconnect.
pub fn run<L: Write>(opts: &Options<'_>, log: &mut L) -> Result<(), ServerError> {
    let listener = listen(opts)?;
    // Unconditionally, not only when port 0 asked the kernel to choose: one code
    // path, and the port that gets logged is always the one really in use.
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

    // Only what is pure or private lives here. The socket layer and the --file
    // loader both touch the world, so their tests are in tests/socket.rs - the
    // same split simple_logger and mini_shell make.

    #[test]
    fn strips_the_os_error_suffix_the_other_ports_do_not_print() {
        // Parity infrastructure: C's strerror and C++'s ec.message() both stop
        // at the message, and two of the log lines check_parity.sh diffs are
        // built from one.
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
        // 100% CPU writing the same line forever, which is far worse than
        // exiting with it.
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
