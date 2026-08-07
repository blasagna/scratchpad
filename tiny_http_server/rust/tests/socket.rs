//! The socket layer and the `--file` loader, against the real thing. No seam:
//! `connect()` completes in the kernel via the accept queue whether or not
//! `accept` has been called, so this needs no thread, no fork, and no sleep.

use std::fs;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddrV4, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

use tiny_http_server::{
    MAX_PAGE_BYTES, Options, Outcome, PageError, ServerError, accept_once, builtin_page, listen,
    load_page, run,
};

/// A unique path under the target directory. No `tempfile` crate: the ports here
/// take only `clap`.
fn scratch(tag: &str) -> PathBuf {
    static COUNTER: AtomicU32 = AtomicU32::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    let mut path = PathBuf::from(env!("CARGO_TARGET_TMPDIR"));
    path.push(format!("{tag}-{}-{n}", process::id()));
    path
}

fn bound_listener() -> (TcpListener, SocketAddrV4) {
    let opts = Options {
        port: 0,
        ..Options::default()
    };
    let listener = listen(&opts).expect("binds an ephemeral port");
    match listener.local_addr().expect("reports its address") {
        std::net::SocketAddr::V4(addr) => (listener, addr),
        other => panic!("expected an IPv4 address, got {other}"),
    }
}

fn quick(page: &[u8]) -> Options<'_> {
    Options {
        port: 0,
        // Short enough that the timeout cases do not slow the suite down.
        io_timeout: Duration::from_millis(150),
        page,
        ..Options::default()
    }
}

/// Connects, sends, and closes its write side - which is what lets the whole
/// thing run on one thread.
fn client_sending(addr: SocketAddrV4, request: &[u8]) -> TcpStream {
    let mut client = TcpStream::connect(addr).expect("connects");
    client.write_all(request).expect("sends");
    client.shutdown(Shutdown::Write).expect("half-closes");
    client
}

fn read_all(mut client: TcpStream) -> Vec<u8> {
    let mut response = Vec::new();
    client.read_to_end(&mut response).expect("reads");
    response
}

// --- load_page ----------------------------------------------------------

#[test]
fn loads_a_page_file() {
    let path = scratch("page");
    fs::write(&path, b"<p>hi</p>\n").expect("writes");
    assert_eq!(
        load_page(&path, MAX_PAGE_BYTES).expect("loads"),
        b"<p>hi</p>\n"
    );
    let _ = fs::remove_file(&path);
}

#[test]
fn an_empty_page_file_is_a_page_of_zero_bytes_and_not_an_error() {
    let path = scratch("empty");
    fs::write(&path, b"").expect("writes");
    assert!(load_page(&path, MAX_PAGE_BYTES).expect("loads").is_empty());
    let _ = fs::remove_file(&path);
}

#[test]
fn reports_a_missing_page_file_with_the_error_intact() {
    let err = load_page(&scratch("missing"), MAX_PAGE_BYTES).expect_err("fails");
    assert!(matches!(&err, PageError::Open(_)));
    // And without the "(os error 2)" the other two ports do not print.
    assert_eq!(err.to_string(), "No such file or directory");
}

#[test]
fn refuses_a_directory_rather_than_failing_inside_the_read() {
    // Opening a directory succeeds on Linux; only the read fails, with EISDIR,
    // which would be reported as "cannot read the page file".
    let err = load_page(Path::new(env!("CARGO_TARGET_TMPDIR")), MAX_PAGE_BYTES).expect_err("fails");
    assert!(matches!(err, PageError::NotRegular));
}

#[test]
fn refuses_a_page_over_the_cap_rather_than_truncating_it() {
    let path = scratch("big");
    fs::write(&path, vec![b'x'; 64]).expect("writes");
    // Exactly the cap is fine; one byte over is not.
    assert!(load_page(&path, 64).is_ok());
    assert!(matches!(
        load_page(&path, 63).expect_err("fails"),
        PageError::TooLarge
    ));
    let _ = fs::remove_file(&path);
}

// --- listen -------------------------------------------------------------

#[test]
fn port_zero_reports_the_port_it_really_bound() {
    let (_listener, addr) = bound_listener();
    assert_ne!(addr.port(), 0);
    assert_eq!(*addr.ip(), Ipv4Addr::LOCALHOST);
}

#[test]
fn refuses_a_second_listener_on_the_same_port() {
    // SO_REUSEADDR is not SO_REUSEPORT, and std sets only the first. Swapping
    // one for the other would let two servers share a port; this says so.
    let (_listener, addr) = bound_listener();
    let opts = Options {
        port: addr.port(),
        ..Options::default()
    };
    assert!(matches!(listen(&opts), Err(ServerError::Bind(_))));
}

// --- accept_once --------------------------------------------------------

