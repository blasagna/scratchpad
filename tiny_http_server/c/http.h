#ifndef TINY_HTTP_SERVER_HTTP_H
#define TINY_HTTP_SERVER_HTTP_H

#include <stddef.h>
#include <stdio.h>

/* Prefix on every line the server logs, matching the binary name. */
#define HTTP_PROGNAME "tiny_http_server"

/* Value of the Server header. A constant, so the response bytes stay golden. */
#define HTTP_SERVER_NAME "tiny_http_server"

/* The only content type this server ever sends; nothing is sniffed. */
#define HTTP_CONTENT_TYPE "text/html; charset=utf-8"

/* Largest request header block accepted. Reaching this without a terminator is
 * a 431, not a truncation. */
#define HTTP_REQUEST_MAX 8192

/* Scratch size an error page is built in. A #define so callers can size an
 * array with it; the longest page (431) is 185 bytes. */
#define HTTP_ERROR_PAGE_MAX 256

/* Longest request line written to the log, after sanitizing. */
#define HTTP_LOG_LINE_MAX 256

/* The stage an operation failed at. The libc-backed ones leave errno in place;
 * the rest carry none. A 4xx/5xx is not here - the server answered. See
 * README. */
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
  /* accept() failed other than transiently. Returned from the statement right
   * after the failing accept, with nothing in between: server_run judges
   * fatality off errno, and even a successful fprintf may set one. */
  HTTP_ERR_ACCEPT,
  /* An accepted connection could not be set up. Deliberately not
   * HTTP_ERR_ACCEPT: a one-off ENOMEM on one connection must not end a server
   * whose listening socket is fine. Carries no errno. */
  HTTP_ERR_CONNECTION,
  HTTP_ERR_OPEN,        /* the --file page could not be opened or read */
  HTTP_ERR_NOT_REGULAR, /* the --file page is not a regular file */
  HTTP_ERR_NOMEM,       /* out of memory */
} HttpResult;

/* The methods routing distinguishes. HTTP_METHOD_OTHER is not a parse failure:
 * "POST" is well-formed, and that it is not served is routing's judgment, made
 * as a 405. */
typedef enum {
  HTTP_METHOD_GET,
  HTTP_METHOD_HEAD,
  HTTP_METHOD_OTHER,
} HttpMethod;

/* A parsed request line. Every pointer points into the caller's buffer, which
 * must outlive this; explicit lengths are what let a request containing a NUL
 * be refused rather than silently truncated. */
typedef struct {
  HttpMethod method;
  /* The target exactly as it arrived: not percent-decoded and not normalized,
   * since nothing here reaches the filesystem. */
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

/* The bytes served at the default path: the built-in page, or whatever --file
 * held at startup. Read-only and owned by the caller. */
typedef struct {
  const char *body;
  size_t len;
} HttpPage;

/* A response ready to be written. body points at bytes the caller owns - the
 * page for a 200, the caller's scratch for an error - and allow is NULL unless
 * this is a 405. */
typedef struct {
  int status;
  const char *reason;
  const char *body;
  size_t body_len;
  const char *allow;
} HttpResponse;

/* Returns a short human-readable label for an HttpResult. */
const char *http_result_str(HttpResult r);

/* Returns the reason phrase for a status this server sends, else "Unknown". It
 * is part of the response bytes and of every error page's title. */
const char *http_status_reason(int status);

/* Returns the page compiled into the binary, served when there is no --file. */
HttpPage http_builtin_page(void);

/* Renders src into dst (size dst_size, NUL included) with every non-printable
 * byte as '?', so a log line cannot clear a terminal or forge a second line.
 * Returns the length written; too-long input is truncated with "...". */
size_t http_sanitize(char *dst, size_t dst_size, const char *src,
                     size_t src_len);

/* Builds the response for one of 400, 404, 405, 431, 505, rendering the body
 * into scratch - which must outlive it, and should be HTTP_ERROR_PAGE_MAX. Pure
 * and infallible, which keeps http_route the same. Only a 405 allows. */
HttpResponse http_error_response(int status, char *scratch,
                                 size_t scratch_size);

/* Parses the request line out of a header block, filling *out. Pure: only the
 * request line is looked at, so there is no Host check and no body. Returns
 * HTTP_ERR_MALFORMED for what the README's grammar refuses, NULs too. */
HttpResult http_parse_request(const char *buf, size_t len, HttpRequest *out);

/* Decides what to answer a well-formed request with, building error bodies in
 * scratch. Pure and infallible - there is no 500 here. The check order is the
 * contract: version, then method, then path. */
HttpResponse http_route(const HttpRequest *req, const HttpPage *page,
                        char *scratch, size_t scratch_size);

/* Reads one header block from in into buf (cap bytes), stopping at the blank
 * line and never at EOF, and stores the length in *out_len even on failure.
 * Bytes past the terminator stay on the stream; see server_accept_once. */
HttpResult http_read_request(FILE *in, char *buf, size_t cap, size_t *out_len);

/* Writes a response, header order fixed so the bytes are golden. suppress_body
 * is nonzero for a HEAD, which keeps the GET's headers, Content-Length
 * included. Returns HTTP_OK or HTTP_ERR_WRITE with errno intact. */
HttpResult http_write_response(FILE *out, const HttpResponse *resp,
                               int suppress_body);

/* The whole transaction for one connection: read, parse, route, write, log.
 * Streams and not a descriptor, so it is testable through fmemopen. Sets
 * *left_unread (may be NULL) on the 431 path, which the result cannot say. */
HttpResult http_serve_connection(FILE *in, FILE *out, FILE *log,
                                 const HttpPage *page, int *left_unread);

#endif
