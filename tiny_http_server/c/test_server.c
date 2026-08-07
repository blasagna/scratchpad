#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

extern "C" {
#include "server.h"
}

/* Runs body against an in-memory output stream and returns what it wrote. */
template<typename F> static std::string captured(F body) {
  char buf[8192];
  FILE *out = fmemopen(buf, sizeof(buf), "w");
  /* Returning early rather than running body against a NULL stream: EXPECT_ is
   * non-fatal, so the alternative is a segfault in place of a failure. */
  EXPECT_NE(out, nullptr);
  if (out == nullptr)
    return "";
  body(out);
  long written = ftell(out);
  EXPECT_EQ(fclose(out), 0);
  EXPECT_LT(static_cast<size_t>(written < 0 ? 0 : written), sizeof(buf))
      << "captured() buffer is too small; the output was truncated";
  return std::string(buf, static_cast<size_t>(written < 0 ? 0 : written));
}

static std::string tmp_path(const char *name) {
  const char *dir = getenv("TEST_TMPDIR");
  return std::string(dir ? dir : ".") + "/" + name;
}

static void write_file(const std::string &path, const std::string &contents) {
  FILE *f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(fwrite(contents.data(), 1, contents.size(), f), contents.size());
  ASSERT_EQ(fclose(f), 0);
}

/* --- server_load_page --- */

TEST(LoadPage, ReadsAFileAndReportsItsLength) {
  std::string path = tmp_path("page.html");
  write_file(path, "<p>hi</p>\n");
  HttpPage page = {nullptr, 0};
  ASSERT_EQ(server_load_page(path.c_str(), SERVER_MAX_PAGE_BYTES, &page),
            HTTP_OK);
  EXPECT_EQ(std::string(page.body, page.len), "<p>hi</p>\n");
  free(const_cast<char *>(page.body));
}

TEST(LoadPage, ReadsAnEmptyFile) {
  /* Not an error: a page of zero bytes is served with Content-Length: 0. */
  std::string path = tmp_path("empty.html");
  write_file(path, "");
  HttpPage page = {reinterpret_cast<const char *>(1), 99};
  ASSERT_EQ(server_load_page(path.c_str(), SERVER_MAX_PAGE_BYTES, &page),
            HTTP_OK);
  EXPECT_EQ(page.len, 0u);
  free(const_cast<char *>(page.body));
}

TEST(LoadPage, PreservesNulBytes) {
  /* Content-Length says where the body ends, not a NUL, so the file's bytes
   * go out exactly as they are. */
  std::string path = tmp_path("nul.html");
  write_file(path, std::string("a\0b", 3));
  HttpPage page = {nullptr, 0};
  ASSERT_EQ(server_load_page(path.c_str(), SERVER_MAX_PAGE_BYTES, &page),
            HTTP_OK);
  EXPECT_EQ(page.len, 3u);
  EXPECT_EQ(std::string(page.body, page.len), std::string("a\0b", 3));
  free(const_cast<char *>(page.body));
}

TEST(LoadPage, ReadsAFileLargerThanOneChunk) {
  std::string path = tmp_path("big.html");
  std::string contents(50000, 'x');
  write_file(path, contents);
  HttpPage page = {nullptr, 0};
  ASSERT_EQ(server_load_page(path.c_str(), SERVER_MAX_PAGE_BYTES, &page),
            HTTP_OK);
  EXPECT_EQ(page.len, contents.size());
  free(const_cast<char *>(page.body));
}

TEST(LoadPage, ReportsAMissingFileWithErrnoIntact) {
  HttpPage page = {nullptr, 0};
  errno = 0;
  EXPECT_EQ(server_load_page(tmp_path("nope.html").c_str(),
                             SERVER_MAX_PAGE_BYTES, &page),
            HTTP_ERR_OPEN);
  EXPECT_EQ(errno, ENOENT);
}

