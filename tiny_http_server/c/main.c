#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "http.h"
#include "server.h"

static void print_help(void) {
  printf("usage: tiny_http_server [options]\n");
  printf("       tiny_http_server -h | --help\n");
  printf("\n");
  printf(
      "A very small HTTP server. Binds a socket, then repeats: accept one\n");
  printf("connection, read the request, answer it, close, accept the next.\n");
  printf("Open http://127.0.0.1:8080 in a browser to see the page.\n");
  printf("\n");
  printf(
      "GET and HEAD of '/' or '/index.html' return 200 with a hello world\n");
  printf("page. Any other path is 404, any other method is 405, a request\n");
  printf("line that cannot be parsed is 400, and a version other than\n");
  printf("HTTP/1.x is 505. Every event is logged to stderr.\n");
  printf("\n");
  printf("Options:\n");
  printf("  -p, --port <n>    port to listen on, 0 to let the kernel pick "
         "(default %d)\n",
         SERVER_DEFAULT_PORT);
  printf("      --host <addr> IPv4 address to bind (default %s)\n",
         SERVER_DEFAULT_HOST);
  printf("      --file <path> serve this file instead of the built-in page\n");
  printf("      --once        serve one connection, then exit\n");
  printf("  -h, --help        show this help\n");
  printf("\n");
  printf(
      "Connections are served one at a time, so this is a toy rather than\n");
  printf(
      "a web server: any one slow client stalls the next. That is why the\n");
  printf(
      "default binds loopback only -- pass --host 0.0.0.0 to expose it on\n");
  printf("every interface, deliberately.\n");
}

static void print_usage_error(void) {
  fprintf(stderr, "usage: tiny_http_server [options]\n");
  fprintf(stderr, "       tiny_http_server --help\n");
}

/*
 * Parses value as a TCP port. On success stores it in *out and returns 0. On
 * failure prints an error and returns -1.
 *
 * Base 10 is explicit, so "08080" is eight thousand and eighty rather than an
 * octal anything. strtol saturates at LONG_MAX on overflow, which the range
 * check already rejects, so there is no separate ERANGE branch to write. Ports
 * 1-1023 are accepted here and fail later at bind with EACCES for a non-root
 * user: naming what is a port is this function's job, and deciding who may have
 * one is the kernel's.
 */
static int parse_port(const char *value, int *out) {
  char *endp;
  long n = strtol(value, &endp, 10);
  if (endp == value || *endp != '\0' || n < 0 || n > 65535) {
    fprintf(stderr,
            "error: invalid value '%s' for --port (expected 0 to 65535)\n",
            value);
    return -1;
  }
  *out = (int)n;
  return 0;
}

/*
 * Checks that value is a dotted quad. Names are not resolved: getaddrinfo
 * would bring DNS and a blocking network lookup into the startup of a program
 * that binds exactly one socket, and would hand back a list of candidates to
 * choose between. inet_pton also turns down "127.1" and "0177.0.0.1", which
 * the older inet_aton would have accepted as 127.0.0.1.
 */
static int check_host(const char *value) {
  struct in_addr addr;
  if (inet_pton(AF_INET, value, &addr) == 1)
    return 0;
  fprintf(stderr,
          "error: invalid value '%s' for --host (expected an IPv4 address "
          "such as 127.0.0.1)\n",
          value);
  return -1;
}

/* Reports a --file that could not be loaded, in the loader's own terms. */
static void report_page_error(HttpResult result, const char *path) {
  switch (result) {
  case HTTP_ERR_NOT_REGULAR:
    fprintf(stderr, "%s: %s: not a regular file\n", HTTP_PROGNAME, path);
    break;
  case HTTP_ERR_TOO_LARGE:
    fprintf(stderr, "%s: %s: larger than the %d byte limit\n", HTTP_PROGNAME,
            path, SERVER_MAX_PAGE_BYTES);
    break;
  case HTTP_ERR_NOMEM:
    fprintf(stderr, "%s: %s\n", HTTP_PROGNAME, http_result_str(result));
    break;
  default:
    fprintf(stderr, "%s: %s: %s\n", HTTP_PROGNAME, path, strerror(errno));
    break;
  }
}

int main(int argc, char *argv[]) {
  ServerOptions opts = server_options_default();
  const char *page_path = NULL;

  static struct option long_opts[] = {
      {"port", required_argument, NULL, 'p'},
      {"host", required_argument, NULL, 'H'},
      {"file", required_argument, NULL, 'F'},
      {"once", no_argument, NULL, 'O'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "hp:", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'p':
      if (parse_port(optarg, &opts.port) != 0)
        return 2;
      break;
    case 'H':
      if (check_host(optarg) != 0)
        return 2;
      opts.host = optarg;
      break;
    case 'F':
      page_path = optarg;
      break;
    case 'O':
      opts.serve_once = 1;
      break;
    case 'h':
      print_help();
      return 0;
    default:
      print_usage_error();
      return 2;
    }
  }

  /* Requests arrive over the socket, never from argv, so a stray operand is a
   * mistake worth naming rather than something to ignore. */
  if (optind < argc) {
    fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
    print_usage_error();
    return 2;
  }

  /*
   * Writing to a socket whose peer has already gone raises SIGPIPE, whose
   * default action is to terminate the process with no message at all - so
   * without this the server vanishes the first time somebody navigates away
   * mid-response, and the symptom is "it just disappears sometimes". Ignoring
   * it turns the same event into an EPIPE the write path reports and the loop
   * survives.
   *
   * It is here rather than in the library because it is process-global state,
   * and the library takes streams its caller owns. It has to be a signal
   * disposition rather than MSG_NOSIGNAL on the send, because the responses go
   * out through a FILE * - which is what makes them testable with fmemopen, and
   * fwrite has nowhere to put a flag. (SO_NOSIGPIPE is a BSD socket option and
   * does not exist here.) signal rather than sigaction is enough for SIG_IGN,
   * which has no handler and so raises no SA_RESTART question.
   */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fprintf(stderr, "%s: cannot ignore SIGPIPE: %s\n", HTTP_PROGNAME,
            strerror(errno));
    return 1;
  }

  /*
   * The page is read once, here, before the socket exists. A file that cannot
   * be read is then a startup failure with a message, rather than a 500 that
   * turns up later depending on which path somebody visits - and routing stays
   * pure, with no I/O and no failure path, which is why this server has no 500
   * at all. The cost is that the page cannot change while the server runs.
   */
  if (page_path != NULL) {
    HttpResult loaded =
        server_load_page(page_path, SERVER_MAX_PAGE_BYTES, &opts.page);
    if (loaded != HTTP_OK) {
      report_page_error(loaded, page_path);
      return 1;
    }
  }

  HttpResult result = server_run(&opts, stderr);

  /* free can clobber errno - glibc's may madvise or munmap when it trims - and
   * the report below pairs the failed stage with strerror(errno). */
  int saved = errno;
  if (page_path != NULL)
    free((void *)opts.page.body);
  errno = saved;

  if (result != HTTP_OK) {
    fprintf(stderr, "%s: %s: %s\n", HTTP_PROGNAME, http_result_str(result),
            strerror(errno));
    return 1;
  }
  return 0;
}
