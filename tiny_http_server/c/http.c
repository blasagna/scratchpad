#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

/* The page served when there is no --file. Compiled in, not a data dependency:
 * `bazel run` executes from the runfiles dir, so a relative path would resolve
 * differently per invocation. An array, so sizeof gives the length. */
static const char kIndexHtml[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><title>tiny_http_server</title></head>\n"
    "<body><h1>Hello, world!</h1><p>Served by tiny_http_server.</p></body>\n"
    "</html>\n";

/* The body of every error response. One template, so adding a status is a row
 * in http_status_reason and nothing else. */
static const char kErrorPageFmt[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
    "<body><h1>%d %s</h1></body>\n"
    "</html>\n";

/* The value of the Allow header on a 405, and the whole of what is allowed. */
static const char kAllowedMethods[] = "GET, HEAD";

/* The two paths the page is served at. */
static const char kRootPath[] = "/";
static const char kIndexPath[] = "/index.html";

/* What a truncated log line ends with, so a prefix is not shown as the whole.
 */
static const char kEllipsis[] = "...";

const char *http_result_str(HttpResult r) {
  switch (r) {
  case HTTP_OK:
    return "success";
  case HTTP_ERR_CLOSED:
    return "client closed the connection without sending a request";
  case HTTP_ERR_TIMEOUT:
    return "client sent nothing before the read timeout";
  case HTTP_ERR_READ:
    return "error reading the request";
  case HTTP_ERR_WRITE:
    return "error writing the response";
  case HTTP_ERR_TOO_LARGE:
    return "request header block is too large";
  case HTTP_ERR_MALFORMED:
    return "malformed request";
  case HTTP_ERR_SOCKET:
    return "cannot create the listening socket";
  case HTTP_ERR_BIND:
    return "cannot bind the listening socket";
  case HTTP_ERR_LISTEN:
    return "cannot listen on the socket";
  case HTTP_ERR_ACCEPT:
    return "cannot accept a connection";
  case HTTP_ERR_CONNECTION:
    return "cannot set up the accepted connection";
  case HTTP_ERR_OPEN:
    return "cannot read the page file";
  case HTTP_ERR_NOT_REGULAR:
    return "the page file is not a regular file";
  case HTTP_ERR_NOMEM:
    return "out of memory";
  }
  return "unknown error";
}

const char *http_status_reason(int status) {
  /* A switch over an int, so -Wswitch has nothing to check and the default is
   * what catches a status nobody sends. */
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 431:
    return "Request Header Fields Too Large";
  case 505:
    return "HTTP Version Not Supported";
  default:
    return "Unknown";
  }
}

HttpPage http_builtin_page(void) {
  HttpPage page;
  page.body = kIndexHtml;
  page.len = sizeof(kIndexHtml) - 1;
  return page;
}

size_t http_sanitize(char *dst, size_t dst_size, const char *src,
                     size_t src_len) {
  const size_t ellipsis_len = sizeof(kEllipsis) - 1;

  if (dst_size == 0)
    return 0;

  size_t room = dst_size - 1;
  size_t keep = src_len;
  size_t dots = 0;
  if (keep > room) {
    /* The ellipsis lives inside room, displacing the last bytes rather than
     * overrunning; too small even for it keeps whatever fits. */
    dots = room < ellipsis_len ? room : ellipsis_len;
    keep = room - dots;
  }

  size_t written = 0;
  for (size_t i = 0; i < keep; i++) {
    unsigned char c = (unsigned char)src[i];
    /* Printable ASCII passes, everything else becomes '?'. Dropping the ESC is
     * what defuses a sequence; what follows it is ordinary text. */
    dst[written++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
  }
  for (size_t i = 0; i < dots; i++)
    dst[written++] = kEllipsis[i];
  dst[written] = '\0';
  return written;
}

HttpResponse http_error_response(int status, char *scratch,
                                 size_t scratch_size) {
  HttpResponse resp;
  resp.status = status;
  resp.reason = http_status_reason(status);
  /* Required by the RFC on a 405, and what tells a PUT what would have worked.
   */
  resp.allow = status == 405 ? kAllowedMethods : NULL;
  resp.body = scratch;
  resp.body_len = 0;

  if (scratch_size == 0) {
    resp.body = NULL;
    return resp;
  }

  int n = snprintf(scratch, scratch_size, kErrorPageFmt, status, resp.reason,
                   status, resp.reason);
  /* Neither branch is reachable at HTTP_ERROR_PAGE_MAX, but a short body beats
   * a failure path http_route would have to carry - that is what keeps routing
   * infallible. */
  if (n < 0)
    resp.body_len = 0;
  else if ((size_t)n >= scratch_size)
    resp.body_len = scratch_size - 1;
  else
    resp.body_len = (size_t)n;
  return resp;
}

/* Length of the first line in buf, not counting its terminator. */
static size_t line_prefix_len(const char *buf, size_t len) {
  const char *nl = memchr(buf, '\n', len);
  size_t n = nl == NULL ? len : (size_t)(nl - buf);
  if (n > 0 && buf[n - 1] == '\r')
    n--;
  return n;
}

/* Steps past one leading empty line, per RFC 7230 3.5. Exactly one, so a block
 * of blank lines stays malformed. Shared by the parser and the log, so the log
 * cannot describe a different line than the one that was rejected. */
static void skip_one_blank_line(const char **buf, size_t *len) {
  const char *p = *buf;
  if (*len >= 2 && p[0] == '\r' && p[1] == '\n') {
    *buf = p + 2;
    *len -= 2;
  } else if (*len >= 1 && p[0] == '\n') {
    *buf = p + 1;
    *len -= 1;
  }
}

/* Reports whether the field is exactly HTTP/<digit>.<digit>. */
static int version_shape_ok(const char *field, size_t len) {
  return len == 8 && memcmp(field, "HTTP/", 5) == 0 &&
         isdigit((unsigned char)field[5]) && field[6] == '.' &&
         isdigit((unsigned char)field[7]);
}

HttpResult http_parse_request(const char *buf, size_t len, HttpRequest *out) {
  const char *p = buf;
  size_t remaining = len;

  skip_one_blank_line(&p, &remaining);

  const char *nl = memchr(p, '\n', remaining);
  /* No terminator means the request line never ended, and a target cut off
   * midway is a different target. */
  if (nl == NULL)
    return HTTP_ERR_MALFORMED;

  size_t line_len = (size_t)(nl - p);
  if (line_len > 0 && p[line_len - 1] == '\r')
    line_len--;

  /* A NUL is refused, not treated as a terminator: everything downstream reads
   * the line as text, so "GET / HTTP/1.1\0junk" would otherwise pass. */
  if (memchr(p, '\0', line_len) != NULL)
    return HTTP_ERR_MALFORMED;

  /* Exactly three space-separated fields: two is HTTP/0.9, four means a space
   * in the target, which a client must percent-encode. */
  const char *first = memchr(p, ' ', line_len);
  if (first == NULL)
    return HTTP_ERR_MALFORMED;
  size_t method_len = (size_t)(first - p);
  const char *rest = first + 1;
  size_t rest_len = line_len - method_len - 1;

  const char *second = memchr(rest, ' ', rest_len);
  if (second == NULL)
    return HTTP_ERR_MALFORMED;
  size_t target_len = (size_t)(second - rest);
  const char *version = second + 1;
  size_t version_len = rest_len - target_len - 1;

  if (memchr(version, ' ', version_len) != NULL)
    return HTTP_ERR_MALFORMED;
  if (method_len == 0 || target_len == 0)
    return HTTP_ERR_MALFORMED;

  /* Malformed rather than unsupported: "HTTP/1" and "http/1.1" do not say
   * which version they meant, so there is nothing for routing to judge. */
  if (!version_shape_ok(version, version_len))
    return HTTP_ERR_MALFORMED;

  if (method_len == 3 && memcmp(p, "GET", 3) == 0)
    out->method = HTTP_METHOD_GET;
  else if (method_len == 4 && memcmp(p, "HEAD", 4) == 0)
    out->method = HTTP_METHOD_HEAD;
  else
    /* Well-formed but not one we serve. Methods are case-sensitive, so "get"
     * lands here and becomes a 405 rather than being corrected. */
    out->method = HTTP_METHOD_OTHER;

  /* Verbatim: no request byte reaches the filesystem, so there is nothing to
   * decode for. Every target form is accepted here and sorted out by routing.
   */
  out->target = rest;
  out->target_len = target_len;

  const char *query = memchr(rest, '?', target_len);
  out->path = rest;
  out->path_len = query == NULL ? target_len : (size_t)(query - rest);

  out->major = version[5] - '0';
  out->minor = version[7] - '0';

  out->line = p;
  out->line_len = line_len;
  return HTTP_OK;
}

/* Reports whether the request's path is one the page is served at. */
static int path_is_served(const HttpRequest *req) {
  size_t root_len = sizeof(kRootPath) - 1;
  size_t index_len = sizeof(kIndexPath) - 1;
  return (req->path_len == root_len &&
          memcmp(req->path, kRootPath, root_len) == 0) ||
         (req->path_len == index_len &&
          memcmp(req->path, kIndexPath, index_len) == 0);
}

HttpResponse http_route(const HttpRequest *req, const HttpPage *page,
                        char *scratch, size_t scratch_size) {
  /* Version before method, and that order is the contract: an HTTP/2 preface
   * ("PRI * HTTP/2.0") is a 505 and not a 405. The minor version is not looked
   * at, so HTTP/1.9 is a version 1 client and is served. */
  if (req->major != 1)
    return http_error_response(505, scratch, scratch_size);
  if (req->method != HTTP_METHOD_GET && req->method != HTTP_METHOD_HEAD)
    return http_error_response(405, scratch, scratch_size);
  if (!path_is_served(req))
    return http_error_response(404, scratch, scratch_size);

  /* A HEAD is routed exactly like a GET, body included: withholding it is
   * http_write_response's job, since the length must be the GET's. */
  HttpResponse resp;
  resp.status = 200;
  resp.reason = http_status_reason(200);
  resp.body = page->body;
  resp.body_len = page->len;
  resp.allow = NULL;
  return resp;
}

HttpResult http_read_request(FILE *in, char *buf, size_t cap, size_t *out_len) {
  size_t n = 0;
  *out_len = 0;

  for (;;) {
    int c = fgetc(in);
    if (c == EOF) {
      /* Snapshotted before anything else runs, ferror included: a successful
       * call may set errno, and the timeout and the read failure below are
       * told apart by this value alone. */
      int err = errno;
      *out_len = n;
      if (!ferror(in))
        /* End of input. At the first byte this is a browser's speculative
         * connection, which is ordinary and gets its own result so the log can
         * stay calm about it. */
        return HTTP_ERR_CLOSED;
      /* SO_RCVTIMEO surfaces as EAGAIN on the underlying read, which stdio
       * reports as an error rather than as end of input. */
      if (err == EAGAIN || err == EWOULDBLOCK)
        return HTTP_ERR_TIMEOUT;
      return HTTP_ERR_READ;
    }

    if (n == cap) {
      *out_len = n;
      return HTTP_ERR_TOO_LARGE;
    }
    buf[n++] = (char)c;

    /* The blank line that ends the header block, in every spelling. Requiring a
     * '\n' before it keeps this off the request line's own CRLF. */
    if (n >= 2 && buf[n - 1] == '\n' && buf[n - 2] == '\n')
      break;
    if (n >= 3 && buf[n - 1] == '\n' && buf[n - 2] == '\r' &&
        buf[n - 3] == '\n')
      break;
  }

  /* Whatever follows stays unread. It is a request body, and leaving it there
   * is why the close is a shutdown plus a drain rather than a bare close. */
  *out_len = n;
  return HTTP_OK;
}

/* Writes n bytes, returning 1 on success and 0 on failure. Callers stop at the
 * first failure so errno still belongs to the call that failed. */
static int put_bytes(FILE *out, const char *data, size_t n) {
  return n == 0 || fwrite(data, 1, n, out) == n;
}

HttpResult http_write_response(FILE *out, const HttpResponse *resp,
                               int suppress_body) {
  /* The header order is fixed so the response bytes are golden and one
   * assertion can cover all of them. */
  if (fprintf(out, "HTTP/1.1 %d %s\r\n", resp->status, resp->reason) < 0)
    return HTTP_ERR_WRITE;
  if (fprintf(out, "Server: %s\r\n", HTTP_SERVER_NAME) < 0)
    return HTTP_ERR_WRITE;
  if (fprintf(out, "Content-Type: %s\r\n", HTTP_CONTENT_TYPE) < 0)
    return HTTP_ERR_WRITE;
  /* Always the full body length, even for a HEAD. */
  if (fprintf(out, "Content-Length: %zu\r\n", resp->body_len) < 0)
    return HTTP_ERR_WRITE;
  if (fprintf(out, "Connection: close\r\n") < 0)
    return HTTP_ERR_WRITE;
  if (resp->allow != NULL && fprintf(out, "Allow: %s\r\n", resp->allow) < 0)
    return HTTP_ERR_WRITE;
  if (!put_bytes(out, "\r\n", 2))
    return HTTP_ERR_WRITE;

  if (!suppress_body && !put_bytes(out, resp->body, resp->body_len))
    return HTTP_ERR_WRITE;

  /* Flushed here: the response must be on the socket before the shutdown, and
   * a buffered stream reports a failed write at the flush. */
  if (fflush(out) != 0 || ferror(out))
    return HTTP_ERR_WRITE;
  return HTTP_OK;
}

HttpResult http_serve_connection(FILE *in, FILE *out, FILE *log,
                                 const HttpPage *page, int *left_unread) {
  char buf[HTTP_REQUEST_MAX];
  char scratch[HTTP_ERROR_PAGE_MAX];
  char safe[HTTP_LOG_LINE_MAX];
  size_t len = 0;

  if (left_unread != NULL)
    *left_unread = 0;

  HttpResult read_result = http_read_request(in, buf, sizeof buf, &len);
  /* Every read failure but one leaves nobody to answer. HTTP_ERR_TOO_LARGE is
   * the exception: that client is still there and is owed a 431. */
  if (read_result != HTTP_OK && read_result != HTTP_ERR_TOO_LARGE) {
    fprintf(log, "%s: %s\n", HTTP_PROGNAME, http_result_str(read_result));
    return read_result;
  }

  HttpResponse resp;
  int suppress_body = 0;
  if (read_result == HTTP_ERR_TOO_LARGE) {
    fprintf(log, "%s: request header block over %d bytes\n", HTTP_PROGNAME,
            HTTP_REQUEST_MAX);
    /* The rest of that request is still on its way, and the 431 below is worth
     * nothing if the close beats it there. */
    if (left_unread != NULL)
      *left_unread = 1;
    resp = http_error_response(431, scratch, sizeof scratch);
  } else {
    HttpRequest req;
    HttpResult parsed = http_parse_request(buf, len, &req);
    if (parsed != HTTP_OK) {
      /* Sanitized and quoted, so an empty line is visible as one. Located the
       * way the parser locates it: the raw first line would log `malformed
       * request ""` for a request that opened with a stray CRLF. */
      const char *line = buf;
      size_t line_len = len;
      skip_one_blank_line(&line, &line_len);
      http_sanitize(safe, sizeof safe, line, line_prefix_len(line, line_len));
      fprintf(log, "%s: malformed request \"%s\"\n", HTTP_PROGNAME, safe);
      resp = http_error_response(400, scratch, sizeof scratch);
    } else {
      http_sanitize(safe, sizeof safe, req.line, req.line_len);
      fprintf(log, "%s: request %s\n", HTTP_PROGNAME, safe);
      resp = http_route(&req, page, scratch, sizeof scratch);
      suppress_body = req.method == HTTP_METHOD_HEAD;
    }
  }

  HttpResult written = http_write_response(out, &resp, suppress_body);
  if (written != HTTP_OK) {
    fprintf(log, "%s: %s\n", HTTP_PROGNAME, http_result_str(written));
    return written;
  }

  /* The count is what went out, so a HEAD reports 0: the log records the wire,
   * the header records the resource. */
  fprintf(log, "%s: response %d %s (%zu bytes)\n", HTTP_PROGNAME, resp.status,
          resp.reason, suppress_body ? (size_t)0 : resp.body_len);
  return HTTP_OK;
}
