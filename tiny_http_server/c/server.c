#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * How many connections the kernel may queue behind the one being served.
 *
 * Not 1: a browser loading a single page opens a speculative connection and a
 * favicon connection alongside the page's, and this server holds each one for
 * a whole transaction, so a queue of one produces visible connection refusals
 * in the network panel. Not SOMAXCONN (4096 here) either: a queue that deep in
 * front of a server that drains it one at a time only converts a refusal, which
 * a browser retries immediately, into a timeout, which it does not. Sixteen
 * covers a browser's six-connections-per-host limit with room over.
 */
static const int kBacklog = 16;

/* How many seconds a connection may spend waiting for bytes by default. */
static const int kIoTimeoutSeconds = 5;

/*
 * The most a connection is drained of before its close. Unbounded is not an
 * option: this server handles one connection at a time, so a client that keeps
 * sending would hold it forever. The receive timeout bounds the wall clock and
 * this bounds the bytes.
 */
static const size_t kDrainMax = 64 * 1024;

/*
 * The same bound for the one connection that is owed a response it might not
 * get: an oversized header block, answered with a 431 that a close on unread
 * data would turn into an RST. A request that reaches this and keeps going has
 * stopped being a request, so the cap stays - it is the page cap, since a
 * header block a megabyte past the 8 KiB limit is already far past arguing
 * with.
 */
static const size_t kDrainOverflowMax = 1024 * 1024;

/* First allocation when reading a page, then doubled. */
static const size_t kPageChunk = 8192;

ServerOptions server_options_default(void) {
  ServerOptions opts;
  opts.host = SERVER_DEFAULT_HOST;
  opts.port = SERVER_DEFAULT_PORT;
  opts.serve_once = 0;
  opts.io_timeout_seconds = kIoTimeoutSeconds;
  opts.page = http_builtin_page();
  return opts;
}

/* Closes fd without disturbing the errno a caller is about to report. close()
 * can fail, and would otherwise overwrite it. */
static void close_quietly(int fd) {
  int saved = errno;
  close(fd);
  errno = saved;
}

HttpResult server_load_page(const char *path, size_t max_bytes, HttpPage *out) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return HTTP_ERR_OPEN;

  /*
   * fopen on a directory succeeds on Linux and only fails later inside fread,
   * with EISDIR, which would be reported as "cannot read the page file" for
   * what is really "that is a directory". Asking fstat first also turns down
   * /dev/zero and FIFOs before they churn through the whole cap.
   */
  struct stat st;
  if (fstat(fileno(f), &st) != 0) {
    int saved = errno;
    fclose(f);
    errno = saved;
    return HTTP_ERR_OPEN;
  }
  if (!S_ISREG(st.st_mode)) {
    fclose(f);
    return HTTP_ERR_NOT_REGULAR;
  }

  /*
   * Read in chunks rather than trusting a size. fseek/ftell lies for special
   * files, and even for a regular one the file may change between the stat and
   * the read - in which case a Content-Length taken from the stat would
   * describe a page that is no longer the one being served. Room for one byte
   * past the cap is what makes "too large" detectable rather than a silent
   * truncation.
   */
  char *buf = NULL;
  size_t cap = 0;
  size_t len = 0;
  HttpResult result = HTTP_OK;
  for (;;) {
    if (len == cap) {
      size_t next = cap == 0 ? kPageChunk : cap * 2;
      if (next > max_bytes + 1)
        next = max_bytes + 1;
      if (next == cap) {
        /* The buffer already holds max_bytes + 1, so the file is over the cap
         * and there is nothing left to grow into. */
        result = HTTP_ERR_TOO_LARGE;
        break;
      }
      char *grown = realloc(buf, next);
      if (grown == NULL) {
        result = HTTP_ERR_NOMEM;
        break;
      }
      buf = grown;
      cap = next;
    }

    size_t n = fread(buf + len, 1, cap - len, f);
    len += n;
    if (n == 0) {
      if (ferror(f))
        result = HTTP_ERR_OPEN;
      break;
    }
    if (len > max_bytes) {
      result = HTTP_ERR_TOO_LARGE;
      break;
    }
  }

  if (result != HTTP_OK) {
    int saved = errno;
    free(buf);
    fclose(f);
    errno = saved;
    return result;
  }
  fclose(f);

  /* An empty file is a page of zero bytes, not an error: it is served with
   * Content-Length: 0 and renders as a blank page, which is what was asked
   * for. buf is not NULL in that case - the loop allocates before its first
   * read, so an empty file leaves one chunk holding nothing, which the caller
   * still frees - and nothing dereferences it either way, since
   * http_write_response skips a zero-length body. */
  out->body = buf;
  out->len = len;
  return HTTP_OK;
}

