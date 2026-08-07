#include "server.hpp"

#include <cerrno>
#include <cstdio>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace http_server {
namespace {

// How many connections the kernel may queue behind the one being served. Not 1
// (a browser opens several per page) and not SOMAXCONN (a deep queue in front
// of a sequential server turns a retried refusal into a timeout).
constexpr int kBacklog = 16;

// The most a connection is drained of before its close. Unbounded would let a
// client that keeps sending hold a one-at-a-time server forever: the receive
// timeout bounds the wall clock and this bounds the bytes.
constexpr std::size_t kDrainMax = 64 * 1024;

// The same bound for the one connection owed a response it might not get: an
// oversized header block, answered with a 431 that a close on unread data would
// turn into an RST. A megabyte past the 8 KiB limit is past arguing with.
constexpr std::size_t kDrainOverflowMax = 1024 * 1024;

// How much is read from the page file at a time.
constexpr std::size_t kPageChunk = 8192;

// The errno a call just failed with, as a value to carry rather than a global.
// Named errno_error and not last_error on purpose: SocketStreambuf has a member
// of that name, and `error_ = last_error()` inside it would self-assign.
std::error_code errno_error() {
  return std::error_code(errno, std::generic_category());
}

// Reads and discards what the client sent and we never asked for, so the close
// sends a FIN rather than an RST. flags is MSG_DONTWAIT everywhere but the 431
// path, where the rest of the request is still in flight; see README.
void drain(int fd, int flags, std::size_t max) {
  char scrap[4096];
  std::size_t total = 0;
  while (total < max) {
    ssize_t n = ::recv(fd, scrap, sizeof scrap, flags);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    total += static_cast<std::size_t>(n);
  }
}

// Applies the read and write timeouts to an accepted connection.
bool set_timeouts(int fd, int seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  // The receive timeout keeps a browser's silent preconnect from wedging a
  // server that serves one at a time; the send timeout covers the mirror image,
  // a client that stops reading a large --file.
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

// Reports whether an accept failure is the client's doing. Both mean the peer
// vanished between the handshake and the accept, which is ordinary. EINTR is
// not here because it is already retried.
bool accept_is_transient(const std::error_code &ec) {
  return ec == std::errc::connection_aborted || ec == std::errc::protocol_error;
}

// Reports whether a connection got far enough to be answered, which is what
// --once waits for. Breaking on any connection makes --once useless against a
// browser's preconnect. kWrite counts, and so does the 431.
bool connection_was_answered(Stage stage) {
  return stage == Stage::kOk || stage == Stage::kWrite;
}

} // namespace

Fd &Fd::operator=(Fd &&other) noexcept {
  if (this != &other) {
    reset();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

int Fd::release() noexcept { return std::exchange(fd_, -1); }

void Fd::reset() noexcept {
  if (fd_ < 0)
    return;
  int saved = errno;
  ::close(fd_);
  errno = saved;
  fd_ = -1;
}

SocketStreambuf::SocketStreambuf(int fd) noexcept : fd_(fd) {
  // An empty get area, so the first read calls underflow, and a full put area.
  setg(in_.data(), in_.data(), in_.data());
  setp(out_.data(), out_.data() + out_.size());
}

SocketStreambuf::~SocketStreambuf() {
  // Every ordinary path already flushed through write_response, so this is a
  // safety net rather than the usual case.
  flush_put_area();
}

SocketStreambuf::int_type SocketStreambuf::underflow() {
  if (gptr() < egptr())
    return traits_type::to_int_type(*gptr());

  for (;;) {
    ssize_t n = ::recv(fd_, in_.data(), in_.size(), 0);
    if (n > 0) {
      error_.clear();
      setg(in_.data(), in_.data(), in_.data() + n);
      return traits_type::to_int_type(*gptr());
    }
    if (n == 0) {
      // The peer closed. Not an error, and the empty error_code is what the
      // probe reads to tell this from a timeout.
      error_.clear();
      return traits_type::eof();
    }
    if (errno == EINTR)
      continue;
    // SO_RCVTIMEO surfaces here as EAGAIN, which is a timeout rather than a
    // failure; keeping the code is what lets the caller say which.
    error_ = errno_error();
    return traits_type::eof();
  }
}

bool SocketStreambuf::flush_put_area() noexcept {
  char *begin = pbase();
  std::size_t remaining = static_cast<std::size_t>(pptr() - begin);
  while (remaining > 0) {
    ssize_t n = ::send(fd_, begin, remaining, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      error_ =
          n < 0 ? errno_error() : std::make_error_code(std::errc::io_error);
      // Drop what could not be sent: keeping it would make every later write
      // retry a send to a peer that is gone.
      setp(out_.data(), out_.data() + out_.size());
      return false;
    }
    begin += n;
    remaining -= static_cast<std::size_t>(n);
  }
  setp(out_.data(), out_.data() + out_.size());
  return true;
}

SocketStreambuf::int_type SocketStreambuf::overflow(int_type ch) {
  if (!flush_put_area())
    return traits_type::eof();
  if (!traits_type::eq_int_type(ch, traits_type::eof())) {
    *pptr() = traits_type::to_char_type(ch);
    pbump(1);
  }
  return traits_type::not_eof(ch);
}

int SocketStreambuf::sync() { return flush_put_area() ? 0 : -1; }

SocketStream::SocketStream(int fd) : std::iostream(nullptr), buf_(fd) {
  // Bases are constructed before members, so the stream starts with no buffer
  // and picks buf_ up here. rdbuf() also clears the badbit that construction on
  // a null buffer set.
  rdbuf(&buf_);
}

Result load_page(const std::filesystem::path &path, std::size_t max_bytes,
                 std::string &out) {
  Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (!fd)
    return {Stage::kOpen, errno_error()};

  // Opening a directory succeeds and only fails inside the read with EISDIR,
  // reported as "cannot read the page file". fstat on the open descriptor, not
  // the path, which would answer about a different file.
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0)
    return {Stage::kOpen, errno_error()};
  if (!S_ISREG(st.st_mode))
    return {Stage::kNotRegular, {}};

  // Read in chunks rather than trusting a size: the file may change between the
  // stat and the read, and a Content-Length from the stat would then describe a
  // page that is no longer the one being served.
  std::string data;
  char chunk[kPageChunk];
  for (;;) {
    ssize_t n = ::read(fd.get(), chunk, sizeof chunk);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return {Stage::kOpen, errno_error()};
    }
    if (n == 0)
      break;
    data.append(chunk, static_cast<std::size_t>(n));
    if (data.size() > max_bytes)
      return {Stage::kTooLarge, {}};
  }

  // An empty file is a page of zero bytes, not an error.
  out = std::move(data);
  return {};
}

Result listen(const Options &opts, Listener &out) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(opts.port));
  // A dotted quad, never a name: getaddrinfo would put a blocking DNS lookup in
  // the startup of a program that binds exactly one socket. main validates this
  // too, so reaching here with a bad host is a caller's bug.
  if (::inet_pton(AF_INET, opts.host.c_str(), &addr.sin_addr) != 1)
    return {Stage::kBind, std::make_error_code(std::errc::invalid_argument)};

  Fd fd{::socket(AF_INET, SOCK_STREAM, 0)};
  if (!fd)
    return {Stage::kSocket, errno_error()};

  // Before the bind, which is the only place it does anything.
  int on = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0)
    return {Stage::kSocket, errno_error()};

  if (::bind(fd.get(), reinterpret_cast<const sockaddr *>(&addr),
             sizeof addr) != 0)
    return {Stage::kBind, errno_error()};
  if (::listen(fd.get(), kBacklog) != 0)
    return {Stage::kListen, errno_error()};

  // Unconditionally, not only for port 0: one code path, and the logged port is
  // always the one really in use.
  sockaddr_in bound{};
  socklen_t bound_len = sizeof bound;
  if (::getsockname(fd.get(), reinterpret_cast<sockaddr *>(&bound),
                    &bound_len) != 0)
    return {Stage::kListen, errno_error()};

  out.fd = std::move(fd);
  out.host = opts.host;
  out.port = ntohs(bound.sin_port);
  return {};
}

