#ifndef TINY_HTTP_SERVER_HTTP_H
#define TINY_HTTP_SERVER_HTTP_H

#include <stddef.h>
#include <stdio.h>

/* Prefix on every line the server logs, matching the binary name. */
#define HTTP_PROGNAME "tiny_http_server"

/* Value of the Server header. A constant rather than a version string: there
 * is nothing to version, and a constant keeps the response bytes golden. */
#define HTTP_SERVER_NAME "tiny_http_server"

/*
 * The only content type this server ever sends. Nothing is sniffed from a
 * path or an extension, because --file means "serve this HTML instead of the
 * built-in page" rather than "serve a file tree" - see server_load_page.
 */
#define HTTP_CONTENT_TYPE "text/html; charset=utf-8"

/*
 * Largest request header block accepted, in bytes. A request that reaches this
 * without a terminator gets a 431 rather than being truncated: the alternative
 * is parsing the first 8 KiB of an unbounded header block and answering as if
 * the rest had not been sent.
 */
#define HTTP_REQUEST_MAX 8192

/*
 * Size of the scratch buffer an error page is built in. Must be a #define
 * rather than a const int so callers can declare `char scratch[
 * HTTP_ERROR_PAGE_MAX]`, which C requires an integer constant expression for.
 * The longest page (431, whose reason phrase is 30 characters) is 185 bytes.
 */
#define HTTP_ERROR_PAGE_MAX 256

/*
 * Longest request line written to the log, after sanitizing. A request line
 * may be nearly HTTP_REQUEST_MAX bytes and is entirely the client's to choose,
 * so the log takes a bounded prefix of it and says so with an ellipsis.
 */
#define HTTP_LOG_LINE_MAX 256

/*
 * Outcome of an HTTP or socket operation. A nonzero value names the stage that
 * failed. For the stages backed by a libc call (HTTP_ERR_READ,
 * HTTP_ERR_WRITE, HTTP_ERR_SOCKET, HTTP_ERR_BIND, HTTP_ERR_LISTEN,
 * HTTP_ERR_ACCEPT, HTTP_ERR_OPEN) the failing call's errno is left in place, so
 * the caller may pair the result with strerror(errno). The remaining stages
 * carry no errno.
 *
 * HTTP_ERR_ACCEPT holds that contract more tightly than the rest, because
 * server_run decides whether to end the server by looking at errno: it is
 * returned from exactly one place, the statement after the accept that failed.
 * Anything that logs or cleans up before returning it - and every other result
 * here does - would hand the caller some later call's errno to judge, since
 * even a successful fprintf may set one.
 *
 * A request the server answers with a 4xx or 5xx is not one of these: the
 * server did its job by answering. Those are an HttpResponse, and the
 * connection they arrived on is HTTP_OK.
 *
 * Only three of these are fatal to the server - HTTP_ERR_SOCKET,
 * HTTP_ERR_BIND, HTTP_ERR_LISTEN, plus a non-transient HTTP_ERR_ACCEPT and a
 * failed page load at startup. Everything a client does is a per-connection
 * event, because a client must not be able to end the server.
 */
typedef enum {
  HTTP_OK = 0,
  HTTP_ERR_CLOSED,    /* the client hung up without sending a request */
  HTTP_ERR_TIMEOUT,   /* nothing arrived before the receive timeout */
  HTTP_ERR_READ,      /* a read error occurred on the connection */
  HTTP_ERR_WRITE,     /* a write error occurred on the connection */
  HTTP_ERR_TOO_LARGE, /* the header block hit HTTP_REQUEST_MAX */
  HTTP_ERR_MALFORMED, /* the request line is not one this server can parse */
  HTTP_ERR_SOCKET,    /* socket() or setsockopt() failed */
  HTTP_ERR_BIND,      /* bind() failed */
  HTTP_ERR_LISTEN,    /* listen() or getsockname() failed */
  HTTP_ERR_ACCEPT,    /* accept() failed other than transiently */
  /*
   * An accepted connection could not be set up: its timeouts, its dup, or one
   * of its streams. Deliberately not HTTP_ERR_ACCEPT, which would let a
   * one-off ENOMEM on one connection end a server the listening socket is
   * still perfectly able to serve from. Reported where it happens, so it
   * carries no errno.
   */
  HTTP_ERR_CONNECTION,
  HTTP_ERR_OPEN,        /* the --file page could not be opened or read */
  HTTP_ERR_NOT_REGULAR, /* the --file page is not a regular file */
  HTTP_ERR_NOMEM,       /* out of memory */
} HttpResult;

