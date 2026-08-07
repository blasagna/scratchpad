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

/* How many connections the kernel may queue behind the one being served. Not 1
 * (a browser opens several per page) and not SOMAXCONN (a deep queue in front
 * of a sequential server turns a retried refusal into a timeout). */
static const int kBacklog = 16;

/* How many seconds a connection may spend waiting for bytes by default. */
static const int kIoTimeoutSeconds = 5;

/* The most a connection is drained of before its close. Unbounded would let a
 * client that keeps sending hold a one-at-a-time server forever: the receive
 * timeout bounds the wall clock and this bounds the bytes. */
static const size_t kDrainMax = 64 * 1024;

/* The same bound for the one connection owed a response it might not get: an
 * oversized header block, answered with a 431 that a close on unread data would
 * turn into an RST. A megabyte past the 8 KiB limit is past arguing with. */
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

  /* fopen on a directory succeeds and only fails inside fread with EISDIR,
   * reported as "cannot read the page file". fstat first also turns down
   * /dev/zero and FIFOs before they churn through the whole cap. */
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

  /* Read in chunks rather than trusting a size: fseek/ftell lies for special
   * files, and a file may change between the stat and the read. Room for one
   * byte past the cap is what makes "too large" detectable. */
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
        /* The buffer already holds max_bytes + 1: the file is over the cap. */
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

  /* An empty file is a page of zero bytes, not an error. buf is still non-NULL
   * (the loop allocates before its first read) and the caller frees it, but
   * nothing dereferences it either way. */
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
  /* A dotted quad, never a name: getaddrinfo would put a blocking DNS lookup
   * in the startup of a program that binds exactly one socket. main validates
   * this too, so reaching here with a bad host is a caller's bug. */
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

  /* Unconditionally, not only for port 0: one code path, and the logged port
   * is always the one really in use. */
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

/* Reads and discards what the client sent and we never asked for, so the close
 * sends a FIN rather than an RST. flags is MSG_DONTWAIT everywhere but the 431
 * path, where the rest of the request is still in flight; see README. */
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
  /* The receive timeout keeps a browser's silent preconnect from wedging a
   * server that serves one at a time; the send timeout covers the mirror image,
   * a client that stops reading a large --file. */
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

HttpResult server_accept_once(const ServerListener *l,
                              const ServerOptions *opts, FILE *log) {
  struct sockaddr_in peer;
  socklen_t peer_len = sizeof peer;
  int fd;
  /* Nothing here installs a handler, so nothing should interrupt this - but a
   * profiler's SIGPROF would, and an unexplained server exit is a bad way to
   * find that out. */
  do {
    fd = accept(l->fd, (struct sockaddr *)&peer, &peer_len);
  } while (fd < 0 && errno == EINTR);
  /* Nothing between the failed accept and this return, deliberately: server_run
   * judges fatality off errno, and even a log line could leave it holding
   * fprintf's. This is the only HTTP_ERR_ACCEPT. */
  if (fd < 0)
    return HTTP_ERR_ACCEPT;

  /* inet_ntop rather than getnameinfo: without NI_NUMERICHOST that one does a
   * reverse DNS lookup, blocking every other client on a network round trip.
   * This cannot fail for an AF_INET address and cannot block. */
  char peer_host[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &peer.sin_addr, peer_host, sizeof peer_host) == NULL)
    snprintf(peer_host, sizeof peer_host, "?");
  fprintf(log, "%s: connection from %s:%d\n", HTTP_PROGNAME, peer_host,
          ntohs(peer.sin_port));

  /* Everything below is this connection's failure, not the listening socket's,
   * so it is HTTP_ERR_CONNECTION and the loop moves on. HTTP_ERR_ACCEPT would
   * let a one-off ENOMEM on one connection end the server. */
  if (!set_timeouts(fd, opts->io_timeout_seconds)) {
    fprintf(log, "%s: cannot set the connection timeouts: %s\n", HTTP_PROGNAME,
            strerror(errno));
    close_quietly(fd);
    fprintf(log, "%s: connection closed\n", HTTP_PROGNAME);
    return HTTP_ERR_CONNECTION;
  }

  /* Two descriptors for one socket: C requires a positioning call between a
   * read and a following write, and fseek on a socket returns ESPIPE. fdopen
   * takes ownership only on success, which the cleanup below turns on. */
  int fd2 = dup(fd);
  if (fd2 < 0) {
    /* Snapshotted before the log write: fprintf may set errno even when it
     * succeeds. The same order applies everywhere below. */
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

  /* A lingering close, not a bare one: Linux sends an RST when a socket closes
   * with unread inbound data, so `curl -d x` would see a reset instead of the
   * 405. The 431 case waits for the rest of the request; see drain. */
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

/* Reports whether an accept failure is the client's doing. Both mean the peer
 * vanished between the handshake and the accept, which is ordinary. EINTR is
 * not here because it is already retried. */
static int accept_is_transient(int err) {
  return err == ECONNABORTED || err == EPROTO;
}

/* Reports whether a connection got far enough to be answered, which is what
 * --once waits for. Breaking on any connection makes --once useless against a
 * browser's preconnect. HTTP_ERR_WRITE counts, and so does the 431. */
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
      /* A listening socket that cannot produce connections is not a
       * per-connection event. Logging and continuing on every accept failure
       * would spin at 100% CPU on EMFILE or EBADF. */
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
