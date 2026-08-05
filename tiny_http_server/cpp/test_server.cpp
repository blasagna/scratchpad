#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.hpp"

namespace {

using http_server::Fd;
using http_server::Listener;
using http_server::Options;
using http_server::Result;
using http_server::Stage;

std::string tmp_path(std::string_view name) {
  const char *dir = std::getenv("TEST_TMPDIR");
  return std::string(dir != nullptr ? dir : ".") + "/" + std::string(name);
}

void write_file(const std::string &path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  ASSERT_TRUE(out.good());
}

// --- load_page -----------------------------------------------------------

TEST(LoadPage, ReadsAFileAndReportsItsLength) {
  std::string path = tmp_path("page.html");
  write_file(path, "<p>hi</p>\n");
  std::string page;
  ASSERT_TRUE(http_server::load_page(path, http_server::kMaxPageBytes, page));
  EXPECT_EQ(page, "<p>hi</p>\n");
}

TEST(LoadPage, ReadsAnEmptyFile) {
  // Not an error: a page of zero bytes is served with Content-Length: 0.
  std::string path = tmp_path("empty.html");
  write_file(path, "");
  std::string page = "stale";
  ASSERT_TRUE(http_server::load_page(path, http_server::kMaxPageBytes, page));
  EXPECT_EQ(page, "");
}

TEST(LoadPage, PreservesNulBytes) {
  // Content-Length says where the body ends, not a NUL, so the file's bytes go
  // out exactly as they are.
  std::string path = tmp_path("nul.html");
  write_file(path, std::string_view("a\0b", 3));
  std::string page;
  ASSERT_TRUE(http_server::load_page(path, http_server::kMaxPageBytes, page));
  EXPECT_EQ(page, std::string("a\0b", 3));
}

TEST(LoadPage, ReadsAFileLargerThanOneChunk) {
  std::string path = tmp_path("big.html");
  std::string contents(50000, 'x');
  write_file(path, contents);
  std::string page;
  ASSERT_TRUE(http_server::load_page(path, http_server::kMaxPageBytes, page));
  EXPECT_EQ(page.size(), contents.size());
}

TEST(LoadPage, ReportsAMissingFileWithTheErrorIntact) {
  std::string page;
  Result r = http_server::load_page(tmp_path("nope.html"),
                                    http_server::kMaxPageBytes, page);
  EXPECT_EQ(r.stage, Stage::kOpen);
  // The error travels in the value rather than being left in errno, so nothing
  // between here and the report can disturb it.
  EXPECT_EQ(r.ec, std::errc::no_such_file_or_directory);
}

TEST(LoadPage, RefusesADirectory) {
  // open on a directory succeeds on Linux and only fails inside the read, with
  // EISDIR, which would be reported as a read error. The fstat is what turns
  // that into a message that says what is actually wrong.
  const char *dir = std::getenv("TEST_TMPDIR");
  std::string page;
  EXPECT_EQ(http_server::load_page(dir != nullptr ? dir : ".",
                                   http_server::kMaxPageBytes, page)
                .stage,
            Stage::kNotRegular);
}

TEST(LoadPage, RefusesAFileOverTheCap) {
  // Refused, not truncated: half a page served as a whole one is worse than a
  // startup error that says why.
  std::string path = tmp_path("over.html");
  write_file(path, std::string(101, 'x'));
  std::string page;
  EXPECT_EQ(http_server::load_page(path, 100, page).stage, Stage::kTooLarge);
}

TEST(LoadPage, AcceptsAFileExactlyAtTheCap) {
  std::string path = tmp_path("exact.html");
  write_file(path, std::string(100, 'x'));
  std::string page;
  ASSERT_TRUE(http_server::load_page(path, 100, page));
  EXPECT_EQ(page.size(), 100u);
}

TEST(LoadPage, LeavesTheOutputAloneWhenItFails) {
  std::string page = "the page already loaded";
  EXPECT_FALSE(http_server::load_page(tmp_path("nope.html"),
                                      http_server::kMaxPageBytes, page));
  EXPECT_EQ(page, "the page already loaded");
}

// --- SocketStream --------------------------------------------------------

// The test binary does not run our main, so the SIGPIPE that main ignores is
// still fatal here. Any test that writes to a socket the peer may have closed
// needs this itself.
void ignore_sigpipe() { ASSERT_NE(std::signal(SIGPIPE, SIG_IGN), SIG_ERR); }

TEST(SocketStream, ReadsThenWritesOnOneDescriptor) {
  // The whole reason there is no dup and no second stream in this port. C
  // requires a positioning call between a read and a following write on one
  // stream and a socket has none, so the C port opens "r" on the accepted
  // descriptor and "w" on a dup of it. A streambuf has no such rule, and this
  // is what says so: read to exhaustion, then write, on one fd, and both ends
  // of the exchange arrive intact.
  ignore_sigpipe();
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd server{fds[0]};
  Fd client{fds[1]};

  const std::string request = "GET / HTTP/1.1\r\n\r\n";
  ASSERT_EQ(::write(client.get(), request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ASSERT_EQ(::shutdown(client.get(), SHUT_WR), 0);

  http_server::SocketStream stream(server.get());
  std::string got;
  char c = '\0';
  while (stream.get(c))
    got.push_back(c);
  EXPECT_EQ(got, request);

  stream.clear();
  stream << "HTTP/1.1 200 OK\r\n";
  stream.flush();
  EXPECT_TRUE(stream.good());

  char buf[64] = {};
  ssize_t n = ::read(client.get(), buf, sizeof buf - 1);
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<std::size_t>(n)),
            "HTTP/1.1 200 OK\r\n");
}

TEST(SocketStream, ReportsNoErrorWhenThePeerClosesCleanly) {
  // An empty last_error() is how read_request tells a browser's silent
  // preconnect from a receive timeout, so it has to stay empty here.
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd server{fds[0]};
  {
    Fd closed{fds[1]};
  }

  http_server::SocketStream stream(server.get());
  char c = '\0';
  EXPECT_FALSE(stream.get(c));
  EXPECT_FALSE(stream.last_error());
}

TEST(SocketStream, RecordsTheErrorWhenAWriteFails) {
  ignore_sigpipe();
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd server{fds[0]};
  {
    Fd closed{fds[1]};
  }

  http_server::SocketStream stream(server.get());
  stream << std::string(64 * 1024, 'x');
  stream.flush();
  EXPECT_FALSE(stream.good());
  EXPECT_TRUE(stream.last_error());
}

// --- Fd ------------------------------------------------------------------

TEST(FdOwner, ClosesOnDestructionAndOnlyOnce) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  int raw = fds[0];
  {
    Fd owner{raw};
    EXPECT_TRUE(owner);
    EXPECT_EQ(owner.get(), raw);
  }
  // Closing an already-closed descriptor is EBADF, which is how the destructor
  // is caught having run exactly once.
  EXPECT_EQ(::close(raw), -1);
  EXPECT_EQ(errno, EBADF);
  ::close(fds[1]);
}