HttpResult server_listen(const ServerOptions *opts, ServerListener *out) {
  out->fd = -1;
  out->host = opts->host;
  out->port = opts->port;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)opts->port);
  /* A dotted quad, never a name. getaddrinfo would bring in DNS - a blocking
   * network lookup during startup, returning a list of candidates for a
   * program that binds exactly one socket. main validates this too; reaching
   * here with a bad host is a caller's bug rather than a user's. */
  if (inet_pton(AF_INET, opts->host, &addr.sin_addr) != 1) {
    errno = EINVAL;
    return HTTP_ERR_BIND;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return HTTP_ERR_SOCKET;

  /* Before the bind, which is the only place it does anything. */
  int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0) {
    close_quietly(fd);
    return HTTP_ERR_SOCKET;
  }

  if (bind(fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
    close_quietly(fd);
    return HTTP_ERR_BIND;
  }
  if (listen(fd, kBacklog) != 0) {
    close_quietly(fd);
    return HTTP_ERR_LISTEN;
  }

  /* Unconditionally, not only when port 0 asked the kernel to choose: one code
   * path, and the port that gets logged is always the one really in use. */
  struct sockaddr_in bound;
  socklen_t bound_len = sizeof bound;
  if (getsockname(fd, (struct sockaddr *)&bound, &bound_len) != 0) {
    close_quietly(fd);
    return HTTP_ERR_LISTEN;
  }

  out->fd = fd;
  out->port = ntohs(bound.sin_port);
  return HTTP_OK;
}

void server_close(ServerListener *l) {
  if (l->fd >= 0)
    close_quietly(l->fd);
  l->fd = -1;
}

/*
 * Reads and discards what the client sent and we never asked for, so the close
 * below sends a FIN rather than an RST. Reads the descriptor directly instead
 * of the stream: bytes sitting in the stream's buffer are already off the
 * socket, and fclose discards them for free.
 *
 * flags is MSG_DONTWAIT for every ordinary connection, so this takes what has
 * already arrived and never waits for more. A blocking drain everywhere would
 * sit here until the peer closed, which costs the accept loop a round trip on
 * every connection and lets a client that reads its response but keeps the
 * socket open stall the whole server for the timeout. What is left is a narrow
 * race - a body that arrives between the drain and the close still provokes an
 * RST - and that is the right trade for a server that handles one connection at
 * a time.
 *
 * The exception, and the only caller that passes 0, is a header block over
 * HTTP_REQUEST_MAX. There the server stopped reading at 8 KiB with the rest of
 * a much larger request still in flight, so what has already arrived is nowhere
 * near all of it - and unlike a body nobody read, that client was answered,
 * with a 431 the RST would throw away. SO_RCVTIMEO bounds the wait exactly as
 * it bounds every other read on this socket.
 */
static void drain(int fd, int flags, size_t max) {
  char scrap[4096];
  size_t total = 0;
  while (total < max) {
    ssize_t n = recv(fd, scrap, sizeof scrap, flags);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    total += (size_t)n;
  }
}

