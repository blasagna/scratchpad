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

// Value of the Server header. A constant rather than a version string: there is
// nothing to version, and a constant keeps the response bytes golden.
inline constexpr std::string_view kServerName = "tiny_http_server";

// The only content type this server ever sends. Nothing is sniffed from a path
// or an extension, because --file means "serve this HTML instead of the
// built-in page" rather than "serve a file tree" - see load_page.
inline constexpr std::string_view kContentType = "text/html; charset=utf-8";

// Largest request header block accepted, in bytes. A request that reaches this
// without a terminator gets a 431 rather than being truncated: the alternative
// is parsing the first 8 KiB of an unbounded header block and answering as if
// the rest had not been sent.
inline constexpr std::size_t kRequestMax = 8192;

// Longest request line written to the log, after sanitizing. A request line may
// be nearly kRequestMax bytes and is entirely the client's to choose, so the
// log takes a bounded prefix of it and says so with an ellipsis.
//
// 255 and not 256: the C port spells this as a 256-byte destination buffer and
// spends one of those bytes on the NUL, so the longest line either port writes
// is 255 characters. The count is the shared contract; the buffer was only ever
// how C said it.
inline constexpr std::size_t kLogLineMax = 255;

// The stage at which an HTTP or socket operation failed. Anything other than
// kOk names what did not work; Result::ec carries the errno behind it where
// there was one.
//
// A request the server answers with a 4xx or 5xx is not one of these: the
// server did its job by answering. Those are a Response, and the connection
// they arrived on is kOk.
//
// Only kSocket, kBind, and kListen are fatal to the server, plus a
// non-transient kAccept and a failed page load at startup. Everything a client
// does is a per-connection event, because a client must not be able to end the
// server.
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

// Outcome of an operation the server performed on its own behalf.
//
// The error_code travels in the value rather than being left in errno for the
// caller to read. That is what makes the accept path safe: run() decides
// whether to end the server by looking at the failure behind an accept, and a
// global errno would be free to change under any log write in between.
struct Result {
  Stage stage = Stage::kOk;
  std::error_code ec{};

  bool ok() const noexcept { return stage == Stage::kOk; }
  explicit operator bool() const noexcept { return ok(); }
};

// The methods routing distinguishes.
//
// kOther is not a parse failure: "POST" is a perfectly well-formed method and
// the request carrying it is a perfectly well-formed request. That it is not
// one this server serves is routing's judgment to make, and it makes it as a
// 405 - which is why parsing does not reject it here.
enum class Method { kGet, kHead, kOther };

// A parsed request line.
//
// Every view points into the caller's block, which must therefore outlive the
// Request. A string_view carries its own length rather than stopping at a NUL,
// which is what lets a request containing a NUL byte be seen and refused rather
// than silently truncated into a valid one.
struct Request {
  Method method = Method::kOther;
  // The request target exactly as it arrived. Not percent-decoded and not
  // normalized: nothing here ever reaches the filesystem, so there is no path
  // to traverse and no decoding to get wrong.
  std::string_view target;
  // The target up to the first '?', which is what routing matches on.
  std::string_view path;
  int major = 0;
  int minor = 0;
  // The whole request line, for the log. Sanitize it before writing it.
  std::string_view line;
};

// A response ready to be written, as routing decided it.
//
// body views bytes the caller owns - the page for a 200, or the caller's
// scratch string for an error. allow is the value of the Allow header, and is
// empty when the response carries none; only a 405 does.
struct Response {
  int status = 200;
  std::string_view reason;
  std::string_view body;
  std::string_view allow;
};

// Returns the reason phrase for a status code this server can send, or
// "Unknown" for any other. The phrase is part of the response bytes and part of
// every error page's title, so it lives in one place.
std::string_view status_reason(int status);

// Returns the page compiled into the binary, served when there is no --file.
std::string_view builtin_page();

// Renders untrusted bytes safe to write to a log.
//
// The request line is a client's bytes going to somebody's terminal. Written
// raw, a line containing "\x1b[2J" clears the screen of whoever is watching the
// server and one containing "\n" forges a second log line. Every byte outside
// printable ASCII therefore becomes '?', including the escape itself - the
// bracket and the digits that follow it are harmless once the ESC is gone.
//
// At most max characters are returned; longer input is truncated and ends with
// "..." so the log says that it was, rather than silently showing a prefix as
// if it were the whole line.
std::string sanitize(std::string_view src, std::size_t max = kLogLineMax);

// Builds the response for a status with no page behind it.
//
// Pure: the body is rendered into the caller's scratch string rather than
// returned by value, so routing cannot fail, needs no out-of-memory path, and
// never copies a large --file page per request. An error carries a real HTML
// body rather than none, because an empty 404 renders as a blank page - or gets
// replaced by the browser's own error page, which looks exactly like a failure
// to connect - and shows nothing at all under curl.
//
// status is one of 400, 404, 405, 431, 505. Any other value is rendered with
// the reason "Unknown", which no caller should reach. The returned Response
// views scratch, which must therefore outlive it. A 405 carries "GET, HEAD" in
// allow; nothing else does.
Response error_response(int status, std::string &scratch);