TEST(FdOwner, MovingTransfersOwnership) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd first{fds[0]};
  Fd second = std::move(first);
  EXPECT_FALSE(first);
  EXPECT_EQ(second.get(), fds[0]);
  ::close(fds[1]);
}

TEST(FdOwner, ReleaseGivesUpOwnershipWithoutClosing) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  int raw = -1;
  {
    Fd owner{fds[0]};
    raw = owner.release();
    EXPECT_FALSE(owner);
  }
  EXPECT_EQ(raw, fds[0]);
  EXPECT_EQ(::close(raw), 0);
  ::close(fds[1]);
}

// --- listen / accept_once (the only tests that touch a real socket) ------

// Connects to a listener on loopback and returns the client's descriptor.
Fd connect_to(const Listener &l) {
  Fd fd{::socket(AF_INET, SOCK_STREAM, 0)};
  EXPECT_TRUE(fd);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(l.port));
  EXPECT_EQ(::inet_pton(AF_INET, l.host.c_str(), &addr.sin_addr), 1);
  // This completes without the server ever calling accept: the kernel finishes
  // the handshake and parks the connection on the listen queue. That single
  // fact is why these tests need no thread, no fork, and no sleep - and why
  // this package has no fake socket layer, which would only have tested the
  // fake.
  EXPECT_EQ(
      ::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof addr), 0);
  return fd;
}

void send_all(const Fd &fd, std::string_view data) {
  ASSERT_EQ(::write(fd.get(), data.data(), data.size()),
            static_cast<ssize_t>(data.size()));
}

// Reads a client descriptor to end of input.
std::string read_all(const Fd &fd) {
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd.get(), buf, sizeof buf);
    if (n <= 0)
      break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

// Port 0 asks the kernel for a free one, so these never collide with a server
// left running in another terminal - which is exactly when someone is most
// likely to be running the tests.
Options ephemeral_options(int timeout_seconds = 2) {
  Options opts;
  opts.port = 0;
  opts.io_timeout_seconds = timeout_seconds;
  return opts;
}

