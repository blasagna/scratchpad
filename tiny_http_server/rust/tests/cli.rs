//! End-to-end tests that spawn the binary.
//!
//! Two things live here that no unit test in the library can reach. The first is
//! everything clap owns - which command lines are accepted, and what a rejected
//! one exits with - since `Cli` is private to `main.rs` and its behavior only
//! exists once a process has argv. The second is `run` itself: a client has to
//! be connected before the loop accepts it and `run` does its own binding, so
//! nothing single-threaded can be waiting on the queue by the time it starts.
//! The C and C++ ports list that one under "checked by hand"; here it is a test.

use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{Shutdown, TcpStream};
use std::path::PathBuf;
use std::process::{self, Command, Output, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};

const BIN: &str = env!("CARGO_BIN_EXE_tiny_http_server");

/// The exit code clap uses for a usage error. The same one the C port's
/// `getopt_long` path returns, which is a coincidence rather than a contract:
/// the C++ port's CLI11 brings its own codes and the parity script only requires
/// that every port rejects the same command line.
const EXIT_USAGE: i32 = 2;

fn run_with(args: &[&str]) -> Output {
    Command::new(BIN).args(args).output().expect("runs")
}

fn scratch(tag: &str) -> PathBuf {
    static COUNTER: AtomicU32 = AtomicU32::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    let mut path = PathBuf::from(env!("CARGO_TARGET_TMPDIR"));
    path.push(format!("{tag}-{}-{n}", process::id()));
    path
}

// --- what clap owns -----------------------------------------------------

#[test]
fn help_exits_zero_and_describes_the_server() {
    let out = run_with(&["--help"]);
    assert_eq!(out.status.code(), Some(0));
    let text = String::from_utf8(out.stdout).expect("utf-8");
    assert!(text.contains("--port"));
    assert!(text.contains("--host"));
    assert!(text.contains("--file"));
    assert!(text.contains("--once"));
}

#[test]
fn rejects_an_unknown_option_and_a_stray_operand() {
    // There are no operands: requests arrive over the socket, never from argv.
    for args in [&["--nope"][..], &["extra"], &["--file"]] {
        let out = run_with(args);
        assert_eq!(out.status.code(), Some(EXIT_USAGE), "{args:?}");
        assert!(out.stdout.is_empty(), "{args:?}");
    }
}

#[test]
fn rejects_a_port_that_is_not_one() {
    for value in [
        "",
        "abc",
        "-1",
        "65536",
        "8080 ",
        " 8080",
        "99999999999999999999",
    ] {
        let out = run_with(&["--port", value]);
        assert_eq!(out.status.code(), Some(EXIT_USAGE), "--port {value:?}");
    }
}

#[test]
fn reads_a_port_in_base_ten_like_the_c_port_does() {
    // The spellings CLI11 accepts and this port does not, which is the recorded
    // divergence running the other way: C++ reads base 0 and strips group
    // separators, so it takes both of these and reads a leading zero as octal.
    for value in ["0x1F90", "8_080"] {
        let out = run_with(&["--port", value]);
        assert_eq!(out.status.code(), Some(EXIT_USAGE), "--port {value:?}");
    }
}

#[test]
fn rejects_a_host_that_is_not_a_dotted_quad() {
    // inet_pton's grammar, which is Ipv4Addr's: no names, and no leading zeros
    // to be read as octal by one library and as decimal by another.
    for value in ["localhost", "127.1", "0177.0.0.1", "::1", "1.2.3.4.5", ""] {
        let out = run_with(&["--host", value]);
        assert_eq!(out.status.code(), Some(EXIT_USAGE), "--host {value:?}");
    }
}

// --- startup failures ---------------------------------------------------

#[test]
fn reports_a_missing_page_file_and_exits_one() {
    let path = scratch("missing");
    let out = run_with(&["--file", path.to_str().expect("utf-8 path")]);
    assert_eq!(out.status.code(), Some(1));
    assert_eq!(
        String::from_utf8(out.stderr).expect("utf-8"),
        format!(
            "tiny_http_server: {}: No such file or directory\n",
            path.display()
        )
    );
}

#[test]
fn reports_a_directory_as_a_page_file_and_exits_one() {
    // fopen on a directory succeeds on Linux; catching it needs a stat, not a
    // failed read, or the message says "cannot read the page file".
    let dir = env!("CARGO_TARGET_TMPDIR");
    let out = run_with(&["--file", dir]);
    assert_eq!(out.status.code(), Some(1));
    assert_eq!(
        String::from_utf8(out.stderr).expect("utf-8"),
        format!("tiny_http_server: {dir}: not a regular file\n")
    );
}

#[test]
fn refuses_a_page_file_over_the_cap_and_exits_one() {
    let path = scratch("huge");
    fs::write(&path, vec![b'x'; 1024 * 1024 + 1]).expect("writes");
    let out = run_with(&["--file", path.to_str().expect("utf-8 path")]);
    let _ = fs::remove_file(&path);

    assert_eq!(out.status.code(), Some(1));
    assert_eq!(
        String::from_utf8(out.stderr).expect("utf-8"),
        format!(
            "tiny_http_server: {}: larger than the 1048576 byte limit\n",
            path.display()
        )
    );
}

// --- run ----------------------------------------------------------------

#[test]
fn serves_one_request_over_a_real_socket_and_exits_zero() {
    let mut child = Command::new(BIN)
        .args(["--host", "127.0.0.1", "--port", "0", "--once"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawns");

    // --port 0 is a feature, not a placeholder: a fixed port collides with the
    // server somebody left running in another terminal, which is exactly when
    // this is being run. The kernel's choice comes back in the listening line.
    let mut log = BufReader::new(child.stderr.take().expect("stderr is piped"));
    let mut first = String::new();
    log.read_line(&mut first).expect("reads the listening line");

    let port: u16 = first
        .trim_end()
        .rsplit(':')
        .next()
        .and_then(|port| port.parse().ok())
        .unwrap_or_else(|| panic!("no port in {first:?}"));
    assert_ne!(port, 0, "--port 0 must report the port it really bound");
    assert_eq!(
        first,
        format!("tiny_http_server: listening on 127.0.0.1:{port}\n")
    );

    let mut client = TcpStream::connect(("127.0.0.1", port)).expect("connects");
    client
        .write_all(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        .expect("sends");
    client.shutdown(Shutdown::Write).expect("half-closes");

    let mut response = Vec::new();
    client.read_to_end(&mut response).expect("reads");
    assert!(response.starts_with(b"HTTP/1.1 200 OK\r\n"));
    assert!(response.ends_with(b"</html>\n"));

    let mut rest = String::new();
    log.read_to_string(&mut rest).expect("reads the rest");
    assert!(rest.starts_with("tiny_http_server: connection from 127.0.0.1:"));
    assert!(rest.ends_with(
        "tiny_http_server: request GET / HTTP/1.1\n\
         tiny_http_server: response 200 OK (178 bytes)\n\
         tiny_http_server: connection closed\n"
    ));

    // Exit 0 regardless of what the request was answered with: the loop ran.
    assert_eq!(child.wait().expect("waits").code(), Some(0));
}