// Reads the request line out of a header block.
//
// Pure. Only the request line is looked at; the header lines after it are
// ignored entirely, which is why there is no Host check (see README) and why a
// request body is never read.
//
// The line must be exactly three space-separated fields - method, target,
// version - and must be terminated. Two fields is HTTP/0.9, which this server
// does not speak; four means a target containing a space, which must be
// percent-encoded. The version token must match HTTP/<digit>.<digit> exactly,
// so "HTTP/1" and "http/1.1" are malformed rather than unsupported - whether a
// version is one we speak is routing's judgment, and this cannot even tell what
// version was meant.
//
// One leading empty line is skipped, which RFC 7230 3.5 recommends: a client
// that ends its previous request with an extra CRLF is common enough that
// rejecting it is worse than allowing it.
//
// Returns the request, or nothing at all - which the caller answers with a 400.
// std::optional rather than a Stage because there is exactly one way to fail. A
// NUL anywhere in the request line is one of those: the rest of the program
// treats the line as text, and the alternative is letting "GET /
// HTTP/1.1\0junk" look like an ordinary request.
std::optional<Request> parse_request(std::string_view block);

// Decides what to answer a well-formed request with.
//
// Pure, and infallible: there is no 500 in this server because nothing routing
// does can fail. The page was read at startup, so a request never touches the
// filesystem, and the error bodies are built in the caller's scratch.
//
// The order of the checks is the contract, not an implementation detail:
// version, then method, then path. A version we do not speak outranks a method
// we do not serve because a method belongs to a protocol - which is what makes
// an HTTP/2 preface ("PRI * HTTP/2.0") a 505 and not a 405.
//
// Returns 200 for GET or HEAD of "/" or "/index.html"; 505 for a major version
// other than 1; 405 with an Allow for any other method; 404 otherwise. A HEAD
// is routed exactly like a GET, body and all - suppressing the body is
// write_response's job, because a HEAD must report the Content-Length the GET
// would have had.
Response route(const Request &req, std::string_view page, std::string &scratch);

// Reports the error behind a stream that stopped producing bytes.
//
// A std::streambuf has no way to say "that was a failure, not end of input"
// through the standard interface, so the socket layer supplies this: it returns
// the error_code the last recv left behind, which is how a receive timeout
// (EAGAIN) is told apart from a real read failure. The stream-only tests leave
// it empty and get kRead from the stream's own badbit, the way mini_shell's
// tests inject one.
using ReadErrorProbe = std::function<std::error_code()>;

// Reads one request header block off a stream.
//
// Stops at the blank line that ends the header block, and never at end of
// input: a client holds the connection open after sending, so anything that
// reads to EOF - std::getline of the whole stream, operator>>, read_to_end -
// blocks until the timeout. The terminator is taken as the last three bytes
// being "\n\r\n" or the last two being "\n\n", which accepts all four spellings
// of a blank line while not firing on the CRLF that ends the request line
// itself. Lone LFs are not a hypothetical: nc, telnet, and hand-written scripts
// all send them.
//
// Bytes past the terminator are left on the stream unread. Those are a request
// body, which this server never reads - see accept_once for what happens to
// them.
//
// out receives the bytes read, terminator included, and is filled in even when
// the result is not kOk, so a caller can tell a client that sent nothing from
// one that sent half a request.
//
// Returns kOk, or kClosed (end of input, whether at the first byte or partway
// through - the first is what a browser's speculative connection looks like and
// is entirely ordinary), kTimeout, kRead, or kTooLarge. Only the last of those
// gets a response; there is nobody left to answer for the others.
Stage read_request(std::istream &in, std::string &out,
                   std::size_t cap = kRequestMax,
                   const ReadErrorProbe &probe = {});

// Writes a response's bytes.
//
// The header order is fixed - status line, Server, Content-Type,
// Content-Length, Connection, then Allow when there is one - so the bytes are
// golden and a test can assert on all of them at once.
//
// Content-Length is not optional. Letting the connection close delimit the body
// (which HTTP/1.0 allowed) makes a truncated response byte-identical to a
// complete one, so a client cannot tell the page from half the page and a
// crash. Connection: close is not optional either: this server speaks HTTP/1.1,
// where persistent connections are the default, and a browser that believed
// that would hold the socket open waiting for a second response - stalling
// every other client, since connections are served one at a time.
//
// suppress_body is true for a HEAD. The headers, Content-Length included, are
// byte-identical to the GET's; only the body is withheld. Setting body to empty
// for a HEAD instead is the obvious shortcut and is wrong: reporting the length
// the GET would have had is the entire reason the method exists.
//
// Returns kOk, or kWrite.
Stage write_response(std::ostream &out, const Response &resp,
                     bool suppress_body);

// What one connection came to.
//
// left_unread is a field rather than the result because a 431 is a response, so
// that connection is kOk like any other - and the caller still has to know that
// the rest of a much larger request is on its way before it closes.
struct Transaction {
  Stage stage = Stage::kOk;
  bool left_unread = false;
};

// The whole transaction for one connection.
//
// Reads the header block, parses it, routes it, writes the response, and logs
// what happened. Takes streams rather than a socket so the entire transaction
// is testable with std::istringstream and std::ostringstream, with no socket
// anywhere.
//
// Writes to log are best effort and unchecked. The log is stderr, and a server
// that exits because somebody closed its stderr is worse than one that keeps
// answering requests nobody is recording.
//
// in and out are the two directions of the connection, which for a socket are
// one SocketStream passed twice.
//
// Returns kOk when a response was written, whatever its status - a 404 is the
// server working. Otherwise the read or write failure, none of which ends the
// server.
Transaction serve_connection(std::istream &in, std::ostream &out,
                             std::ostream &log, std::string_view page,
                             const ReadErrorProbe &probe = {});

} // namespace http_server

#endif // TINY_HTTP_SERVER_CPP_HTTP_HPP