Result accept_once(const Listener &l, const Options &opts, std::ostream &log) {
  sockaddr_in peer{};
  socklen_t peer_len = sizeof peer;
  int accepted = -1;
  // Nothing here installs a handler, so nothing should interrupt this - but a
  // profiler's SIGPROF would, and an unexplained server exit is a bad way to
  // find that out.
  do {
    accepted =
        ::accept(l.fd.get(), reinterpret_cast<sockaddr *>(&peer), &peer_len);
  } while (accepted < 0 && errno == EINTR);
  // The failure is captured into the value, which is what makes the rest of
  // this function safe to write in any order - where the C port must return
  // from the statement right after the accept. See README.
  if (accepted < 0)
    return {Stage::kAccept, errno_error()};
  Fd conn{accepted};

  // inet_ntop rather than getnameinfo: without NI_NUMERICHOST that one does a
  // reverse DNS lookup, blocking every other client on a network round trip.
  // This cannot fail for an AF_INET address and cannot block.
  char peer_host[INET_ADDRSTRLEN];
  if (::inet_ntop(AF_INET, &peer.sin_addr, peer_host, sizeof peer_host) ==
      nullptr)
    std::snprintf(peer_host, sizeof peer_host, "?");
  log << kProgName << ": connection from " << peer_host << ":"
      << ntohs(peer.sin_port) << "\n";

  // Setting a connection up fails against that connection, not the listening
  // socket, so it is kConnection and the loop moves on. kAccept would let a
  // one-off ENOMEM on one connection end the server.
  if (!set_timeouts(conn.get(), opts.io_timeout_seconds)) {
    Result failed{Stage::kConnection, errno_error()};
    log << kProgName
        << ": cannot set the connection timeouts: " << failed.ec.message()
        << "\n";
    conn.reset();
    log << kProgName << ": connection closed\n";
    return failed;
  }

  Transaction tx;
  {
    // One stream for both directions, and the reason there is no dup here: see
    // SocketStreambuf.
    SocketStream stream(conn.get());
    tx = serve_connection(stream, stream, log, opts.page,
                          [&stream] { return stream.last_error(); });

    // A lingering close, not a bare one: Linux sends an RST when a socket
    // closes with unread inbound data, so `curl -d x` would see a reset instead
    // of the 405. The 431 case waits for the rest of the request; see drain.
    ::shutdown(conn.get(), SHUT_WR);
    drain(conn.get(), tx.left_unread ? 0 : MSG_DONTWAIT,
          tx.left_unread ? kDrainOverflowMax : kDrainMax);
  }
  conn.reset();

  log << kProgName << ": connection closed\n";
  return {tx.stage, {}};
}

Result run(const Options &opts, std::ostream &log) {
  Listener l;
  if (Result opened = listen(opts, l); !opened)
    return opened;
  log << kProgName << ": listening on " << l.host << ":" << l.port << "\n";

  for (;;) {
    Result served = accept_once(l, opts, log);

    if (served.stage == Stage::kAccept) {
      // A listening socket that cannot produce connections is not a
      // per-connection event. Logging and continuing on every accept failure
      // would spin at 100% CPU on EMFILE or EBADF.
      if (!accept_is_transient(served.ec))
        return served;
      log << kProgName << ": " << describe(served.stage) << ": "
          << served.ec.message() << "\n";
      continue;
    }

    if (opts.serve_once && connection_was_answered(served.stage))
      break;
  }

  return {};
}

} // namespace http_server
