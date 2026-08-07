#ifndef TINY_HTTP_SERVER_SERVER_H
#define TINY_HTTP_SERVER_SERVER_H

#include <stddef.h>
#include <stdio.h>

#include "http.h"

/* Where the server binds when --host is not given; see README for why the
 * default is not 0.0.0.0. */
#define SERVER_DEFAULT_HOST "127.0.0.1"

/* The port the exercise asks for. */
#define SERVER_DEFAULT_PORT 8080

/* Largest --file page loaded. The page lives in memory for the process's whole
 * life, so an uncapped read is a memory bug wearing an option's clothes.
 * Exceeding this is a startup error, not a truncation. */
#define SERVER_MAX_PAGE_BYTES (1024 * 1024)

/* A bound, listening socket. port is what getsockname reported, which differs
 * from the one asked for whenever --port 0 let the kernel choose. host is the
 * caller's string, not a copy. */
typedef struct {
  int fd;
  const char *host;
  int port;
} ServerListener;

/* How the server behaves. The flags are int, not bool, because this header is
 * compiled as C17 here and as C++ in test_server.c, where bool is a different
 * type; extern "C" fixes linkage, not layout. */
typedef struct {
  const char *host;
  int port;
  /* Serve exactly one request, then return. One request and not one
   * connection: a browser's silent preconnect is a connection, and stopping on
   * it exits having served nothing. */
  int serve_once;
  /* Seconds a connection may wait for bytes, either direction. Not an option:
   * its obvious setting, 0, means "no timeout" and puts back the wedged-loop
   * bug. It lives here so the tests can shorten it. */
  int io_timeout_seconds;
  /* What is served at a known path: the built-in page, or --file's bytes. */
  HttpPage page;
} ServerOptions;

/* Returns a ServerOptions holding the defaults, with the built-in page. */
ServerOptions server_options_default(void);

/* Reads a --file page into *out, before the socket exists, so an unreadable one
 * is a startup failure rather than a 500 later. Over max_bytes is refused, not
 * truncated; empty is a page of zero bytes. The caller frees out->body. */
HttpResult server_load_page(const char *path, size_t max_bytes, HttpPage *out);

/* Creates the listening socket: socket, SO_REUSEADDR, bind, listen,
 * getsockname, in that order and all three failures fatal. The host must be a
 * dotted quad, parsed with inet_pton; nothing resolves names. */
HttpResult server_listen(const ServerOptions *opts, ServerListener *out);

/* Closes a listener. Safe on one server_listen already failed on. */
void server_close(ServerListener *l);

/* Accepts one connection, serves it, and closes it with a shutdown plus a
 * bounded drain. Everything a client can do is a per-connection failure the
 * caller logs and moves past; only HTTP_ERR_ACCEPT can end the server. */
HttpResult server_accept_once(const ServerListener *l,
                              const ServerOptions *opts, FILE *log);

/* Binds, then serves one connection at a time until something fatal happens. A
 * client cannot end the server. Returns HTTP_OK only when opts->serve_once
 * answered a request; otherwise the fatal result, with errno intact. */
HttpResult server_run(const ServerOptions *opts, FILE *log);

#endif