/*
 * The methods routing distinguishes.
 *
 * HTTP_METHOD_OTHER is not a parse failure: "POST" is a perfectly well-formed
 * method and the request carrying it is a perfectly well-formed request. That
 * it is not one this server serves is routing's judgment to make, and it makes
 * it as a 405 - which is why parsing does not reject it here.
 */
typedef enum {
  HTTP_METHOD_GET,
  HTTP_METHOD_HEAD,
  HTTP_METHOD_OTHER,
} HttpMethod;

/*
 * A parsed request line.
 *
 * Every pointer points into the caller's buffer and none is NUL-terminated, so
 * the buffer must outlive the HttpRequest. Lengths are carried explicitly
 * rather than implied by a NUL, which is what lets a request containing a NUL
 * byte be seen and refused rather than silently truncated into a valid one.
 */
typedef struct {
  HttpMethod method;
  /* The request target exactly as it arrived. Not percent-decoded and not
   * normalized: nothing here ever reaches the filesystem, so there is no path
   * to traverse and no decoding to get wrong. */
  const char *target;
  size_t target_len;
  /* The target up to the first '?', which is what routing matches on. */
  const char *path;
  size_t path_len;
  int major;
  int minor;
  /* The whole request line, for the log. Sanitize it before writing it. */
  const char *line;
  size_t line_len;
} HttpRequest;

/*
 * The bytes served at the default path: either the built-in page or whatever
 * --file held at startup. Read-only and owned by the caller.
 */
typedef struct {
  const char *body;
  size_t len;
} HttpPage;

/*
 * A response ready to be written, as routing decided it.
 *
 * body points at bytes the caller owns - the page for a 200, or the caller's
 * scratch buffer for an error - and body_len counts them. allow is the value
 * of the Allow header, or NULL when the response carries none; only a 405 does.
 */
typedef struct {
  int status;
  const char *reason;
  const char *body;
  size_t body_len;
  const char *allow;
} HttpResponse;

/* Returns a short human-readable label for an HttpResult. */
const char *http_result_str(HttpResult r);

/*
 * Returns the reason phrase for a status code this server can send, or
 * "Unknown" for any other. The phrase is part of the response bytes and part
 * of every error page's title, so it lives in one place.
 */
const char *http_status_reason(int status);

/* Returns the page compiled into the binary, served when there is no --file. */
HttpPage http_builtin_page(void);

/*
 * http_sanitize - renders untrusted bytes safe to write to a log.
 *
 * The request line is a client's bytes going to somebody's terminal. Written
 * raw, a line containing "\x1b[2J" clears the screen of whoever is watching the
 * server and one containing "\n" forges a second log line. Every byte outside
 * printable ASCII therefore becomes '?', including the escape itself - the
 * bracket and the digits that follow it are harmless once the ESC is gone.
 *
 * Input:  dst, dst_size - destination and its size, including room for the
 *         NUL. Must be at least 1.
 *         src, src_len - the bytes to render, which may contain NULs.
 *
 * Output: Returns the number of bytes written, not counting the NUL. Input too
 *         long for dst is truncated and ends with "..." so the log says that it
 *         was, rather than silently showing a prefix as if it were the whole
 *         line.
 */
size_t http_sanitize(char *dst, size_t dst_size, const char *src,
                     size_t src_len);