TEST(LoadPage, RefusesADirectory) {
  /* fopen on a directory succeeds and only fails inside fread with EISDIR,
   * which reads as a read error. The fstat is what says what is really wrong.
   */
  const char *dir = getenv("TEST_TMPDIR");
  HttpPage page = {nullptr, 0};
  EXPECT_EQ(server_load_page(dir ? dir : ".", SERVER_MAX_PAGE_BYTES, &page),
            HTTP_ERR_NOT_REGULAR);
}

TEST(LoadPage, RefusesAFileOverTheCap) {
  /* Refused, not truncated: half a page served as a whole one is worse than a
   * startup error that says why. */
  std::string path = tmp_path("over.html");
  write_file(path, std::string(101, 'x'));
  HttpPage page = {nullptr, 0};
  EXPECT_EQ(server_load_page(path.c_str(), 100, &page), HTTP_ERR_TOO_LARGE);
}

TEST(LoadPage, AcceptsAFileExactlyAtTheCap) {
  std::string path = tmp_path("exact.html");
  write_file(path, std::string(100, 'x'));
  HttpPage page = {nullptr, 0};
  ASSERT_EQ(server_load_page(path.c_str(), 100, &page), HTTP_OK);
  EXPECT_EQ(page.len, 100u);
  free(const_cast<char *>(page.body));
}

/* --- server_listen / server_accept_once (the only tests that touch a socket)
 */

/* The test binary does not run our main, so the SIGPIPE it ignores is still
 * fatal here. Any test that writes to a socket the peer may have closed needs
 * this itself. */
static void ignore_sigpipe() { ASSERT_NE(signal(SIGPIPE, SIG_IGN), SIG_ERR); }

/* Connects to a listener on loopback and returns the client's descriptor. */
static int connect_to(const ServerListener &l) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(l.port));
  EXPECT_EQ(inet_pton(AF_INET, l.host, &addr.sin_addr), 1);
  /* This completes without the server ever calling accept: the kernel parks the
   * connection on the listen queue. That is why these tests need no thread, no
   * fork, and no sleep - and why there is no fake socket layer. */
  EXPECT_EQ(
      connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)), 0);
  return fd;
}

