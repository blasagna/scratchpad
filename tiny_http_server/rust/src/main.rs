//! CLI for the tiny HTTP server: binds a socket, then answers one connection at
//! a time until something fatal happens.

use std::io;
use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;

use tiny_http_server::{
    DEFAULT_HOST, DEFAULT_PORT, MAX_PAGE_BYTES, Options, PROG_NAME, builtin_page, load_page, run,
};

#[derive(Parser)]
#[command(
    name = "tiny_http_server",
    about = "A very small HTTP server that answers one connection at a time.",
    long_about = "A very small HTTP server. Binds a socket, then repeats: accept one\n\
                  connection, read the request, answer it, close, accept the next.\n\
                  Open http://127.0.0.1:8080 in a browser to see the page.\n\
                  \n\
                  GET and HEAD of '/' or '/index.html' return 200 with a hello world\n\
                  page. Any other path is 404, any other method is 405, a request\n\
                  line that cannot be parsed is 400, and a version other than\n\
                  HTTP/1.x is 505. Every event is logged to stderr.",
    after_help = "Connections are served one at a time, so this is a toy rather than\n\
                  a web server: any one slow client stalls the next. That is why the\n\
                  default binds loopback only -- pass --host 0.0.0.0 to expose it on\n\
                  every interface, deliberately."
)]
struct Cli {
    /// port to listen on, 0 to let the kernel pick
    ///
    /// u16 is the whole validator, and it is the repo's "let the library carry
    /// the grammar" rule landing unusually well: the type's range is exactly
    /// 0-65535, it reads base 10 (so `08080` is eight thousand and eighty, as in
    /// C and unlike C++'s octal), it takes a leading `+`, and it turns down
    /// `abc`, `0x1F90`, `8_080`, `"8080 "`, `-1`, and `65536`. A hand-written
    /// validator to match C exactly is the mistake text_analyzer documented.
    #[arg(short, long, default_value_t = DEFAULT_PORT, value_name = "N")]
    port: u16,

    /// IPv4 address to bind
    ///
    /// `Ipv4Addr`'s own parser, which is `inet_pton`'s grammar: it rejects
    /// `localhost` (names are not resolved - DNS is a blocking network lookup
    /// and would hand back a list of candidates for a program that binds exactly
    /// one socket), and it rejects `127.1` and `0177.0.0.1` the same way, since
    /// leading zeros are refused rather than read as octal. The C++ port needs a
    /// hand-written `->check()` here because no CLI11 built-in states that rule;
    /// this port needs nothing at all.
    #[arg(long, default_value_t = DEFAULT_HOST, value_name = "ADDR")]
    host: Ipv4Addr,

    /// serve this file instead of the built-in page
    #[arg(long, value_name = "PATH")]
    file: Option<PathBuf>,

    /// serve one request, then exit
    #[arg(long)]
    once: bool,
}

fn main() -> ExitCode {
    // clap owns every usage error here and exits 2 for one, so this port has no
    // usage-error path of its own. Requests arrive over the socket, never from
    // argv, so there is no positional to declare and a stray operand is clap's
    // to reject.
    let cli = Cli::parse();

    // There is no signal(SIGPIPE, SIG_IGN) here, and that is not an omission:
    // the Rust runtime sets that disposition before main runs. Both other ports
    // have to call it - writing to a socket whose peer has gone raises SIGPIPE,
    // whose default action is to terminate the process with no message at all,
    // so without it the server vanishes the first time somebody navigates away
    // mid-response and the symptom is "it just disappears sometimes". It also
    // covers `tiny_http_server 2>&1 | head`, which would otherwise kill the
    // server on the closed log pipe. Do not add a call back: an explicit
    // SIG_DFL or a second mechanism is the only way to reintroduce the bug.

    // The page is read once, here, before the socket exists. A file that cannot
    // be read is then a startup failure with a message, rather than a 500 that
    // turns up later depending on which path somebody visits - and routing stays
    // pure, with no I/O and no failure path, which is why this server has no 500
    // at all. The cost is that the page cannot change while the server runs.
    let loaded = match &cli.file {
        Some(path) => match load_page(path, MAX_PAGE_BYTES) {
            Ok(page) => Some(page),
            Err(err) => {
                eprintln!("{PROG_NAME}: {}: {err}", path.display());
                return ExitCode::FAILURE;
            }
        },
        None => None,
    };

    let opts = Options {
        host: cli.host,
        port: cli.port,
        serve_once: cli.once,
        page: loaded.as_deref().unwrap_or(builtin_page()),
        ..Options::default()
    };

    // The log is the caller's stream rather than a global reached from inside,
    // which is what lets every test in the library hand in a Vec<u8>.
    let result = run(&opts, &mut io::stderr().lock());

    match result {
        // Whatever any request was answered with. A 404 is the server working.
        Ok(()) => ExitCode::SUCCESS,
        // Display composes the failed stage and the errno, so the server's two
        // shapes of failure come out of one arm.
        Err(failure) => {
            eprintln!("{PROG_NAME}: {failure}");
            ExitCode::FAILURE
        }
    }
}