/* Applies the read and write timeouts to an accepted connection. */
static int set_timeouts(int fd, int seconds) {
  struct timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  /* The receive timeout is what keeps a browser's speculative connection -
   * connected, then silent - from wedging a server that serves one at a time.
   * The send timeout covers the mirror image, which only bites with a large
   * --file: a client that stops reading blocks the write once the socket's
   * send buffer fills. */
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

HttpResult server_accept_once(const ServerListener *l,
                              const ServerOptions *opts, FILE *log) {
  struct sockaddr_in peer;
  socklen_t peer_len = sizeof peer;
  int fd;
  /* Nothing here installs a signal handler, so nothing should interrupt this -
   * but a profiler's SIGPROF would, and an unexplained server exit is a bad way
   * to find that out. */
  do {
    fd = accept(l->fd, (struct sockaddr *)&peer, &peer_len);
  } while (fd < 0 && errno == EINTR);
  /* Nothing between the failed accept and this return, deliberately: server_run
   * ends the server or not by reading errno, and even a log line here could
   * leave it holding fprintf's errno instead of accept's - turning a fatal
   * EMFILE into a "transient" ECONNABORTED and back into the 100% CPU spin the
   * classification exists to prevent. This is the only HTTP_ERR_ACCEPT. */
  if (fd < 0)
    return HTTP_ERR_ACCEPT;

  /* inet_ntop rather than getnameinfo: without NI_NUMERICHOST that one does a
   * reverse DNS lookup, which on a loop that serves one connection at a time
   * blocks every other client on a network round trip, and can hang for
   * seconds against a broken resolver. This cannot fail for an AF_INET address
   * and cannot block. */
  char peer_host[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &peer.sin_addr, peer_host, sizeof peer_host) == NULL)
    snprintf(peer_host, sizeof peer_host, "?");
  fprintf(log, "%s: connection from %s:%d\n", HTTP_PROGNAME, peer_host,
          ntohs(peer.sin_port));

  /*
   * Everything from here down is this connection's own failure, not the
   * listening socket's, so it comes back as HTTP_ERR_CONNECTION and the loop
   * moves on. Returning HTTP_ERR_ACCEPT would put a one-off ENOMEM on one
   * connection in front of server_run's fatal-or-not decision, which is a
   * client's request ending the server.
   */
  if (!set_timeouts(fd, opts->io_timeout_seconds)) {
    fprintf(log, "%s: cannot set the connection timeouts: %s\n", HTTP_PROGNAME,
            strerror(errno));
    close_quietly(fd);
    fprintf(log, "%s: connection closed\n", HTTP_PROGNAME);
    return HTTP_ERR_CONNECTION;
  }

  /*
   * Two descriptors for one socket, because a socket cannot be one
   * bidirectional stream: C requires a positioning call between a read and a
   * following write, and fseek on a socket returns ESPIPE.
   *
   * fdopen takes ownership only on success, which is what the cleanup below
   * turns on. Getting it backwards leaks a descriptor - the server then dies of
   * EMFILE after a few thousand requests - or double-closes one, which once
   * other descriptors are in play silently closes something unrelated.
   */
  int fd2 = dup(fd);
  if (fd2 < 0) {
    /* Snapshotted before the log write, not after: fprintf is allowed to set
     * errno even when it succeeds, and does when stderr is closed or full. The
     * same order applies everywhere below. */
    int saved = errno;
    fprintf(log, "%s: cannot duplicate the connection: %s\n", HTTP_PROGNAME,
            strerror(saved));
    close_quietly(fd);
    fprintf(log, "%s: connection closed\n", HTTP_PROGNAME);
    return HTTP_ERR_CONNECTION;
  }

  FILE *in = fdopen(fd, "r");
  if (in == NULL) {
    int saved = errno;
    fprintf(log, "%s: cannot open the connection for reading: %s\n",
            HTTP_PROGNAME, strerror(saved));
    close_quietly(fd);
    close_quietly(fd2);
    fprintf(log, "%s: connection closed\n", HTTP_PROGNAME);
    return HTTP_ERR_CONNECTION;
  }
  FILE *out = fdopen(fd2, "w");
  if (out == NULL) {
    int saved = errno;
    fprintf(log, "%s: cannot open the connection for writing: %s\n",
            HTTP_PROGNAME, strerror(saved));
    /* in owns fd now, so closing fd here as well would be a double close. */
    fclose(in);
    close(fd2);
    fprintf(log, "%s: connection closed\n", HTTP_PROGNAME);
    return HTTP_ERR_CONNECTION;
  }

  int left_unread = 0;
  HttpResult result =
      http_serve_connection(in, out, log, &opts->page, &left_unread);

  /*
   * A lingering close, not a bare one. Linux sends an RST rather than a FIN
   * when a socket is closed with unread inbound data, and a peer is allowed to
   * throw away data it already received when it gets an RST - so `curl -d x`,
   * whose request body this server deliberately never reads, would report a
   * connection reset instead of showing the 405 that really was sent.
   *
   * An oversized header block is the one case where what is left unread is not
   * a body the client already finished sending but the remainder of a request
   * still on its way, so that one waits for it; see drain.
   */
  int flags = MSG_DONTWAIT;
  size_t drain_max = kDrainMax;
  if (left_unread) {
    flags = 0;
    drain_max = kDrainOverflowMax;
  }
  int saved = errno;
  shutdown(fd, SHUT_WR);
  drain(fd, flags, drain_max);
  fclose(out);
  fclose(in);
  errno = saved;

  fprintf(log, "%s: connection closed\n", HTTP_PROGNAME);
  return result;
}

/*
 * Reports whether an accept failure is the client's doing rather than the
 * server's. Both of these mean the peer vanished between the handshake and the
 * accept, which is ordinary. EINTR is not here because it is already retried.
 */
static int accept_is_transient(int err) {
  return err == ECONNABORTED || err == EPROTO;
}

/*
 * Reports whether a connection got far enough to be answered, which is what
 * --once is waiting for.
 *
 * Breaking on any connection at all is the obvious rule and it makes --once
 * useless against a browser: Chrome and Firefox preconnect, so the first
 * connection is a silent one that times out after five seconds, and the server
 * would exit having served nothing while the request the user actually made
 * gets refused. HTTP_ERR_WRITE counts - the response was written, the client
 * left before taking it - and so does the 431 path, which is HTTP_OK.
 */
static int connection_was_answered(HttpResult r) {
  return r == HTTP_OK || r == HTTP_ERR_WRITE;
}

HttpResult server_run(const ServerOptions *opts, FILE *log) {
  ServerListener l;
  HttpResult listened = server_listen(opts, &l);
  if (listened != HTTP_OK)
    return listened;
  fprintf(log, "%s: listening on %s:%d\n", HTTP_PROGNAME, l.host, l.port);

  HttpResult result = HTTP_OK;
  for (;;) {
    HttpResult served = server_accept_once(&l, opts, log);

    if (served == HTTP_ERR_ACCEPT) {
      /*
       * Everything a client does is a per-connection event, but a listening
       * socket that cannot produce connections is not. Logging and continuing
       * on every accept failure was the alternative and was rejected: EMFILE
       * or EBADF would then spin at 100% CPU writing the same line forever,
       * which is far worse than exiting with it.
       */
      if (!accept_is_transient(errno)) {
        result = served;
        break;
      }
      fprintf(log, "%s: %s: %s\n", HTTP_PROGNAME, http_result_str(served),
              strerror(errno));
      continue;
    }

    if (opts->serve_once && connection_was_answered(served))
      break;
  }

  int saved = errno;
  server_close(&l);
  errno = saved;
  return result;
}
