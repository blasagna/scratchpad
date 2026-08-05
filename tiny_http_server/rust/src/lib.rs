//! A very small HTTP server: bind a socket, then accept one connection at a
//! time, read the request header block, answer `GET` and `HEAD` of `/` with a
//! hello world page, and log every event to stderr.
//!
//! The C port is the reference dialect and `../README.md` is the contract every
//! port is measured against; `../check_parity.sh` diffs the response bytes, the
//! log, and the exit status of all three. `http` is the protocol and is either
//! pure or generic over `Read`/`Write`, so the whole request-to-response
//! transaction is exercised with `&[u8]` and `Vec<u8>` and no socket anywhere;
//! `server` is the socket layer and the `--file` loader.

mod http;
mod server;

pub use http::{
    CONTENT_TYPE, ConnectionError, LOG_LINE_MAX, Method, PROG_NAME, REQUEST_MAX, ReadOutcome,
    Request, Response, SERVER_NAME, Transaction, builtin_page, error_response, parse_request,
    read_request, route, sanitize, serve_connection, status_reason, write_response,
};
pub use server::{
    DEFAULT_HOST, DEFAULT_PORT, DEFAULT_TIMEOUT, MAX_PAGE_BYTES, Options, Outcome, PageError,
    ServerError, accept_once, listen, load_page, os_message, run,
};
