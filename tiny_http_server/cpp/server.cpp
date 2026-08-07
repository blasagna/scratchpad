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

// How many connections the kernel may queue behind the one being served.
//
// Not 1: a browser loading a single page opens a speculative connection and a
// favicon connection alongside the page's, and this server holds each one for a
// whole transaction, so a queue of one produces visible connection refusals in
// the network panel. Not SOMAXCONN (4096 here) either: a queue that deep in
// front of a server that drains it one at a time only converts a refusal, which
// a browser retries immediately, into a timeout, which it does not. Sixteen
// covers a browser's six-connections-per-host limit with room over.
constexpr int kBacklog = 16;

// The most a connection is drained of before its close. Unbounded is not an
// option: this server handles one connection at a time, so a client that keeps
// sending would hold it forever. The receive timeout bounds the wall clock and
// this bounds the bytes.
constexpr std::size_t kDrainMax = 64 * 1024;

// The same bound for the one connection that is owed a response it might not
// get: an oversized header block, answered with a 431 that a close on unread
// data would turn into an RST. A request that reaches this and keeps going has
// stopped being a request, so the cap stays - it is the page cap, since a
// header block a megabyte past the 8 KiB limit is already far past arguing
// with.
constexpr std::size_t kDrainOverflowMax = 1024 * 1024;

// How much is read from the page file at a time.
constexpr std::size_t kPageChunk = 8192;

// The errno a call just failed with, as a value to carry rather than a global
// to read later.
//
// Named errno_error and not last_error on purpose. SocketStreambuf has a member
// called last_error, and inside its member functions that name would win the
// lookup - so `error_ = last_error()` there compiles, assigns error_ to itself,
// and the receive timeout silently becomes an ordinary hang-up. Both return
// std::error_code, so nothing warns.
std::error_code errno_error() {
  return std::error_code(errno, std::generic_category());
}

// Reads and discards what the client sent and we never asked for, so the close
// below sends a FIN rather than an RST. Reads the descriptor directly instead
// of the stream: bytes sitting in the stream's buffer are already off the
// socket, and dropping the buffer discards them for free.
//
// flags is MSG_DONTWAIT for every ordinary connection, so this takes what has
// already arrived and never waits for more. A blocking drain everywhere would
// sit here until the peer closed, which costs the accept loop a round trip on
// every connection and lets a client that reads its response but keeps the
// socket open stall the whole server for the timeout. What is left is a narrow
// race - a body that arrives between the drain and the close still provokes an
// RST - and that is the right trade for a server that handles one connection at
// a time.
//
// The exception, and the only caller that passes 0, is a header block over
// kRequestMax. There the server stopped reading at 8 KiB with the rest of a
// much larger request still in flight, so what has already arrived is nowhere
// near all of it - and unlike a body nobody read, that client was answered,
// with a 431 the RST would throw away. SO_RCVTIMEO bounds the wait exactly as
// it bounds every other read on this socket.
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
  // The receive timeout is what keeps a browser's speculative connection -
  // connected, then silent - from wedging a server that serves one at a time.
  // The send timeout covers the mirror image, which only bites with a large
  // --file: a client that stops reading blocks the write once the socket's send
  // buffer fills.
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

// Reports whether an accept failure is the client's doing rather than the
// server's. Both of these mean the peer vanished between the handshake and the
// accept, which is ordinary. EINTR is not here because it is already retried.
bool accept_is_transient(const std::error_code &ec) {
  return ec == std::errc::connection_aborted || ec == std::errc::protocol_error;
}

// Reports whether a connection got far enough to be answered, which is what
// --once is waiting for.
//
// Breaking on any connection at all is the obvious rule and it makes --once
// useless against a browser: Chrome and Firefox preconnect, so the first
// connection is a silent one that times out after five seconds, and the server
// would exit having served nothing while the request the user actually made
// gets refused. kWrite counts - the response was written, the client left
// before taking it - and so does the 431 path, which is kOk.
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
  // Every ordinary path has already flushed through write_response, so this is
  // the safety net for one that did not rather than the usual case.
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
      // The peer closed. Not an error, and the empty error_code is how
      // read_request's probe tells this from a timeout.
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
      // Drop what could not be sent. Keeping it would make every later write
      // retry a send to a peer that is gone, and the stream is already bad.
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
  // and picks buf_ up here - by which time it exists. rdbuf() also clears the
  // badbit that constructing on a null buffer set.
  rdbuf(&buf_);
}

