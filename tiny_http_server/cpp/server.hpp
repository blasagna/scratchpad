#ifndef TINY_HTTP_SERVER_CPP_SERVER_HPP
#define TINY_HTTP_SERVER_CPP_SERVER_HPP

#include <array>
#include <cstddef>
#include <filesystem>
#include <istream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>

#include "http.hpp"

namespace http_server {

// Where the server binds when --host is not given. The exercise says to open a
// browser at localhost, which loopback satisfies; see README for why the
// default is not 0.0.0.0.
inline constexpr std::string_view kDefaultHost = "127.0.0.1";

// The port the exercise asks for.
inline constexpr int kDefaultPort = 8080;

// How many seconds a connection may spend waiting for bytes by default.
inline constexpr int kDefaultTimeoutSeconds = 5;

// Largest --file page loaded, in bytes. The page lives in memory for the
// process's whole life and Content-Length is derived from it, so an uncapped
// read is a memory bug wearing an option's clothes. Exceeding this is a startup
// error rather than a truncation - half a page served as a whole one is worse
// than a refusal that says why.
inline constexpr std::size_t kMaxPageBytes = 1024 * 1024;

// An owned file descriptor.
//
// The first RAII descriptor type in this repo - mini_shell's C++ port keeps its
// pipe in a bare int[2] with a hand-written close helper - and it earns its
// place because a listener and every accepted connection outlive the function
// that made them. What it buys is that the C port's cleanup table, where
// getting one row backwards is either a leak or a double close of somebody
// else's descriptor, has nothing left to get wrong.
class Fd {
public:
  Fd() = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() { reset(); }

  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd &operator=(Fd &&other) noexcept;

  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }

  // Gives up ownership without closing.
  int release() noexcept;

  // Closes the descriptor, if any, without disturbing the errno a caller is
  // about to report: close() can fail, and would otherwise overwrite it.
  void reset() noexcept;

private:
  int fd_ = -1;
};

// A std::streambuf over a connected socket.
//
// This is the whole reason the C++ port needs no dup and no second stream. C
// requires a file-positioning call between a read and a following write on one
// stream, and a socket has none - fseek returns ESPIPE - so the C port opens
// "r" on the accepted descriptor and "w" on a dup of it. That rule belongs to
// stdio and to std::basic_filebuf, not to std::streambuf: a buffer written here
// keeps its own get and put areas over one descriptor, and the transition
// between them is nothing at all.
//
// It does not own the descriptor. Writes go out with send() and reads come in
// with recv(), both retried on EINTR, and the error behind a failing call is
// kept in last_error() - which is what read_request's probe reads to tell a
// receive timeout from a real failure, since a streambuf that has stopped
// producing bytes looks like end of input either way.
class SocketStreambuf : public std::streambuf {
public:
  explicit SocketStreambuf(int fd) noexcept;
  ~SocketStreambuf() override;

  std::error_code last_error() const noexcept { return error_; }

protected:
  int_type underflow() override;
  int_type overflow(int_type ch) override;
  int sync() override;

private:
  // Sends everything in the put area, then empties it. Returns false and
  // records the error on failure.
  bool flush_put_area() noexcept;

  static constexpr std::size_t kBufferSize = 8192;

  int fd_;
  std::error_code error_{};
  std::array<char, kBufferSize> in_{};
  std::array<char, kBufferSize> out_{};
};

// A connected socket as one stream, readable and writable.
//
// Passed twice to serve_connection, as its in and its out. The transaction
// itself never learns that the two are the same object, which is what keeps the
// tests able to hand it a std::istringstream and a std::ostringstream instead.
class SocketStream : public std::iostream {
public:
  explicit SocketStream(int fd);

  std::error_code last_error() const noexcept { return buf_.last_error(); }

private:
  SocketStreambuf buf_;
};

