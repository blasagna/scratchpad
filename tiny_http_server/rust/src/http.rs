//! The protocol: parse a request line, decide what to answer it with, write the
//! response. Pure or generic over `Read`/`Write`, so there is no socket here.
//! Bytes and not `String`, so a NUL survives far enough to be *refused*.

use std::borrow::Cow;
use std::fmt;
use std::io::{self, Read, Write};

/// Prefix on every line the server logs, matching the binary name.
pub const PROG_NAME: &str = "tiny_http_server";

/// Value of the `Server` header. A constant, so the response bytes stay golden.
pub const SERVER_NAME: &str = "tiny_http_server";

/// The only content type this server ever sends; nothing is sniffed.
pub const CONTENT_TYPE: &str = "text/html; charset=utf-8";

/// Largest request header block accepted. Reaching this without a terminator is
/// a 431, not a truncation.
pub const REQUEST_MAX: usize = 8192;

/// Longest request line written to the log, after sanitizing. 255 and not 256:
/// the C port spells this as a 256-byte buffer and spends one byte on the NUL,
/// so the count is the shared contract and the buffer was only how C said it.
pub const LOG_LINE_MAX: usize = 255;

/// The page served when there is no `--file`. Compiled in, not read from a file
/// beside the binary: `cargo run` and `bazel run` each execute from somewhere of
/// their own, so a relative path would resolve differently.
const INDEX_HTML: &[u8] = b"<!DOCTYPE html>\n\
<html lang=\"en\">\n\
<head><meta charset=\"utf-8\"><title>tiny_http_server</title></head>\n\
<body><h1>Hello, world!</h1><p>Served by tiny_http_server.</p></body>\n\
</html>\n";

/// The value of the `Allow` header on a 405, and the whole of what is allowed.
const ALLOWED_METHODS: &str = "GET, HEAD";

/// The two paths the page is served at.
const ROOT_PATH: &[u8] = b"/";
const INDEX_PATH: &[u8] = b"/index.html";

/// What a truncated log line ends with, so a prefix is not shown as the whole.
const ELLIPSIS: &str = "...";

/// What went wrong on one connection. None of it ends the server; the server's
/// own failures are [`crate::ServerError`], a different type for exactly that
/// reason. A 4xx or 5xx is not here - that is a [`Response`].
#[derive(Debug)]
pub enum ConnectionError {
    /// The client hung up without sending a request. What a browser's
    /// speculative preconnect looks like, and entirely ordinary.
    Closed,
    /// Nothing arrived before the receive timeout.
    Timeout,
    /// A read error on the connection.
    Read(io::Error),
    /// A write error on the connection.
    Write(io::Error),
}

impl ConnectionError {
    /// Reports whether the response still went out, which is what `--once`
    /// waits for. Breaking on any connection makes `--once` useless against a
    /// browser's preconnect. A write failure counts: the response was written.
    pub fn answered(&self) -> bool {
        matches!(self, Self::Write(_))
    }
}

impl fmt::Display for ConnectionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Logged, so shared with the other ports byte for byte. The io::Error is
        // deliberately not appended: neither of those ports has one here.
        let text = match self {
            Self::Closed => "client closed the connection without sending a request",
            Self::Timeout => "client sent nothing before the read timeout",
            Self::Read(_) => "error reading the request",
            Self::Write(_) => "error writing the response",
        };
        f.write_str(text)
    }
}

impl std::error::Error for ConnectionError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Closed | Self::Timeout => None,
            Self::Read(err) | Self::Write(err) => Some(err),
        }
    }
}

/// The methods routing distinguishes. `Other` is not a parse failure: `POST` is
/// well-formed, and that it is not served is routing's judgment, made as a 405.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Get,
    Head,
    Other,
}

/// A parsed request line. Every field borrows the caller's block, which must
/// outlive it; a slice carries its own length, which is what lets a request
/// containing a NUL be refused rather than silently truncated.
#[derive(Debug)]
pub struct Request<'a> {
    pub method: Method,
    /// The request target exactly as it arrived: not percent-decoded and not
    /// normalized, since nothing here reaches the filesystem.
    pub target: &'a [u8],
    /// The target up to the first `?`, which is what routing matches on.
    pub path: &'a [u8],
    pub major: u8,
    pub minor: u8,
    /// The whole request line, for the log. [`sanitize`] it before writing it.
    pub line: &'a [u8],
}

/// A response ready to be written. The body is a [`Cow`] because a 200 borrows
/// the page - possibly a 1 MiB `--file` - while an error owns what it rendered,
/// which is what lets [`route`] stay pure with no scratch buffer.
#[derive(Debug)]
pub struct Response<'a> {
    pub status: u16,
    pub reason: &'static str,
    pub body: Cow<'a, [u8]>,
    pub allow: Option<&'static str>,
}

/// Whether a header block ended, or ran past the cap. Separate from
/// [`ConnectionError`] because the cap is the one read outcome whose client is
/// still there to be answered, with a 431.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReadOutcome {
    Complete,
    TooLarge,
}