Result load_page(const std::filesystem::path &path, std::size_t max_bytes,
                 std::string &out) {
  Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (!fd)
    return {Stage::kOpen, errno_error()};

  // Opening a directory succeeds on Linux and only fails later inside the read,
  // with EISDIR, which would be reported as "cannot read the page file" for
  // what is really "that is a directory". Asking fstat first also turns down
  // /dev/zero and FIFOs before they churn through the whole cap. It is fstat on
  // the descriptor already open rather than std::filesystem::is_regular_file on
  // the path, which would be answering about a different file than the one
  // being read.
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0)
    return {Stage::kOpen, errno_error()};
  if (!S_ISREG(st.st_mode))
    return {Stage::kNotRegular, {}};

  // Read in chunks rather than trusting a size. The file may change between the
  // stat and the read, in which case a Content-Length taken from the stat would
  // describe a page that is no longer the one being served.
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

  // An empty file is a page of zero bytes, not an error: it is served with
  // Content-Length: 0 and renders as a blank page, which is what was asked for.
  out = std::move(data);
  return {};
}

Result listen(const Options &opts, Listener &out) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(opts.port));
  // A dotted quad, never a name. getaddrinfo would bring in DNS - a blocking
  // network lookup during startup, returning a list of candidates for a program
  // that binds exactly one socket. main validates this too; reaching here with
  // a bad host is a caller's bug rather than a user's.
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

  // Unconditionally, not only when port 0 asked the kernel to choose: one code
  // path, and the port that gets logged is always the one really in use.
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
  // Nothing here installs a signal handler, so nothing should interrupt this -
  // but a profiler's SIGPROF would, and an unexplained server exit is a bad way
  // to find that out.
  do {
    accepted =
        ::accept(l.fd.get(), reinterpret_cast<sockaddr *>(&peer), &peer_len);
  } while (accepted < 0 && errno == EINTR);
  // The failure is captured into the value here, which is what makes the rest
  // of this function safe to write in any order. The C port has to return from
  // the statement immediately after the accept, with no logging in between,
  // because run() judges fatality off the global errno and even a successful
  // write may set one - and a fatal EMFILE misread as a transient ECONNABORTED
  // is a loop spinning at 100% CPU forever.
  if (accepted < 0)
    return {Stage::kAccept, errno_error()};
  Fd conn{accepted};

  // inet_ntop rather than getnameinfo: without NI_NUMERICHOST that one does a
  // reverse DNS lookup, which on a loop that serves one connection at a time
  // blocks every other client on a network round trip, and can hang for seconds
  // against a broken resolver. This cannot fail for an AF_INET address and
  // cannot block.
  char peer_host[INET_ADDRSTRLEN];
  if (::inet_ntop(AF_INET, &peer.sin_addr, peer_host, sizeof peer_host) ==
      nullptr)
    std::snprintf(peer_host, sizeof peer_host, "?");
  log << kProgName << ": connection from " << peer_host << ":"
      << ntohs(peer.sin_port) << "\n";

  // Setting an accepted connection up fails against that connection, not
  // against the listening socket, so it comes back as kConnection and the loop
  // moves on. Returning kAccept would put a one-off ENOMEM on one connection in
  // front of run()'s fatal-or-not decision, which is a client's request ending
  // the server.
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

    // A lingering close, not a bare one. Linux sends an RST rather than a FIN
    // when a socket is closed with unread inbound data, and a peer is allowed
    // to throw away data it already received when it gets an RST - so `curl -d
    // x`, whose request body this server deliberately never reads, would report
    // a connection reset instead of showing the 405 that really was sent.
    //
    // An oversized header block is the one case where what is left unread is
    // not a body the client already finished sending but the remainder of a
    // request still on its way, so that one waits for it; see drain.
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
      // Everything a client does is a per-connection event, but a listening
      // socket that cannot produce connections is not. Logging and continuing
      // on every accept failure was the alternative and was rejected: EMFILE or
      // EBADF would then spin at 100% CPU writing the same line forever, which
      // is far worse than exiting with it.
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
