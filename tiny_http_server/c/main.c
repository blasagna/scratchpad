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
  printf("      --once        serve one request, then exit\n");
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

/* Parses value as a TCP port into *out; returns 0, or -1 after printing an
 * error. Base 10 is explicit, so "08080" is not octal, and privileged ports are
 * accepted here and fail later at bind, which is the kernel's call. */
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

/* Checks that value is a dotted quad. Names are not resolved: DNS is a blocking
 * lookup returning a list of candidates, for a program that binds one socket.
 * inet_pton also turns down "127.1" and "0177.0.0.1". */
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
   * mistake worth naming. */
  if (optind < argc) {
    fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
    print_usage_error();
    return 2;
  }

  /* Without this the server vanishes with no message the first time somebody
   * navigates away mid-response. A disposition and not MSG_NOSIGNAL, because
   * responses go out through a FILE *, which takes no flag. See README. */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fprintf(stderr, "%s: cannot ignore SIGPIPE: %s\n", HTTP_PROGNAME,
            strerror(errno));
    return 1;
  }

  /* The page is read once, before the socket exists, so an unreadable file is a
   * startup failure rather than a 500 later - and routing stays pure, which is
   * why this server has no 500 at all. */
  if (page_path != NULL) {
    HttpResult loaded =
        server_load_page(page_path, SERVER_MAX_PAGE_BYTES, &opts.page);
    if (loaded != HTTP_OK) {
      report_page_error(loaded, page_path);
      return 1;
    }
  }

  HttpResult result = server_run(&opts, stderr);

  /* free can clobber errno, and the report below pairs the failed stage with
   * strerror(errno). */
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
