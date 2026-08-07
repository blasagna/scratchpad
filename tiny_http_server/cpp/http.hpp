#ifndef TINY_HTTP_SERVER_CPP_HTTP_HPP
#define TINY_HTTP_SERVER_CPP_HTTP_HPP

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace http_server {

// Prefix on every line the server logs, matching the binary name.
inline constexpr std::string_view kProgName = "tiny_http_server";

// Value of the Server header. A constant, so the response bytes stay golden.
inline constexpr std::string_view kServerName = "tiny_http_server";

// The only content type this server ever sends; nothing is sniffed.
inline constexpr std::string_view kContentType = "text/html; charset=utf-8";

// Largest request header block accepted. Reaching this without a terminator is
// a 431, not a truncation.
inline constexpr std::size_t kRequestMax = 8192;

// Longest request line written to the log, after sanitizing. 255 and not 256:
// the C port spells this as a 256-byte buffer and spends one byte on the NUL,
// so the count is the shared contract and the buffer was only how C said it.
inline constexpr std::size_t kLogLineMax = 255;

// The stage at which an HTTP or socket operation failed; Result::ec carries the
// errno behind it where there was one. A 4xx/5xx is not here - the server did
// its job by answering. Which stages are fatal is in the README.
enum class Stage {
  kOk,
  kClosed,     // the client hung up without sending a request
  kTimeout,    // nothing arrived before the receive timeout
  kRead,       // a read error occurred on the connection
  kWrite,      // a write error occurred on the connection
  kTooLarge,   // the header block hit kRequestMax, or --file hit its cap
  kMalformed,  // the request line is not one this server can parse
  kSocket,     // socket() or setsockopt() failed
  kBind,       // bind() failed
  kListen,     // listen() or getsockname() failed
  kAccept,     // accept() failed other than transiently
  kConnection, // an accepted connection could not be set up
  kOpen,       // the --file page could not be opened or read
  kNotRegular, // the --file page is not a regular file
  kNoMem,      // out of memory
};

// Returns a short human-readable label for a stage. These strings are part of
// the log, so they are shared with the other ports byte for byte.
std::string_view describe(Stage stage);

// Outcome of an operation the server performed on its own behalf. The
// error_code travels in the value rather than in errno, which is what makes the
// accept path safe: a global would change under any log write in between.
struct Result {
  Stage stage = Stage::kOk;
  std::error_code ec{};

  bool ok() const noexcept { return stage == Stage::kOk; }
  explicit operator bool() const noexcept { return ok(); }
};

// The methods routing distinguishes. kOther is not a parse failure: "POST" is
// well-formed, and that it is not served is routing's judgment, made as a 405.
enum class Method { kGet, kHead, kOther };

// A parsed request line. Every view points into the caller's block, which must
// outlive this; a string_view carries its own length, which is what lets a
// request containing a NUL be refused rather than silently truncated.
struct Request {
  Method method = Method::kOther;
  // The request target exactly as it arrived: not percent-decoded and not
  // normalized, since nothing here reaches the filesystem.
  std::string_view target;
  // The target up to the first '?', which is what routing matches on.
  std::string_view path;
  int major = 0;
  int minor = 0;
  // The whole request line, for the log. Sanitize it before writing it.
  std::string_view line;
};

// A response ready to be written. body views bytes the caller owns - the page
// for a 200, the caller's scratch for an error. allow is empty unless this is a
// 405.
struct Response {
  int status = 200;
  std::string_view reason;
  std::string_view body;
  std::string_view allow;
};

// Returns the reason phrase for a status this server sends, else "Unknown". It
// is part of the response bytes and of every error page's title.
std::string_view status_reason(int status);

// Returns the page compiled into the binary, served when there is no --file.
std::string_view builtin_page();

// Renders untrusted bytes safe to write to a log: every non-printable byte
// becomes '?', so a request line cannot clear a terminal or forge a second
// line. At most max characters; longer input is truncated and ends with "...".
std::string sanitize(std::string_view src, std::size_t max = kLogLineMax);

// Builds the response for one of 400, 404, 405, 431, 505, rendering the body
// into scratch - which must outlive it. Pure and infallible, which is what
// keeps route() the same, and never copies a --file page. Only a 405 allows.
Response error_response(int status, std::string &scratch);

// Parses the request line out of a header block. Pure: only the request line is
// looked at, so there is no Host check and no body. Returns nothing at all for
// anything the README's grammar refuses, NULs included, which is a 400.
std::optional<Request> parse_request(std::string_view block);

// Decides what to answer a well-formed request with, building error bodies in
// scratch. Pure and infallible - there is no 500 here. The check order is the
// contract: version, then method, then path.
Response route(const Request &req, std::string_view page, std::string &scratch);

// Reports the error behind a stream that stopped producing bytes. A streambuf
// cannot say "that was a failure, not end of input", so the socket layer keeps
// the last recv's error_code here. Stream-only tests leave it empty.
using ReadErrorProbe = std::function<std::error_code()>;

// Reads one header block from in into out, stopping at the blank line and never
// at end of input, and fills out even when the result is not kOk. Bytes past
// the terminator stay on the stream; see accept_once.
Stage read_request(std::istream &in, std::string &out,
                   std::size_t cap = kRequestMax,
                   const ReadErrorProbe &probe = {});

// Writes a response, header order fixed so the bytes are golden. suppress_body
// is true for a HEAD, which keeps the GET's headers, Content-Length included.
// Returns kOk, or kWrite.
Stage write_response(std::ostream &out, const Response &resp,
                     bool suppress_body);

// What one connection came to. left_unread is a field rather than the result
// because a 431 is a response, so that connection is kOk like any other - and
// the caller still has to know the rest of the request is on its way.
struct Transaction {
  Stage stage = Stage::kOk;
  bool left_unread = false;
};

// The whole transaction for one connection: read, parse, route, write, log.
// Streams and not a socket, so it is testable with std::istringstream; in and
// out are one SocketStream passed twice. Writes to log are best effort.
Transaction serve_connection(std::istream &in, std::ostream &out,
                             std::ostream &log, std::string_view page,
                             const ReadErrorProbe &probe = {});

} // namespace http_server

#endif // TINY_HTTP_SERVER_CPP_HTTP_HPP
