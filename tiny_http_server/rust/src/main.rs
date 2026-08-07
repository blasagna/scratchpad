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
    /// `u16` is the whole validator: exactly 0-65535, base 10. See the README.
    #[arg(short, long, default_value_t = DEFAULT_PORT, value_name = "N")]
    port: u16,

    /// IPv4 address to bind
    ///
    /// `Ipv4Addr`'s parser is `inet_pton`'s grammar, validator-free.
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
    // clap owns every usage error and exits 2, so this port has no usage path of
    // its own. Requests arrive over the socket, never from argv, so there is no
    // positional to declare and a stray operand is clap's to reject.
    let cli = Cli::parse();

    // There is no signal(SIGPIPE, SIG_IGN) here, and that is not an omission:
    // the Rust runtime sets that disposition before main runs. Do not add a call
    // back - an explicit SIG_DFL is the only way to reintroduce the bug.

    // The page is read once, before the socket exists, so an unreadable file is
    // a startup failure rather than a 500 later - and routing stays pure, which
    // is why this server has no 500 at all.
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

    // The log is the caller's stream rather than a global, which is what lets
    // every test in the library hand in a Vec<u8>.
    let result = run(&opts, &mut io::stderr().lock());

    match result {
        // Whatever any request was answered with. A 404 is the server working.
        Ok(()) => ExitCode::SUCCESS,
        // Display composes the failed stage and the errno, so both shapes of
        // failure come out of one arm.
        Err(failure) => {
            eprintln!("{PROG_NAME}: {failure}");
            ExitCode::FAILURE
        }
    }
}