#[test]
fn serves_a_request_over_a_real_socket() {
    let (listener, addr) = bound_listener();
    let client = client_sending(addr, b"GET / HTTP/1.1\r\nHost: x\r\n\r\n");

    let mut log = Vec::new();
    let outcome = accept_once(&listener, &quick(builtin_page()), &mut log).expect("no fatal error");
    assert_eq!(outcome, Outcome::Answered);

    let response = read_all(client);
    assert!(response.starts_with(b"HTTP/1.1 200 OK\r\n"));
    assert!(response.ends_with(builtin_page()));

    let log = String::from_utf8(log).expect("ascii");
    assert!(log.starts_with("tiny_http_server: connection from 127.0.0.1:"));
    assert!(log.ends_with(
        "tiny_http_server: request GET / HTTP/1.1\n\
         tiny_http_server: response 200 OK (178 bytes)\n\
         tiny_http_server: connection closed\n"
    ));
}

#[test]
fn serves_the_file_page_when_one_is_given() {
    let (listener, addr) = bound_listener();
    let client = client_sending(addr, b"GET / HTTP/1.1\r\n\r\n");

    let mut log = Vec::new();
    accept_once(&listener, &quick(b"<p>from --file</p>\n"), &mut log).expect("no fatal error");

    let response = read_all(client);
    assert!(response.ends_with(b"Connection: close\r\n\r\n<p>from --file</p>\n"));
}

#[test]
fn delivers_the_405_past_a_request_body_it_never_read() {
    // Linux sends an RST when a socket closes with unread inbound data, and a
    // peer may discard what it already received - so without the shutdown and
    // the drain, `curl -d x` sees a reset instead of the 405.
    let (listener, addr) = bound_listener();
    let client = client_sending(addr, b"POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");

    let mut log = Vec::new();
    accept_once(&listener, &quick(builtin_page()), &mut log).expect("no fatal error");

    let response = read_all(client);
    assert!(response.starts_with(b"HTTP/1.1 405 Method Not Allowed\r\n"));
    assert!(response.ends_with(b"</html>\n"));
}

#[test]
fn answers_an_oversized_header_block_with_a_431() {
    let (listener, addr) = bound_listener();
    let mut request = b"GET / HTTP/1.1\r\nX-Pad: ".to_vec();
    request.resize(20_000, b'a');
    let client = client_sending(addr, &request);

    let mut log = Vec::new();
    let outcome = accept_once(&listener, &quick(builtin_page()), &mut log).expect("no fatal error");
    // A 431 is a response, so that connection succeeded like any other and
    // --once stops on it.
    assert_eq!(outcome, Outcome::Answered);

    let response = read_all(client);
    assert!(response.starts_with(b"HTTP/1.1 431 Request Header Fields Too Large\r\n"));
    assert!(String::from_utf8_lossy(&log).contains("request header block over 8192 bytes"));
}

#[test]
fn a_silent_client_times_out_and_does_not_count_as_answered() {
    // A browser's speculative preconnect: connected, then nothing. --once must
    // not stop on it, or it exits having served nothing.
    let (listener, addr) = bound_listener();
    let _client = TcpStream::connect(addr).expect("connects");

    let mut log = Vec::new();
    let outcome = accept_once(&listener, &quick(builtin_page()), &mut log).expect("no fatal error");
    assert_eq!(outcome, Outcome::Unanswered);

    let log = String::from_utf8(log).expect("ascii");
    // The timeout, and not an ordinary hang-up: the difference SO_RCVTIMEO
    // exists for, and the one a std::streambuf cannot see.
    assert!(log.contains("tiny_http_server: client sent nothing before the read timeout\n"));
    assert!(log.ends_with("tiny_http_server: connection closed\n"));
}

#[test]
fn a_client_that_hangs_up_without_sending_is_logged_calmly() {
    let (listener, addr) = bound_listener();
    let client = TcpStream::connect(addr).expect("connects");
    client.shutdown(Shutdown::Both).expect("closes");

    let mut log = Vec::new();
    let outcome = accept_once(&listener, &quick(builtin_page()), &mut log).expect("no fatal error");
    assert_eq!(outcome, Outcome::Unanswered);
    assert!(
        String::from_utf8_lossy(&log)
            .contains("tiny_http_server: client closed the connection without sending a request\n")
    );
}

// --- run ----------------------------------------------------------------

#[test]
fn a_fatal_bind_failure_ends_the_server_before_it_listens() {
    let (_listener, addr) = bound_listener();
    let opts = Options {
        port: addr.port(),
        ..Options::default()
    };

    let mut log = Vec::new();
    assert!(matches!(run(&opts, &mut log), Err(ServerError::Bind(_))));
    // Nothing was logged: it never got as far as listening.
    assert!(log.is_empty());
}