/* Reads a client descriptor to end of input. */
static std::string read_all(int fd) {
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

TEST(RealSocket, BindsAnEphemeralPortAndServesOneRequest) {
  ignore_sigpipe();

  ServerOptions opts = server_options_default();
  /* Port 0 asks the kernel for a free one, so this never collides with a server
   * left running in another terminal. */
  opts.port = 0;
  opts.io_timeout_seconds = 2;

  ServerListener l;
  ASSERT_EQ(server_listen(&opts, &l), HTTP_OK);
  EXPECT_GT(l.port, 0);

  int client = connect_to(l);
  ASSERT_GE(client, 0);
  const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_EQ(write(client, req, strlen(req)), static_cast<ssize_t>(strlen(req)));

  std::string log = captured(
      [&](FILE *f) { EXPECT_EQ(server_accept_once(&l, &opts, f), HTTP_OK); });

  std::string response = read_all(client);
  EXPECT_EQ(
      response,
      "HTTP/1.1 200 OK\r\n"
      "Server: tiny_http_server\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "Content-Length: 178\r\n"
      "Connection: close\r\n"
      "\r\n"
      "<!DOCTYPE html>\n"
      "<html lang=\"en\">\n"
      "<head><meta charset=\"utf-8\"><title>tiny_http_server</title></head>\n"
      "<body><h1>Hello, world!</h1><p>Served by tiny_http_server.</p></body>\n"
      "</html>\n");

  EXPECT_NE(log.find("connection from 127.0.0.1:"), std::string::npos);
  EXPECT_NE(log.find("request GET / HTTP/1.1\n"), std::string::npos);
  EXPECT_NE(log.find("response 200 OK (178 bytes)\n"), std::string::npos);
  EXPECT_NE(log.find("connection closed\n"), std::string::npos);

  close(client);
  server_close(&l);
}

TEST(RealSocket, ServesARequestWithAnUnreadBody) {
  /* What `curl -d x` does. The body is never read, and the shutdown plus drain
   * is what keeps the client from seeing a reset in place of the response. */
  ignore_sigpipe();

  ServerOptions opts = server_options_default();
  opts.port = 0;
  opts.io_timeout_seconds = 2;
  ServerListener l;
  ASSERT_EQ(server_listen(&opts, &l), HTTP_OK);

  int client = connect_to(l);
  ASSERT_GE(client, 0);
  const char *req =
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
  ASSERT_EQ(write(client, req, strlen(req)), static_cast<ssize_t>(strlen(req)));

  captured(
      [&](FILE *f) { EXPECT_EQ(server_accept_once(&l, &opts, f), HTTP_OK); });

  std::string response = read_all(client);
  EXPECT_EQ(response.find("HTTP/1.1 405 Method Not Allowed\r\n"), 0u);
  EXPECT_NE(response.find("Allow: GET, HEAD\r\n"), std::string::npos);

  close(client);
  server_close(&l);
}

TEST(RealSocket, RefusesASecondListenerOnTheSamePort) {
  /* This pins SO_REUSEADDR as not being SO_REUSEPORT, which would allow two
   * live servers on one port and split traffic between them at random. Without
   * it, swapping one for the other passes everything else here. */
  ServerOptions first = server_options_default();
  first.port = 0;
  ServerListener a;
  ASSERT_EQ(server_listen(&first, &a), HTTP_OK);

  ServerOptions second = server_options_default();
  second.port = a.port;
  ServerListener b;
  errno = 0;
  EXPECT_EQ(server_listen(&second, &b), HTTP_ERR_BIND);
  EXPECT_EQ(errno, EADDRINUSE);

  server_close(&a);
}

TEST(RealSocket, ReportsABadHostRatherThanBinding) {
  ServerOptions opts = server_options_default();
  opts.host = "localhost";
  opts.port = 0;
  ServerListener l;
  EXPECT_EQ(server_listen(&opts, &l), HTTP_ERR_BIND);
  EXPECT_EQ(l.fd, -1);
}

TEST(RealSocket, DropsAClientThatSendsNothing) {
  /* A browser's speculative connection: connected, then silent. On a
   * one-at-a-time server that is a wedge, and the receive timeout is the only
   * thing that stops it. Costs a second of wall clock. */
  ignore_sigpipe();

  ServerOptions opts = server_options_default();
  opts.port = 0;
  opts.io_timeout_seconds = 1;
  ServerListener l;
  ASSERT_EQ(server_listen(&opts, &l), HTTP_OK);

  int client = connect_to(l);
  ASSERT_GE(client, 0);

  std::string log = captured([&](FILE *f) {
    EXPECT_EQ(server_accept_once(&l, &opts, f), HTTP_ERR_TIMEOUT);
  });
  EXPECT_NE(log.find("client sent nothing before the read timeout"),
            std::string::npos);

  close(client);
  server_close(&l);
}

TEST(RealSocket, AnswersAnUnknownPathOverARealConnection) {
  ignore_sigpipe();

  ServerOptions opts = server_options_default();
  opts.port = 0;
  opts.io_timeout_seconds = 2;
  ServerListener l;
  ASSERT_EQ(server_listen(&opts, &l), HTTP_OK);

  int client = connect_to(l);
  ASSERT_GE(client, 0);
  const char *req = "GET /favicon.ico HTTP/1.1\r\n\r\n";
  ASSERT_EQ(write(client, req, strlen(req)), static_cast<ssize_t>(strlen(req)));

  std::string log = captured(
      [&](FILE *f) { EXPECT_EQ(server_accept_once(&l, &opts, f), HTTP_OK); });
  EXPECT_NE(log.find("response 404 Not Found"), std::string::npos);

  std::string response = read_all(client);
  EXPECT_EQ(response.find("HTTP/1.1 404 Not Found\r\n"), 0u);

  close(client);
  server_close(&l);
}

/* server_run has no test here and cannot: a client must be connected before the
 * loop accepts it, and server_run does its own binding. Its two decisions are
 * checked from the shell; see the verification list in c/README.md. */
