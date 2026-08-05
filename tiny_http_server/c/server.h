#ifndef TINY_HTTP_SERVER_SERVER_H
#define TINY_HTTP_SERVER_SERVER_H

#include <stddef.h>
#include <stdio.h>

#include "http.h"

/* Where the server binds when --host is not given. The exercise says to open a
 * browser at localhost, which loopback satisfies; see README for why the
 * default is not 0.0.0.0. */
#define SERVER_DEFAULT_HOST "127.0.0.1"

/* The port the exercise asks for. */
#define SERVER_DEFAULT_PORT 8080

/*
 * Largest --file page loaded, in bytes. The page lives in memory for the
 * process's whole life and Content-Length is derived from it, so an uncapped
 * read is a memory bug wearing an option's clothes. Exceeding this is a
 * startup error rather than a truncation - half a page served as a whole one
 * is worse than a refusal that says why.
 */
#define SERVER_MAX_PAGE_BYTES (1024 * 1024)

/*
 * A bound, listening socket.
 *
 * port is what getsockname reported, which is the port actually in use rather
 * than the one asked for - the two differ whenever --port 0 asked the kernel
 * to choose. host is the caller's string, not a copy.
 */
typedef struct {
  int fd;
  const char *host;
  int port;
} ServerListener;

/*
 * How the server behaves.
 *
 * The flags are int, not bool, because this header is compiled in two
 * dialects. The C here is C17 (.bazelrc sets -std=c++20 as a cxxopt, which does
 * not reach C), where bool needs <stdbool.h> and is _Bool; test_server.c
 * compiles the same header as C++ (-x c++), where bool is a distinct builtin.
 * The extern "C" wrapper there fixes linkage, not layout, so the two would
 * agree only by ABI accident. int is the same type in both.
 */
typedef struct {
  const char *host;
  int port;
  /* Serve exactly one connection, then return. What an end-to-end check uses
   * instead of backgrounding the server and killing it by PID. */
  int serve_once;
  /* Seconds a connection may spend waiting for bytes, in either direction.
   * Not a command-line option: its most obvious setting, 0, means "no timeout"
   * and puts back the wedged-loop bug the timeout exists to prevent. It lives
   * here so the tests can shorten it. */
  int io_timeout_seconds;
  /* What is served at a known path: the built-in page, or --file's bytes. */
  HttpPage page;
} ServerOptions;

/* Returns a ServerOptions holding the defaults, with the built-in page. */
ServerOptions server_options_default(void);

/*
 * server_load_page - reads a --file page into memory, once, at startup.
 *
 * Called before the socket exists, so an unreadable page is a startup failure
 * with a message rather than a 500 that shows up later depending on which path
 * somebody visits. That is also what keeps http_route pure and infallible:
 * there is no 500 anywhere in this server, because nothing routing does can
 * fail. The cost is that the page cannot change while the server runs; restart
 * to change it.
 *
 * Input:  path - the file to read.
 *         max_bytes - the byte limit; a larger file is refused, not truncated.
 *         out - receives the bytes, which the caller frees with free((void *)
 *         out->body). Untouched unless the result is HTTP_OK.
 *
 * Output: HTTP_OK, or HTTP_ERR_OPEN with errno intact, HTTP_ERR_NOT_REGULAR,
 *         or HTTP_ERR_NOMEM. An empty file is not an error: it is a page of
 *         zero bytes and is served as one.
 */
HttpResult server_load_page(const char *path, size_t max_bytes, HttpPage *out);

/*
 * server_listen - creates the listening socket.
 *
 * socket, SO_REUSEADDR, bind, listen, getsockname, in that order. SO_REUSEADDR
 * has to come before the bind, and without it restarting inside about a minute
 * fails EADDRINUSE on the TIME_WAIT remnants of the connections just served -
 * which is every Ctrl-C and rerun. It is not SO_REUSEPORT: a second live
 * listener is still refused, so EADDRINUSE keeps meaning "the server is already
 * running" instead of silently splitting traffic between two servers.
 *
 * getsockname runs unconditionally rather than only for port 0, so there is one
 * code path and the logged port is always the one really in use.
 *
 * Input:  opts - host and port. The host must be a dotted quad; it is parsed
 *         here with inet_pton and nothing resolves names.
 *         out - receives the listener. Close it with server_close.
 *
 * Output: HTTP_OK, or HTTP_ERR_SOCKET, HTTP_ERR_BIND, or HTTP_ERR_LISTEN, each
 *         with errno as the failing call left it. All three are fatal: there is
 *         no server without them.
 */
HttpResult server_listen(const ServerOptions *opts, ServerListener *out);

/* Closes a listener. Safe on one server_listen already failed on. */
void server_close(ServerListener *l);

/*
 * server_accept_once - accepts one connection, serves it, and closes it.
 *
 * The accepted descriptor is dup'd and wrapped in two streams, "r" on one and
 * "w" on the other. One bidirectional FILE * is not an option: C requires a
 * positioning call between a read and a following write, and a socket has none
 * - fseek returns ESPIPE - so the transition is undefined and shows up as
 * dropped or duplicated response bytes.
 *
 * The connection is closed with a shutdown and a bounded drain rather than a
 * bare close. Linux sends RST instead of FIN when a socket still holds unread
 * inbound data, and a peer may discard data it already received when it gets
 * an RST - so `curl -d x` would report a connection reset instead of showing
 * the 405 that was really sent, since a request body is deliberately never
 * read.
 *
 * Input:  l - a listener from server_listen.
 *         opts - the timeout and the page.
 *         log - where connection and request events go.
 *
 * Output: HTTP_OK when a response was written, whatever its status. Everything
 *         a client can do comes back as a per-connection failure the caller
 *         logs and moves past; only HTTP_ERR_ACCEPT is worth ending the server
 *         over, and server_run decides which accept failures are.
 */
HttpResult server_accept_once(const ServerListener *l,
                              const ServerOptions *opts, FILE *log);

/*
 * server_run - binds, then serves connections until something fatal happens.
 *
 * One connection at a time: accept, read, respond, close, accept. Nothing here
 * forks and nothing threads, so there is no reaping and no shared state, and a
 * slow client stalls the next one - which is what the receive timeout bounds.
 *
 * A client cannot end the server. Every failure a peer can cause is logged
 * against its connection and the loop continues; only the listening socket's
 * own failures, and an accept error that is not transient, come back from here.
 * Logging and continuing on every accept error was the alternative and was
 * rejected: EMFILE would then spin at 100% CPU writing one line forever.
 *
 * Input:  opts - everything, including whether to stop after one connection.
 *         log - where every event goes.
 *
 * Output: HTTP_OK only when opts->serve_once served its connection; otherwise
 *         the fatal result, with errno as the failing call left it.
 */
HttpResult server_run(const ServerOptions *opts, FILE *log);

#endif
