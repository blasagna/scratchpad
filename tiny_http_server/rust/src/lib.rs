//! A very small HTTP server: bind a socket, then answer one connection at a
//! time with a hello world page, logging every event to stderr. `../README.md`
//! is the contract; `http` is the protocol and `server` is the socket layer.

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