/*
 * http_error_response - builds the response for a status with no page behind
 * it.
 *
 * Pure: the body is rendered into the caller's scratch buffer rather than
 * allocated, so routing cannot fail and needs no out-of-memory path. An error
 * carries a real HTML body rather than none, because an empty 404 renders as a
 * blank page - or gets replaced by the browser's own error page, which looks
 * exactly like a failure to connect - and shows nothing at all under curl.
 *
 * Input:  status - one of 400, 404, 405, 431, 505. Any other value is rendered
 *         with the reason "Unknown", which no caller should reach.
 *         scratch, scratch_size - where the body is built. Pass
 *         HTTP_ERROR_PAGE_MAX bytes; a smaller buffer yields a shorter body
 *         rather than a truncated one, since the length comes from what was
 *         written.
 *
 * Output: An HttpResponse whose body points into scratch, which must therefore
 *         outlive it. A 405 carries "GET, HEAD" in allow; nothing else does.
 */
HttpResponse http_error_response(int status, char *scratch,
                                 size_t scratch_size);

/*
 * http_parse_request - reads the request line out of a header block.
 *
 * Pure. Only the request line is looked at; the header lines after it are
 * ignored entirely, which is why there is no Host check (see README) and why a
 * request body is never read.
 *
 * The line must be exactly three space-separated fields - method, target,
 * version - and must be terminated. Two fields is HTTP/0.9, which this server
 * does not speak; four means a target containing a space, which must be
 * percent-encoded. The version token must match HTTP/<digit>.<digit> exactly,
 * so "HTTP/1" and "http/1.1" are malformed rather than unsupported - whether a
 * version is one we speak is routing's judgment, and this cannot even tell what
 * version was meant.
 *
 * One leading empty line is skipped, which RFC 7230 3.5 recommends: a client
 * that ends its previous request with an extra CRLF is common enough that
 * rejecting it is worse than allowing it.
 *
 * Input:  buf, len - the header block as http_read_request returned it.
 *
 * Output: HTTP_OK with *out filled in, or HTTP_ERR_MALFORMED, which the caller
 *         answers with a 400. A NUL anywhere in the request line is malformed:
 *         the rest of the program treats the line as text, and the alternative
 *         is letting "GET / HTTP/1.1\0junk" look like an ordinary request.
 */
HttpResult http_parse_request(const char *buf, size_t len, HttpRequest *out);

/*
 * http_route - decides what to answer a well-formed request with.
 *
 * Pure, and infallible: there is no 500 in this server because nothing routing
 * does can fail. The page was read at startup, so a request never touches the
 * filesystem, and the error bodies are built in the caller's scratch.
 *
 * The order of the checks is the contract, not an implementation detail:
 * version, then method, then path. A version we do not speak outranks a method
 * we do not serve because a method belongs to a protocol - which is what makes
 * an HTTP/2 preface ("PRI * HTTP/2.0") a 505 and not a 405.
 *
 * Input:  req - a request http_parse_request accepted.
 *         page - the bytes to serve at a known path.
 *         scratch, scratch_size - as http_error_response takes them.
 *
 * Output: 200 for GET or HEAD of "/" or "/index.html"; 505 for a major version
 *         other than 1; 405 with an Allow for any other method; 404 otherwise.
 *         A HEAD is routed exactly like a GET, body and all - suppressing the
 *         body is http_write_response's job, because a HEAD must report the
 *         Content-Length the GET would have had.
 */
HttpResponse http_route(const HttpRequest *req, const HttpPage *page,
                        char *scratch, size_t scratch_size);