// How the server behaves.
struct Options {
  std::string host{kDefaultHost};
  int port = kDefaultPort;
  // Serve exactly one request, then return. What an end-to-end check uses
  // instead of backgrounding the server and killing it by PID. One request and
  // not one connection: a browser's silent preconnect is a connection, and
  // stopping on it exits having served nothing.
  bool serve_once = false;
  // Seconds a connection may spend waiting for bytes, in either direction. Not
  // a command-line option: its most obvious setting, 0, means "no timeout" and
  // puts back the wedged-loop bug the timeout exists to prevent. It lives here
  // so the tests can shorten it.
  int io_timeout_seconds = kDefaultTimeoutSeconds;
  // What is served at a known path: the built-in page, or --file's bytes, which
  // the caller owns and must keep alive.
  std::string_view page = builtin_page();
};

// A bound, listening socket.
//
// port is what getsockname reported, which is the port actually in use rather
// than the one asked for - the two differ whenever --port 0 asked the kernel to
// choose.
struct Listener {
  Fd fd;
  std::string host;
  int port = 0;
};

// Reads a --file page into memory, once, at startup.
//
// Called before the socket exists, so an unreadable page is a startup failure
// with a message rather than a 500 that shows up later depending on which path
// somebody visits. That is also what keeps route() pure and infallible: there
// is no 500 anywhere in this server, because nothing routing does can fail. The
// cost is that the page cannot change while the server runs; restart to change
// it.
//
// out receives the bytes and is untouched unless the result is kOk. A file
// larger than max_bytes is refused, not truncated.
//
// Returns kOk, or kOpen with the failing errno, kNotRegular, or kTooLarge. An
// empty file is not an error: it is a page of zero bytes and is served as one.
Result load_page(const std::filesystem::path &path, std::size_t max_bytes,
                 std::string &out);

// Creates the listening socket.
//
// socket, SO_REUSEADDR, bind, listen, getsockname, in that order. SO_REUSEADDR
// has to come before the bind, and without it restarting inside about a minute
// fails EADDRINUSE on the TIME_WAIT remnants of the connections just served -
// which is every Ctrl-C and rerun. It is not SO_REUSEPORT: a second live
// listener is still refused, so EADDRINUSE keeps meaning "the server is already
// running" instead of silently splitting traffic between two servers.
//
// getsockname runs unconditionally rather than only for port 0, so there is one
// code path and the logged port is always the one really in use.
//
// The host must be a dotted quad; it is parsed here with inet_pton and nothing
// resolves names.
//
// Returns kOk, or kSocket, kBind, or kListen, each carrying the failing errno.
// All three are fatal: there is no server without them.
Result listen(const Options &opts, Listener &out);

// Accepts one connection, serves it, and closes it.
//
// The connection is closed with a shutdown and a bounded drain rather than a
// bare close. Linux sends RST instead of FIN when a socket still holds unread
// inbound data, and a peer may discard data it already received when it gets an
// RST - so `curl -d x` would report a connection reset instead of showing the
// 405 that was really sent, since a request body is deliberately never read.
//
// Returns kOk when a response was written, whatever its status. Everything a
// client can do comes back as a per-connection failure the caller logs and
// moves past, kConnection included - an accepted connection that could not be
// set up is this connection's failure and is logged here. Only kAccept is worth
// ending the server over, it comes from the accept alone, and run() decides
// which accept failures are.
Result accept_once(const Listener &l, const Options &opts, std::ostream &log);

// Binds, then serves connections until something fatal happens.
//
// One connection at a time: accept, read, respond, close, accept. Nothing here
// forks and nothing threads, so there is no reaping and no shared state, and a
// slow client stalls the next one - which is what the receive timeout bounds.
//
// A client cannot end the server. Every failure a peer can cause is logged
// against its connection and the loop continues; only the listening socket's
// own failures, and an accept error that is not transient, come back from here.
// Logging and continuing on every accept error was the alternative and was
// rejected: EMFILE would then spin at 100% CPU writing one line forever.
//
// Returns kOk only when opts.serve_once answered a request; a connection that
// sent nothing does not count, or --once against a browser exits on the
// preconnect. Otherwise the fatal result.
Result run(const Options &opts, std::ostream &log);

} // namespace http_server

#endif // TINY_HTTP_SERVER_CPP_SERVER_HPP