/// What one connection came to. `left_unread` is a field rather than part of
/// the result because a 431 *is* a response - and the caller still has to know
/// the rest of a much larger request is on its way before it closes.
#[derive(Debug)]
pub struct Transaction {
    /// `Ok` when a response was written, whatever its status: a 404 is the
    /// server working.
    pub result: Result<(), ConnectionError>,
    pub left_unread: bool,
}

/// Returns the reason phrase for a status this server sends, else `"Unknown"`.
/// It is part of the response bytes and of every error page's title.
pub fn status_reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        431 => "Request Header Fields Too Large",
        505 => "HTTP Version Not Supported",
        _ => "Unknown",
    }
}

/// Returns the page compiled into the binary, served when there is no `--file`.
pub fn builtin_page() -> &'static [u8] {
    INDEX_HTML
}

/// Renders untrusted bytes safe to write to a log: every non-printable byte
/// becomes `?`, so a request line cannot clear a terminal or forge a second
/// line. At most `max` characters; longer input is truncated and ends with `...`.
pub fn sanitize(src: &[u8], max: usize) -> String {
    // The ellipsis lives inside max, displacing the last bytes rather than
    // pushing the line over; too small even for it keeps whatever fits.
    let (keep, dots) = if src.len() > max {
        let dots = max.min(ELLIPSIS.len());
        (max - dots, dots)
    } else {
        (src.len(), 0)
    };

    let mut out = String::with_capacity(keep + dots);
    for &c in &src[..keep] {
        // Printable ASCII passes, everything else becomes '?'. Dropping the ESC
        // is what defuses a sequence; what follows it is ordinary text.
        out.push(if (0x20..0x7f).contains(&c) {
            c as char
        } else {
            '?'
        });
    }
    out.push_str(&ELLIPSIS[..dots]);
    out
}

/// Builds the response for one of 400, 404, 405, 431, 505; any other reads
/// `"Unknown"`. Pure and infallible, and an error carries a real HTML body
/// because an empty one renders as a blank page. Only a 405 `allow`s.
pub fn error_response(status: u16) -> Response<'static> {
    let reason = status_reason(status);

    // One template, so adding a status is a row in status_reason and nothing
    // else.
    let head = format!("{status} {reason}");
    let body = format!(
        "<!DOCTYPE html>\n<html lang=\"en\">\n\
         <head><meta charset=\"utf-8\"><title>{head}</title></head>\n\
         <body><h1>{head}</h1></body>\n\
         </html>\n"
    );

    Response {
        status,
        reason,
        body: Cow::Owned(body.into_bytes()),
        // Required by the RFC on a 405, and what tells a PUT what would have
        // worked.
        allow: (status == 405).then_some(ALLOWED_METHODS),
    }
}

/// Steps past one leading empty line, per RFC 7230 3.5. Exactly one, so a block
/// of blank lines stays malformed. Shared by the parser and the log, so the log
/// cannot describe a different line than the one that was rejected.
fn skip_one_blank_line(block: &[u8]) -> &[u8] {
    if let Some(rest) = block.strip_prefix(b"\r\n") {
        return rest;
    }
    block.strip_prefix(b"\n").unwrap_or(block)
}

/// The first line of `block`, not counting its terminator.
fn first_line(block: &[u8]) -> &[u8] {
    let line = match block.iter().position(|&b| b == b'\n') {
        Some(nl) => &block[..nl],
        None => block,
    };
    line.strip_suffix(b"\r").unwrap_or(line)
}

/// Reports whether the field is exactly `HTTP/<digit>.<digit>`.
fn version_shape_ok(field: &[u8]) -> bool {
    field.len() == 8
        && field.starts_with(b"HTTP/")
        && field[5].is_ascii_digit()
        && field[6] == b'.'
        && field[7].is_ascii_digit()
}

/// Splits off everything before the first `needle` byte, and everything after.
fn split_once(bytes: &[u8], needle: u8) -> Option<(&[u8], &[u8])> {
    let at = bytes.iter().position(|&b| b == needle)?;
    Some((&bytes[..at], &bytes[at + 1..]))
}

