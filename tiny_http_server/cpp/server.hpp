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

// Where the server binds when --host is not given; see README for why the
// default is not 0.0.0.0.
inline constexpr std::string_view kDefaultHost = "127.0.0.1";

// The port the exercise asks for.
inline constexpr int kDefaultPort = 8080;

// How many seconds a connection may spend waiting for bytes by default.
inline constexpr int kDefaultTimeoutSeconds = 5;

// Largest --file page loaded. The page lives in memory for the process's whole
// life, so an uncapped read is a memory bug wearing an option's clothes.
// Exceeding this is a startup error, not a truncation.
inline constexpr std::size_t kMaxPageBytes = 1024 * 1024;

// An owned file descriptor. It earns its place because a listener and every
// accepted connection outlive the function that made them, and it makes the C
// port's fdopen cleanup table unrepresentable.
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

// A std::streambuf over a connected socket, and the whole reason this port
// needs no dup and no second stream: stdio's read-then-write positioning rule
// is not a streambuf's. It keeps the failing recv's error in last_error().
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

// A connected socket as one stream, readable and writable. Passed twice to
// serve_connection, which never learns the two are the same object - which is
// what keeps the tests able to hand it a pair of string streams instead.
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
  // Serve exactly one request, then return. One request and not one connection:
  // a browser's silent preconnect is a connection, and stopping on it exits
  // having served nothing.
  bool serve_once = false;
  // Seconds a connection may wait for bytes, either direction. Not an option:
  // its obvious setting, 0, means "no timeout" and puts back the wedged-loop
  // bug. It lives here so the tests can shorten it.
  int io_timeout_seconds = kDefaultTimeoutSeconds;
  // What is served at a known path: the built-in page, or --file's bytes, which
  // the caller owns and must keep alive.
  std::string_view page = builtin_page();
};

// A bound, listening socket. port is what getsockname reported, which differs
// from the one asked for whenever --port 0 let the kernel choose.
struct Listener {
  Fd fd;
  std::string host;
  int port = 0;
};

// Reads a --file page into out, before the socket exists, so an unreadable one
// is a startup failure rather than a 500 later. Over max_bytes is refused, not
// truncated; empty is a page of zero bytes. out is untouched unless kOk.
Result load_page(const std::filesystem::path &path, std::size_t max_bytes,
                 std::string &out);

// Creates the listening socket: socket, SO_REUSEADDR, bind, listen,
// getsockname, in that order and all three failures fatal. The host must be a
// dotted quad, parsed with inet_pton; nothing resolves names.
Result listen(const Options &opts, Listener &out);

// Accepts one connection, serves it, and closes it with a shutdown plus a
// bounded drain. Everything a client can do is a per-connection failure the
// caller logs and moves past; only kAccept can end the server.
Result accept_once(const Listener &l, const Options &opts, std::ostream &log);

// Binds, then serves one connection at a time until something fatal happens. A
// client cannot end the server. Returns kOk only when opts.serve_once answered
// a request; otherwise the fatal result.
Result run(const Options &opts, std::ostream &log);

} // namespace http_server

#endif // TINY_HTTP_SERVER_CPP_SERVER_HPP