TEST(RealSocket, BindsAnEphemeralPortAndServesOneRequest) {
  ignore_sigpipe();

  Options opts = ephemeral_options();
  Listener l;
  ASSERT_TRUE(http_server::listen(opts, l));
  EXPECT_GT(l.port, 0);

  Fd client = connect_to(l);
  send_all(client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

  std::ostringstream log;
  EXPECT_TRUE(http_server::accept_once(l, opts, log));

  EXPECT_EQ(
      read_all(client),
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

  std::string logged = log.str();
  EXPECT_NE(logged.find("connection from 127.0.0.1:"), std::string::npos);
  EXPECT_NE(logged.find("request GET / HTTP/1.1\n"), std::string::npos);
  EXPECT_NE(logged.find("response 200 OK (178 bytes)\n"), std::string::npos);
  EXPECT_NE(logged.find("connection closed\n"), std::string::npos);
}

TEST(RealSocket, ServesARequestWithAnUnreadBody) {
  // What `curl -d x` does. The body is never read, and the shutdown plus drain
  // before the close is what keeps the client from seeing a connection reset in
  // place of the response.
  ignore_sigpipe();

  Options opts = ephemeral_options();
  Listener l;
  ASSERT_TRUE(http_server::listen(opts, l));

  Fd client = connect_to(l);
  send_all(client, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n"
                   "hello");

  std::ostringstream log;
  EXPECT_TRUE(http_server::accept_once(l, opts, log));

  std::string response = read_all(client);
  EXPECT_TRUE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
  EXPECT_NE(response.find("Allow: GET, HEAD\r\n"), std::string::npos);
}

TEST(RealSocket, RefusesASecondListenerOnTheSamePort) {
  // This is what pins SO_REUSEADDR as not being SO_REUSEPORT. The first lets a
  // restart bind over the TIME_WAIT remnants of connections just served; the
  // second would allow two live servers on one port and split traffic between
  // them at random. Without this test, swapping one for the other passes
  // everything else here.
  Options first = ephemeral_options();
  Listener a;
  ASSERT_TRUE(http_server::listen(first, a));

  Options second = ephemeral_options();
  second.port = a.port;
  Listener b;
  Result r = http_server::listen(second, b);
  EXPECT_EQ(r.stage, Stage::kBind);
  EXPECT_EQ(r.ec, std::errc::address_in_use);
}

TEST(RealSocket, ReportsABadHostRatherThanBinding) {
  Options opts = ephemeral_options();
  opts.host = "localhost";
  Listener l;
  Result r = http_server::listen(opts, l);
  EXPECT_EQ(r.stage, Stage::kBind);
  EXPECT_EQ(r.ec, std::errc::invalid_argument);
  EXPECT_FALSE(l.fd);
}

TEST(RealSocket, DropsAClientThatSendsNothing) {
  // A browser's speculative connection: connected, then silent. On a server
  // that handles one connection at a time this is a wedge, and the receive
  // timeout is the only thing that stops it being one. Costs a second of wall
  // clock, which is the price of the one test that can cover the timeout at
  // all.
  ignore_sigpipe();

  Options opts = ephemeral_options(1);
  Listener l;
  ASSERT_TRUE(http_server::listen(opts, l));

  Fd client = connect_to(l);

  std::ostringstream log;
  EXPECT_EQ(http_server::accept_once(l, opts, log).stage, Stage::kTimeout);
  EXPECT_NE(log.str().find("client sent nothing before the read timeout"),
            std::string::npos);
}

TEST(RealSocket, AnswersAnUnknownPathOverARealConnection) {
  ignore_sigpipe();

  Options opts = ephemeral_options();
  Listener l;
  ASSERT_TRUE(http_server::listen(opts, l));

  Fd client = connect_to(l);
  send_all(client, "GET /favicon.ico HTTP/1.1\r\n\r\n");

  std::ostringstream log;
  EXPECT_TRUE(http_server::accept_once(l, opts, log));
  EXPECT_NE(log.str().find("response 404 Not Found"), std::string::npos);
  EXPECT_TRUE(read_all(client).starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST(RealSocket, ReportsAClientThatHangsUpWithoutSendingAnything) {
  // The other half of the preconnect case, and the one that has to stay
  // distinct from the timeout: a clean close leaves no error behind, so the log
  // stays calm about what is an entirely ordinary event.
  ignore_sigpipe();

  Options opts = ephemeral_options();
  Listener l;
  ASSERT_TRUE(http_server::listen(opts, l));

  {
    Fd client = connect_to(l);
  }

  std::ostringstream log;
  EXPECT_EQ(http_server::accept_once(l, opts, log).stage, Stage::kClosed);
  EXPECT_NE(
      log.str().find("client closed the connection without sending a request"),
      std::string::npos);
}

// run() itself has no test here, and cannot have one in this shape: a client
// has to be connected before the loop accepts it, and run() does its own
// binding, so nothing single-threaded can be waiting on the queue by the time
// it starts. Its two decisions - --once stopping after one answered connection,
// and a fatal bind failure - are covered end to end from the shell instead; see
// the verification list in cpp/README.md and tiny_http_server/check_parity.sh.

} // namespace