/// Parses the request line out of a header block. Pure: only the request line
/// is looked at, so there is no `Host` check and no body. Returns nothing at all
/// for anything the README's grammar refuses, NULs included, which is a 400.
pub fn parse_request(block: &[u8]) -> Option<Request<'_>> {
    let rest = skip_one_blank_line(block);

    // No terminator means the request line never ended, and a target cut off
    // midway is a different target.
    let (line, _) = split_once(rest, b'\n')?;
    let line = line.strip_suffix(b"\r").unwrap_or(line);

    // A NUL is refused, not treated as a terminator: everything downstream reads
    // the line as text, so "GET / HTTP/1.1\0junk" would otherwise pass.
    if line.contains(&0) {
        return None;
    }

    // Exactly three space-separated fields: two is HTTP/0.9, four means a space
    // in the target, which a client must percent-encode.
    let (method, tail) = split_once(line, b' ')?;
    let (target, version) = split_once(tail, b' ')?;
    if version.contains(&b' ') || method.is_empty() || target.is_empty() {
        return None;
    }

    // Malformed rather than unsupported: "HTTP/1" and "http/1.1" do not say
    // which version they meant, so there is nothing for routing to judge.
    if !version_shape_ok(version) {
        return None;
    }

    Some(Request {
        method: match method {
            b"GET" => Method::Get,
            b"HEAD" => Method::Head,
            // Well-formed but not one we serve. Methods are case-sensitive, so
            // "get" lands here and becomes a 405 rather than being corrected.
            _ => Method::Other,
        },
        // Verbatim: no request byte reaches the filesystem, so there is nothing
        // to decode for. Every target form is sorted out by routing.
        target,
        path: match split_once(target, b'?') {
            Some((path, _)) => path,
            None => target,
        },
        major: version[5] - b'0',
        minor: version[7] - b'0',
        line,
    })
}

/// Decides what to answer a well-formed request with. Pure and infallible -
/// there is no 500 here. The check order is the contract: version, then method,
/// then path. A `HEAD` routes exactly like a `GET`, body and all.
pub fn route<'a>(req: &Request<'_>, page: &'a [u8]) -> Response<'a> {
    if req.major != 1 {
        return error_response(505);
    }
    if req.method != Method::Get && req.method != Method::Head {
        return error_response(405);
    }
    if req.path != ROOT_PATH && req.path != INDEX_PATH {
        return error_response(404);
    }

    Response {
        status: 200,
        reason: status_reason(200),
        body: Cow::Borrowed(page),
        allow: None,
    }
}

/// Reads one header block into `out`, stopping at the blank line and never at
/// end of input, and filling `out` even on an error. Bytes past the terminator
/// are left unread; see [`crate::accept_once`].
pub fn read_request<R: Read>(
    input: &mut R,
    out: &mut Vec<u8>,
    cap: usize,
) -> Result<ReadOutcome, ConnectionError> {
    out.clear();

    let mut byte = [0u8; 1];
    loop {
        match input.read(&mut byte) {
            Ok(0) => {
                // End of input. At the first byte this is a browser's
                // speculative connection, which is ordinary and gets its own
                // variant so the log can stay calm about it.
                return Err(ConnectionError::Closed);
            }
            Ok(_) => {}
            // A bare read(2) surfaces EINTR where stdio restarts the read under
            // SA_RESTART, so it is retried here by hand.
            Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
            // SO_RCVTIMEO arrives as EAGAIN, which Rust reports as WouldBlock;
            // TimedOut is the same event spelled another way.
            Err(err)
                if err.kind() == io::ErrorKind::WouldBlock
                    || err.kind() == io::ErrorKind::TimedOut =>
            {
                return Err(ConnectionError::Timeout);
            }
            Err(err) => return Err(ConnectionError::Read(err)),
        }

        if out.len() == cap {
            return Ok(ReadOutcome::TooLarge);
        }
        out.push(byte[0]);

        // The blank line that ends the header block, in every spelling.
        // Requiring a '\n' before it keeps this off the request line's own CRLF.
        if out.ends_with(b"\n\n") || out.ends_with(b"\n\r\n") {
            return Ok(ReadOutcome::Complete);
        }
    }
}

fn put_response<W: Write>(out: &mut W, resp: &Response<'_>, suppress_body: bool) -> io::Result<()> {
    // The header order is fixed so the response bytes are golden and one
    // assertion can cover all of them.
    write!(out, "HTTP/1.1 {} {}\r\n", resp.status, resp.reason)?;
    write!(out, "Server: {SERVER_NAME}\r\n")?;
    write!(out, "Content-Type: {CONTENT_TYPE}\r\n")?;
    // Always the full body length, even for a HEAD.
    write!(out, "Content-Length: {}\r\n", resp.body.len())?;
    out.write_all(b"Connection: close\r\n")?;
    if let Some(allow) = resp.allow {
        write!(out, "Allow: {allow}\r\n")?;
    }
    out.write_all(b"\r\n")?;

    if !suppress_body {
        // write_all and never write!, so a '{' in a --file page is not a format
        // hole.
        out.write_all(&resp.body)?;
    }

    // Flushed here: the response must be on the socket before the shutdown, and
    // a buffered writer reports a failed write at the flush.
    out.flush()
}

/// Writes a response, header order fixed so the bytes are golden.
/// `suppress_body` is true for a `HEAD`, which keeps the `GET`'s headers,
/// `Content-Length` included. Neither that nor `Connection: close` is optional.
pub fn write_response<W: Write>(
    out: &mut W,
    resp: &Response<'_>,
    suppress_body: bool,
) -> Result<(), ConnectionError> {
    put_response(out, resp, suppress_body).map_err(ConnectionError::Write)
}