/*
 * http_read_request - reads one request header block off a stream.
 *
 * Stops at the blank line that ends the header block, and never at end of
 * input: a client holds the connection open after sending, so anything that
 * reads to EOF - fread of a large count, getdelim - blocks until the timeout.
 * The terminator is taken as the last three bytes being "\n\r\n" or the last
 * two being "\n\n", which accepts all four spellings of a blank line while not
 * firing on the CRLF that ends the request line itself. Lone LFs are not a
 * hypothetical: nc, telnet, and hand-written scripts all send them.
 *
 * Bytes past the terminator are left on the stream unread. Those are a request
 * body, which this server never reads - see server_accept_once for what
 * happens to them.
 *
 * Input:  in - the connection, or an fmemopen stream in tests.
 *         buf, cap - where the block is stored; cap is the byte limit.
 *         out_len - receives the number of bytes stored, terminator included.
 *         Set even when the result is not HTTP_OK, so a caller can tell a
 *         client that sent nothing from one that sent half a request.
 *
 * Output: HTTP_OK, or HTTP_ERR_CLOSED (end of input, whether at the first byte
 *         or partway through - the first is what a browser's speculative
 *         connection looks like and is entirely ordinary), HTTP_ERR_TIMEOUT,
 *         HTTP_ERR_READ, or HTTP_ERR_TOO_LARGE. Only the last of those gets a
 *         response; there is nobody left to answer for the others.
 */
HttpResult http_read_request(FILE *in, char *buf, size_t cap, size_t *out_len);

/*
 * http_write_response - writes a response's bytes.
 *
 * The header order is fixed - status line, Server, Content-Type,
 * Content-Length, Connection, then Allow when there is one - so the bytes are
 * golden and a test can assert on all of them at once.
 *
 * Content-Length is not optional. Letting the connection close delimit the body
 * (which HTTP/1.0 allowed) makes a truncated response byte-identical to a
 * complete one, so a client cannot tell the page from half the page and a
 * crash. Connection: close is not optional either: this server speaks HTTP/1.1,
 * where persistent connections are the default, and a browser that believed
 * that would hold the socket open waiting for a second response - stalling
 * every other client, since connections are served one at a time.
 *
 * Input:  out - the connection, or an fmemopen stream in tests.
 *         resp - what http_route or http_error_response decided.
 *         suppress_body - nonzero for a HEAD. The headers, Content-Length
 *         included, are byte-identical to the GET's; only the body is withheld.
 *         Setting body_len to 0 for a HEAD instead is the obvious shortcut and
 *         is wrong: reporting the length the GET would have had is the entire
 *         reason the method exists.
 *
 * Output: HTTP_OK, or HTTP_ERR_WRITE with errno as the failing call left it.
 */
HttpResult http_write_response(FILE *out, const HttpResponse *resp,
                               int suppress_body);

/*
 * http_serve_connection - the whole transaction for one connection.
 *
 * Reads the header block, parses it, routes it, writes the response, and logs
 * what happened. Takes streams rather than a file descriptor so the entire
 * transaction is testable through fmemopen, with no socket anywhere - which is
 * the reason the write path goes through FILE * at all, and therefore the
 * reason SIGPIPE has to be ignored process-wide in main rather than handled
 * with MSG_NOSIGNAL here.
 *
 * Writes to log are best effort and unchecked. The log is stderr, and a server
 * that exits because somebody closed its stderr is worse than one that keeps
 * answering requests nobody is recording.
 *
 * Input:  in, out - the two ends of the connection. Separate streams, because
 *         a socket cannot be one bidirectional FILE * - C requires a
 *         positioning call between a read and a following write, and a socket
 *         has none.
 *         log - where the request and response lines go.
 *         page - the bytes to serve at a known path.
 *         left_unread - set to nonzero when the server stopped reading with
 *         the request still arriving, which is the 431 path and only that one.
 *         The caller needs it because the result cannot say so: a 431 is a
 *         response, so that connection is HTTP_OK like any other. May be NULL,
 *         which the streams-only tests pass.
 *
 * Output: HTTP_OK when a response was written, whatever its status - a 404 is
 *         the server working. Otherwise the read or write failure, none of
 *         which ends the server.
 */
HttpResult http_serve_connection(FILE *in, FILE *out, FILE *log,
                                 const HttpPage *page, int *left_unread);

#endif