/// The whole transaction for one connection: read, parse, route, write, log.
/// Streams and not a socket, so it is testable with `&[u8]`; `input` and
/// `output` are two borrows of one `TcpStream`. Writes to `log` are best effort.
pub fn serve_connection<R: Read, W: Write, L: Write>(
    input: &mut R,
    output: &mut W,
    log: &mut L,
    page: &[u8],
) -> Transaction {
    let mut block = Vec::new();

    let outcome = match read_request(input, &mut block, REQUEST_MAX) {
        Ok(outcome) => outcome,
        // Every read failure leaves nobody to answer. TooLarge is the exception
        // and is not an error: that client is still there and is owed a 431.
        Err(failure) => {
            let _ = writeln!(log, "{PROG_NAME}: {failure}");
            return Transaction {
                result: Err(failure),
                left_unread: false,
            };
        }
    };

    let mut left_unread = false;
    let mut suppress_body = false;
    let resp = if outcome == ReadOutcome::TooLarge {
        let _ = writeln!(
            log,
            "{PROG_NAME}: request header block over {REQUEST_MAX} bytes"
        );
        // The rest of that request is still on its way, and the 431 below is
        // worth nothing if the close beats it there.
        left_unread = true;
        error_response(431)
    } else if let Some(req) = parse_request(&block) {
        let _ = writeln!(
            log,
            "{PROG_NAME}: request {}",
            sanitize(req.line, LOG_LINE_MAX)
        );
        suppress_body = req.method == Method::Head;
        route(&req, page)
    } else {
        // Sanitized and quoted, so an empty line is visible as one. Located the
        // way the parser locates it: the raw first line would log `malformed
        // request ""` for a request that opened with a stray CRLF.
        let shown = sanitize(first_line(skip_one_blank_line(&block)), LOG_LINE_MAX);
        let _ = writeln!(log, "{PROG_NAME}: malformed request \"{shown}\"");
        error_response(400)
    };

    if let Err(failure) = write_response(output, &resp, suppress_body) {
        let _ = writeln!(log, "{PROG_NAME}: {failure}");
        return Transaction {
            result: Err(failure),
            left_unread,
        };
    }

    // The count is what went out, so a HEAD reports 0: the log records the wire,
    // the header records the resource.
    let sent = if suppress_body { 0 } else { resp.body.len() };
    let _ = writeln!(
        log,
        "{PROG_NAME}: response {} {} ({sent} bytes)",
        resp.status, resp.reason
    );

    Transaction {
        result: Ok(()),
        left_unread,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A reader that fails the way a socket does, so the three read outcomes are
    /// separable without one. The C++ port needs a `ReadErrorProbe` for this.
    struct FailingReader {
        before: Vec<u8>,
        kind: io::ErrorKind,
        at: usize,
    }

    impl FailingReader {
        fn new(before: &[u8], kind: io::ErrorKind) -> Self {
            Self {
                before: before.to_vec(),
                kind,
                at: 0,
            }
        }
    }

    impl Read for FailingReader {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if self.at < self.before.len() {
                buf[0] = self.before[self.at];
                self.at += 1;
                return Ok(1);
            }
            Err(io::Error::new(self.kind, "injected"))
        }
    }

    /// A reader that reports EINTR once, to pin the retry arm.
    struct InterruptOnce {
        rest: Vec<u8>,
        interrupted: bool,
    }

    impl Read for InterruptOnce {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if !self.interrupted {
                self.interrupted = true;
                return Err(io::Error::new(io::ErrorKind::Interrupted, "injected"));
            }
            if self.rest.is_empty() {
                return Ok(0);
            }
            buf[0] = self.rest.remove(0);
            Ok(1)
        }
    }

    /// A writer whose every write fails, so the write path is reachable.
    struct FailingWriter;

    impl Write for FailingWriter {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::Error::from(io::ErrorKind::BrokenPipe))
        }
        fn flush(&mut self) -> io::Result<()> {
            Err(io::Error::from(io::ErrorKind::BrokenPipe))
        }
    }

    /// Drives one whole transaction and hands back (response bytes, log).
    fn serve(request: &[u8]) -> (String, String) {
        serve_with_page(request, builtin_page())
    }

    fn serve_with_page(request: &[u8], page: &[u8]) -> (String, String) {
        let mut input = request;
        let mut out = Vec::new();
        let mut log = Vec::new();
        serve_connection(&mut input, &mut out, &mut log, page);
        (
            String::from_utf8_lossy(&out).into_owned(),
            String::from_utf8_lossy(&log).into_owned(),
        )
    }

    fn routed(request: &[u8]) -> u16 {
        let req = parse_request(request).expect("request should parse");
        route(&req, builtin_page()).status
    }

    // --- sanitize -------------------------------------------------------

    #[test]
    fn passes_printable_ascii_through_unchanged() {
        assert_eq!(sanitize(b"GET / HTTP/1.1", LOG_LINE_MAX), "GET / HTTP/1.1");
    }

    #[test]
    fn defuses_an_escape_sequence_by_dropping_the_escape() {
        assert_eq!(sanitize(b"/\x1b[2J", LOG_LINE_MAX), "/?[2J");
    }

    #[test]
    fn a_newline_cannot_forge_a_second_log_line() {
        assert_eq!(sanitize(b"a\nb", LOG_LINE_MAX), "a?b");
    }

    #[test]
    fn replaces_nul_and_high_bit_bytes() {
        assert_eq!(sanitize(b"a\0b\xffc\x7f", LOG_LINE_MAX), "a?b?c?");
    }

    #[test]
    fn keeps_a_line_of_exactly_the_limit_whole() {
        let line = vec![b'a'; LOG_LINE_MAX];
        let out = sanitize(&line, LOG_LINE_MAX);
        assert_eq!(out.len(), LOG_LINE_MAX);
        assert!(!out.ends_with("..."));
    }

    #[test]
    fn truncates_one_byte_over_the_limit_to_the_limit() {
        // The ellipsis displaces the last bytes rather than pushing the line
        // past max, which keeps every port's longest line 255 wide.
        let line = vec![b'a'; LOG_LINE_MAX + 1];
        let out = sanitize(&line, LOG_LINE_MAX);
        assert_eq!(out.len(), LOG_LINE_MAX);
        assert!(out.ends_with("..."));
    }

    #[test]
    fn keeps_what_fits_of_the_ellipsis_when_the_limit_is_tiny() {
        assert_eq!(sanitize(b"abcdef", 2), "..");
        assert_eq!(sanitize(b"abcdef", 4), "a...");
    }

    // --- parse_request --------------------------------------------------

    #[test]
    fn parses_an_ordinary_request_line() {
        let req = parse_request(b"GET /index.html HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(req.method, Method::Get);
        assert_eq!(req.target, b"/index.html");
        assert_eq!(req.path, b"/index.html");
        assert_eq!((req.major, req.minor), (1, 1));
        assert_eq!(req.line, b"GET /index.html HTTP/1.1");
    }

    #[test]
    fn accepts_a_lone_lf_line_ending() {
        let req = parse_request(b"GET / HTTP/1.1\n\n").expect("parses");
        assert_eq!(req.line, b"GET / HTTP/1.1");
    }

    #[test]
    fn takes_the_path_up_to_the_query_string() {
        let req = parse_request(b"GET /?a=1 HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(req.target, b"/?a=1");
        assert_eq!(req.path, b"/");
    }

    #[test]
    fn skips_exactly_one_leading_blank_line() {
        assert!(parse_request(b"\r\nGET / HTTP/1.1\r\n\r\n").is_some());
        assert!(parse_request(b"\nGET / HTTP/1.1\n\n").is_some());
        // Two is not one: a block of nothing but blank lines is still malformed.
        assert!(parse_request(b"\r\n\r\nGET / HTTP/1.1\r\n\r\n").is_none());
    }

    #[test]
    fn refuses_a_request_line_with_no_terminator() {
        assert!(parse_request(b"GET / HTTP/1.1").is_none());
    }

    #[test]
    fn refuses_a_nul_rather_than_truncating_at_it() {
        // Truncating would let this pass as an ordinary request for "/".
        assert!(parse_request(b"GET /\0x HTTP/1.1\r\n\r\n").is_none());
    }

    #[test]
    fn refuses_two_and_four_field_request_lines() {
        assert!(parse_request(b"GET /\r\n\r\n").is_none());
        assert!(parse_request(b"GET /a b HTTP/1.1\r\n\r\n").is_none());
    }

    #[test]
    fn refuses_an_empty_method_or_target() {
        assert!(parse_request(b" / HTTP/1.1\r\n\r\n").is_none());
        assert!(parse_request(b"GET  HTTP/1.1\r\n\r\n").is_none());
    }

    #[test]
    fn refuses_a_version_token_that_names_no_version() {
        // None of these says which version was meant, so there is nothing for
        // routing to judge and they are 400 rather than 505.
        for line in [
            &b"GET / HTTP/1\r\n\r\n"[..],
            b"GET / http/1.1\r\n\r\n",
            b"GET / HTTP/11.1\r\n\r\n",
            b"GET / HTTP/1.\r\n\r\n",
            b"GET / HTTP1.1\r\n\r\n",
        ] {
            assert!(
                parse_request(line).is_none(),
                "{line:?} should be malformed"
            );
        }
    }

    #[test]
    fn a_method_it_does_not_serve_still_parses() {
        // Rejecting it here would lose the Allow header a 405 carries.
        let req = parse_request(b"POST / HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(req.method, Method::Other);
    }

    #[test]
    fn methods_are_case_sensitive() {
        let req = parse_request(b"get / HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(req.method, Method::Other);
    }

    #[test]
    fn accepts_absolute_and_asterisk_form_targets() {
        assert_eq!(
            parse_request(b"GET http://host/ HTTP/1.1\r\n\r\n")
                .expect("parses")
                .target,
            b"http://host/"
        );
        assert_eq!(
            parse_request(b"PRI * HTTP/2.0\r\n\r\n")
                .expect("parses")
                .target,
            b"*"
        );
    }

    #[test]
    fn ignores_the_header_lines_after_the_request_line() {
        // No Host check: only the request line is parsed at all.
        let req = parse_request(b"GET / HTTP/1.1\r\nX-Junk: \0\xff\r\n\r\n").expect("parses");
        assert_eq!(req.line, b"GET / HTTP/1.1");
    }

    // --- route ----------------------------------------------------------

    #[test]
    fn serves_the_page_at_both_of_its_paths() {
        assert_eq!(routed(b"GET / HTTP/1.1\r\n\r\n"), 200);
        assert_eq!(routed(b"GET /index.html HTTP/1.1\r\n\r\n"), 200);
        assert_eq!(routed(b"HEAD / HTTP/1.1\r\n\r\n"), 200);
        assert_eq!(routed(b"GET /?a=1 HTTP/1.1\r\n\r\n"), 200);
    }

    #[test]
    fn the_version_outranks_the_method() {
        // An HTTP/2 preface is a 505 and not a 405: a method belongs to a
        // protocol. This is the status precedence, and it is contract.
        assert_eq!(routed(b"PRI * HTTP/2.0\r\n\r\n"), 505);
        assert_eq!(routed(b"POST / HTTP/2.0\r\n\r\n"), 505);
    }

    #[test]
    fn the_method_outranks_the_path() {
        assert_eq!(routed(b"POST /nope HTTP/1.1\r\n\r\n"), 405);
    }

    #[test]
    fn only_the_major_version_is_looked_at() {
        assert_eq!(routed(b"GET / HTTP/1.9\r\n\r\n"), 200);
        assert_eq!(routed(b"GET / HTTP/0.9\r\n\r\n"), 505);
    }

    #[test]
    fn a_lowercase_get_is_a_method_error() {
        assert_eq!(routed(b"get / HTTP/1.1\r\n\r\n"), 405);
    }

    #[test]
    fn favicon_is_an_ordinary_not_found() {
        // Every browser asks for it unprompted, so this is the log's most common
        // line. It is intentional, not a bug to special-case away.
        assert_eq!(routed(b"GET /favicon.ico HTTP/1.1\r\n\r\n"), 404);
    }

    #[test]
    fn a_percent_encoded_path_is_not_decoded() {
        // Nothing here reaches the filesystem, so there is no path to normalize.
        assert_eq!(routed(b"GET /%69ndex.html HTTP/1.1\r\n\r\n"), 404);
    }

    #[test]
    fn a_405_carries_an_allow_and_nothing_else_does() {
        let req = parse_request(b"POST / HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(route(&req, builtin_page()).allow, Some("GET, HEAD"));
        let req = parse_request(b"GET /nope HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(route(&req, builtin_page()).allow, None);
    }

    #[test]
    fn a_head_is_routed_exactly_like_a_get() {
        // Body included: withholding it is write_response's job, since the
        // length a HEAD reports must be the GET's.
        let req = parse_request(b"HEAD / HTTP/1.1\r\n\r\n").expect("parses");
        assert_eq!(route(&req, builtin_page()).body.as_ref(), builtin_page());
    }

    // --- bodies ---------------------------------------------------------

    #[test]
    fn the_builtin_page_is_the_length_the_contract_states() {
        assert_eq!(builtin_page().len(), 178);
    }

    #[test]
    fn every_error_body_is_the_length_the_contract_states() {
        for (status, len) in [(400, 145), (404, 141), (405, 159), (431, 185), (505, 175)] {
            assert_eq!(error_response(status).body.len(), len, "status {status}");
        }
    }

    #[test]
    fn an_error_body_titles_and_headlines_the_status() {
        let resp = error_response(404);
        let body = String::from_utf8(resp.body.into_owned()).expect("ascii");
        assert!(body.contains("<title>404 Not Found</title>"));
        assert!(body.contains("<h1>404 Not Found</h1>"));
    }

    // --- write_response -------------------------------------------------

    #[test]
    fn writes_the_headers_in_the_fixed_order() {
        let (out, _) = serve(b"GET / HTTP/1.1\r\n\r\n");
        assert_eq!(
            out,
            "HTTP/1.1 200 OK\r\n\
             Server: tiny_http_server\r\n\
             Content-Type: text/html; charset=utf-8\r\n\
             Content-Length: 178\r\n\
             Connection: close\r\n\
             \r\n\
             <!DOCTYPE html>\n\
             <html lang=\"en\">\n\
             <head><meta charset=\"utf-8\"><title>tiny_http_server</title></head>\n\
             <body><h1>Hello, world!</h1><p>Served by tiny_http_server.</p></body>\n\
             </html>\n"
        );
    }

    #[test]
    fn a_head_reports_the_length_the_get_would_have_and_sends_no_body() {
        let (out, _) = serve(b"HEAD / HTTP/1.1\r\n\r\n");
        assert!(out.contains("Content-Length: 178\r\n"));
        assert!(out.ends_with("Connection: close\r\n\r\n"));
    }

    #[test]
    fn the_allow_header_comes_last_and_only_on_a_405() {
        let (out, _) = serve(b"POST / HTTP/1.1\r\n\r\n");
        assert!(out.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
        assert!(out.contains("Connection: close\r\nAllow: GET, HEAD\r\n\r\n"));
    }

    #[test]
    fn an_empty_page_is_served_with_a_zero_length() {
        let (out, _) = serve_with_page(b"GET / HTTP/1.1\r\n\r\n", b"");
        assert!(out.contains("Content-Length: 0\r\n"));
        assert!(out.ends_with("\r\n\r\n"));
    }

    #[test]
    fn a_page_with_braces_is_not_a_format_hole() {
        let (out, _) = serve_with_page(b"GET / HTTP/1.1\r\n\r\n", b"{{}} {0} {x}");
        assert!(out.ends_with("{{}} {0} {x}"));
    }

    // --- read_request ---------------------------------------------------

    #[test]
    fn accepts_all_four_spellings_of_the_blank_line() {
        for block in [
            &b"GET / HTTP/1.1\r\n\r\n"[..],
            b"GET / HTTP/1.1\r\n\n",
            b"GET / HTTP/1.1\n\r\n",
            b"GET / HTTP/1.1\n\n",
        ] {
            let mut out = Vec::new();
            assert_eq!(
                read_request(&mut &block[..], &mut out, REQUEST_MAX).expect("reads"),
                ReadOutcome::Complete
            );
            assert_eq!(out, block);
        }
    }

    #[test]
    fn stops_at_the_blank_line_and_leaves_the_body_unread() {
        // Reading to end of input blocks until the timeout against a real
        // client, so nothing here may consume past the terminator.
        let mut input = &b"GET / HTTP/1.1\r\n\r\nhello"[..];
        let mut out = Vec::new();
        read_request(&mut input, &mut out, REQUEST_MAX).expect("reads");
        assert_eq!(out, b"GET / HTTP/1.1\r\n\r\n");
        assert_eq!(input, b"hello");
    }

    #[test]
    fn does_not_fire_on_the_crlf_that_ends_the_request_line() {
        let mut input = &b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"[..];
        let mut out = Vec::new();
        read_request(&mut input, &mut out, REQUEST_MAX).expect("reads");
        assert_eq!(out, b"GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    }

    #[test]
    fn reports_a_block_that_reaches_the_cap_as_too_large() {
        let block = [b'a'; 64];
        let mut out = Vec::new();
        assert_eq!(
            read_request(&mut &block[..], &mut out, 16).expect("reads"),
            ReadOutcome::TooLarge
        );
        assert_eq!(out.len(), 16);
    }

    #[test]
    fn tells_a_clean_close_from_a_timeout_from_a_read_error() {
        // The three cases the C++ port needs a ReadErrorProbe to separate.
        let mut out = Vec::new();
        assert!(matches!(
            read_request(&mut &b""[..], &mut out, REQUEST_MAX),
            Err(ConnectionError::Closed)
        ));
        assert!(matches!(
            read_request(
                &mut FailingReader::new(b"", io::ErrorKind::WouldBlock),
                &mut out,
                REQUEST_MAX
            ),
            Err(ConnectionError::Timeout)
        ));
        assert!(matches!(
            read_request(
                &mut FailingReader::new(b"", io::ErrorKind::TimedOut),
                &mut out,
                REQUEST_MAX
            ),
            Err(ConnectionError::Timeout)
        ));
        assert!(matches!(
            read_request(
                &mut FailingReader::new(b"", io::ErrorKind::ConnectionReset),
                &mut out,
                REQUEST_MAX
            ),
            Err(ConnectionError::Read(_))
        ));
    }

    #[test]
    fn keeps_a_partial_block_so_the_caller_can_see_it() {
        let mut out = Vec::new();
        let _ = read_request(&mut &b"GET / HT"[..], &mut out, REQUEST_MAX);
        assert_eq!(out, b"GET / HT");
    }

    #[test]
    fn retries_an_interrupted_read_rather_than_reporting_it() {
        let mut input = InterruptOnce {
            rest: b"GET / HTTP/1.1\r\n\r\n".to_vec(),
            interrupted: false,
        };
        let mut out = Vec::new();
        assert_eq!(
            read_request(&mut input, &mut out, REQUEST_MAX).expect("reads"),
            ReadOutcome::Complete
        );
    }

    // --- serve_connection -----------------------------------------------

    #[test]
    fn logs_the_request_and_the_response() {
        let (_, log) = serve(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        assert_eq!(
            log,
            "tiny_http_server: request GET / HTTP/1.1\n\
             tiny_http_server: response 200 OK (178 bytes)\n"
        );
    }

    #[test]
    fn a_head_logs_the_bytes_that_went_on_the_wire() {
        // 0, not 178: the log records what happened, the header records what
        // the resource is.
        let (_, log) = serve(b"HEAD / HTTP/1.1\r\n\r\n");
        assert!(log.ends_with("response 200 OK (0 bytes)\n"));
    }

    #[test]
    fn logs_a_malformed_request_line_sanitized_and_quoted() {
        let (out, log) = serve(b"GET /\x1b[2J\n x HTTP/1.1\r\n\r\n");
        assert!(out.starts_with("HTTP/1.1 400 Bad Request\r\n"));
        assert_eq!(
            log,
            "tiny_http_server: malformed request \"GET /?[2J\"\n\
             tiny_http_server: response 400 Bad Request (145 bytes)\n"
        );
    }

    #[test]
    fn logs_the_line_the_parser_judged_not_the_raw_first_one() {
        // With a stray leading CRLF, the raw first line would say `malformed
        // request ""` and hide the bytes that caused the 400.
        let (_, log) = serve(b"\r\nGET /\r\n\r\n");
        assert!(log.starts_with("tiny_http_server: malformed request \"GET /\"\n"));
    }

    #[test]
    fn an_oversized_header_block_is_answered_with_a_431() {
        let mut input = vec![];
        input.extend_from_slice(b"GET / HTTP/1.1\r\nX-Pad: ");
        input.resize(REQUEST_MAX * 2, b'a');
        let mut out = Vec::new();
        let mut log = Vec::new();
        let tx = serve_connection(&mut &input[..], &mut out, &mut log, builtin_page());

        assert!(tx.result.is_ok());
        // Not an error: that client is still there and is owed a response, and
        // the caller has to know the rest of the request is in flight.
        assert!(tx.left_unread);
        assert!(String::from_utf8_lossy(&out).starts_with("HTTP/1.1 431 "));
        assert_eq!(
            String::from_utf8_lossy(&log),
            "tiny_http_server: request header block over 8192 bytes\n\
             tiny_http_server: response 431 Request Header Fields Too Large (185 bytes)\n"
        );
    }

    #[test]
    fn a_client_that_sends_nothing_gets_no_response_and_a_calm_log_line() {
        let (out, log) = serve(b"");
        assert_eq!(out, "");
        assert_eq!(
            log,
            "tiny_http_server: client closed the connection without sending a request\n"
        );
    }

    #[test]
    fn a_receive_timeout_is_logged_as_one_and_not_as_a_hang_up() {
        let mut input = FailingReader::new(b"GET / HT", io::ErrorKind::WouldBlock);
        let mut out = Vec::new();
        let mut log = Vec::new();
        let tx = serve_connection(&mut input, &mut out, &mut log, builtin_page());

        assert!(matches!(tx.result, Err(ConnectionError::Timeout)));
        assert_eq!(
            String::from_utf8_lossy(&log),
            "tiny_http_server: client sent nothing before the read timeout\n"
        );
    }

    #[test]
    fn a_client_that_hangs_up_mid_response_is_a_write_failure_that_still_counts() {
        let mut log = Vec::new();
        let tx = serve_connection(
            &mut &b"GET / HTTP/1.1\r\n\r\n"[..],
            &mut FailingWriter,
            &mut log,
            builtin_page(),
        );

        let failure = tx.result.expect_err("the write failed");
        // --once must still stop on this: the response was written, the client
        // left before taking it.
        assert!(failure.answered());
        assert_eq!(
            String::from_utf8_lossy(&log),
            "tiny_http_server: request GET / HTTP/1.1\n\
             tiny_http_server: error writing the response\n"
        );
    }

    #[test]
    fn a_closed_log_never_ends_the_transaction() {
        // Log writes are best effort: a server that exits because somebody
        // closed its stderr is worse than one nobody is recording.
        let mut out = Vec::new();
        let tx = serve_connection(
            &mut &b"GET / HTTP/1.1\r\n\r\n"[..],
            &mut out,
            &mut FailingWriter,
            builtin_page(),
        );
        assert!(tx.result.is_ok());
        assert!(String::from_utf8_lossy(&out).starts_with("HTTP/1.1 200 OK\r\n"));
    }

    #[test]
    fn a_long_target_is_an_ordinary_404_with_a_truncated_log_line() {
        // There is no 414.
        let mut request = b"GET /".to_vec();
        request.resize(4005, b'a');
        request.extend_from_slice(b" HTTP/1.1\r\n\r\n");
        let (out, log) = serve(&request);

        assert!(out.starts_with("HTTP/1.1 404 Not Found\r\n"));
        let line = log.lines().next().expect("a request line");
        assert_eq!(
            line.len(),
            "tiny_http_server: request ".len() + LOG_LINE_MAX
        );
        assert!(line.ends_with("..."));
    }
}
