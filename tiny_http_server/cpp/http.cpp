#include "http.hpp"

#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

namespace http_server {
namespace {

// The page served when there is no --file. Compiled in, not a data dependency:
// `bazel run` executes from the runfiles dir, so a relative path would resolve
// differently depending on how the program was started.
constexpr std::string_view kIndexHtml =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><title>tiny_http_server</title></head>\n"
    "<body><h1>Hello, world!</h1><p>Served by tiny_http_server.</p></body>\n"
    "</html>\n";

// The value of the Allow header on a 405, and the whole of what is allowed.
constexpr std::string_view kAllowedMethods = "GET, HEAD";

// The two paths the page is served at.
constexpr std::string_view kRootPath = "/";
constexpr std::string_view kIndexPath = "/index.html";

// What a truncated log line ends with, so a prefix is not shown as the whole.
constexpr std::string_view kEllipsis = "...";

// Steps past one leading empty line, per RFC 7230 3.5. Exactly one, so a block
// of blank lines stays malformed. Shared by the parser and the log, so the log
// cannot describe a different line than the one that was rejected.
std::string_view skip_one_blank_line(std::string_view block) {
  if (block.starts_with("\r\n"))
    return block.substr(2);
  if (block.starts_with("\n"))
    return block.substr(1);
  return block;
}

// The first line of block, not counting its terminator.
std::string_view first_line(std::string_view block) {
  std::string_view line = block.substr(0, block.find('\n'));
  if (line.ends_with('\r'))
    line.remove_suffix(1);
  return line;
}

// Spelled out rather than delegated to std::isdigit, which is locale-dependent
// in principle.
bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

bool version_shape_ok(std::string_view field) {
  return field.size() == 8 && field.starts_with("HTTP/") &&
         is_ascii_digit(field[5]) && field[6] == '.' &&
         is_ascii_digit(field[7]);
}

// Reports whether the request's path is one the page is served at.
bool path_is_served(const Request &req) {
  return req.path == kRootPath || req.path == kIndexPath;
}

} // namespace

std::string_view describe(Stage stage) {
  // No default, so -Wswitch names any enumerator added later; the fallthrough
  // is what a value cast in from outside the enum lands on.
  switch (stage) {
  case Stage::kOk:
    return "success";
  case Stage::kClosed:
    return "client closed the connection without sending a request";
  case Stage::kTimeout:
    return "client sent nothing before the read timeout";
  case Stage::kRead:
    return "error reading the request";
  case Stage::kWrite:
    return "error writing the response";
  case Stage::kTooLarge:
    return "request header block is too large";
  case Stage::kMalformed:
    return "malformed request";
  case Stage::kSocket:
    return "cannot create the listening socket";
  case Stage::kBind:
    return "cannot bind the listening socket";
  case Stage::kListen:
    return "cannot listen on the socket";
  case Stage::kAccept:
    return "cannot accept a connection";
  case Stage::kConnection:
    return "cannot set up the accepted connection";
  case Stage::kOpen:
    return "cannot read the page file";
  case Stage::kNotRegular:
    return "the page file is not a regular file";
  case Stage::kNoMem:
    return "out of memory";
  }
  return "unknown error";
}

std::string_view status_reason(int status) {
  // A switch over an int, so -Wswitch has nothing to check and the default is
  // what catches a status nobody sends.
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

std::string_view builtin_page() { return kIndexHtml; }

std::string sanitize(std::string_view src, std::size_t max) {
  std::size_t keep = src.size();
  std::size_t dots = 0;
  if (keep > max) {
    // The ellipsis lives inside max, displacing the last bytes rather than
    // pushing the line over; too small even for it keeps whatever fits.
    dots = max < kEllipsis.size() ? max : kEllipsis.size();
    keep = max - dots;
  }

  std::string out;
  out.reserve(keep + dots);
  for (std::size_t i = 0; i < keep; i++) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    // Printable ASCII passes, everything else becomes '?'. Dropping the ESC is
    // what defuses a sequence; what follows it is ordinary text.
    out.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
  }
  out.append(kEllipsis.substr(0, dots));
  return out;
}

Response error_response(int status, std::string &scratch) {
  Response resp;
  resp.status = status;
  resp.reason = status_reason(status);
  // Required by the RFC on a 405, and what tells a PUT what would have worked.
  resp.allow = status == 405 ? kAllowedMethods : std::string_view{};

  // One template, so adding a status is a row in status_reason and nothing
  // else.
  std::string head = std::to_string(status);
  head += ' ';
  head += resp.reason;

  scratch.clear();
  scratch += "<!DOCTYPE html>\n<html lang=\"en\">\n"
             "<head><meta charset=\"utf-8\"><title>";
  scratch += head;
  scratch += "</title></head>\n<body><h1>";
  scratch += head;
  scratch += "</h1></body>\n</html>\n";
  resp.body = scratch;
  return resp;
}

std::optional<Request> parse_request(std::string_view block) {
  std::string_view rest = skip_one_blank_line(block);

  // No terminator means the request line never ended, and a target cut off
  // midway is a different target.
  std::size_t nl = rest.find('\n');
  if (nl == std::string_view::npos)
    return std::nullopt;

  std::string_view line = rest.substr(0, nl);
  if (line.ends_with('\r'))
    line.remove_suffix(1);

  // A NUL is refused, not treated as a terminator: everything downstream reads
  // the line as text, so "GET / HTTP/1.1\0junk" would otherwise pass.
  if (line.find('\0') != std::string_view::npos)
    return std::nullopt;

  // Exactly three space-separated fields: two is HTTP/0.9, four means a space
  // in the target, which a client must percent-encode.
  std::size_t first = line.find(' ');
  if (first == std::string_view::npos)
    return std::nullopt;
  std::string_view method = line.substr(0, first);
  std::string_view tail = line.substr(first + 1);

  std::size_t second = tail.find(' ');
  if (second == std::string_view::npos)
    return std::nullopt;
  std::string_view target = tail.substr(0, second);
  std::string_view version = tail.substr(second + 1);

  if (version.find(' ') != std::string_view::npos)
    return std::nullopt;
  if (method.empty() || target.empty())
    return std::nullopt;

  // Malformed rather than unsupported: "HTTP/1" and "http/1.1" do not say
  // which version they meant, so there is nothing for routing to judge.
  if (!version_shape_ok(version))
    return std::nullopt;

  Request req;
  if (method == "GET")
    req.method = Method::kGet;
  else if (method == "HEAD")
    req.method = Method::kHead;
  else
    // Well-formed but not one we serve. Methods are case-sensitive, so "get"
    // lands here and becomes a 405 rather than being corrected.
    req.method = Method::kOther;

  // Verbatim: no request byte reaches the filesystem, so there is nothing to
  // decode for. Every target form is accepted here and sorted out by routing.
  req.target = target;
  req.path = target.substr(0, target.find('?'));
  req.major = version[5] - '0';
  req.minor = version[7] - '0';
  req.line = line;
  return req;
}

Response route(const Request &req, std::string_view page,
               std::string &scratch) {
  // Version before method, and that order is the contract: an HTTP/2 preface
  // ("PRI * HTTP/2.0") is a 505 and not a 405. The minor version is not looked
  // at, so HTTP/1.9 is a version 1 client and is served.
  if (req.major != 1)
    return error_response(505, scratch);
  if (req.method != Method::kGet && req.method != Method::kHead)
    return error_response(405, scratch);
  if (!path_is_served(req))
    return error_response(404, scratch);

  // A HEAD is routed exactly like a GET, body included: withholding it is
  // write_response's job, since the length must be the GET's.
  Response resp;
  resp.status = 200;
  resp.reason = status_reason(200);
  resp.body = page;
  return resp;
}

Stage read_request(std::istream &in, std::string &out, std::size_t cap,
                   const ReadErrorProbe &probe) {
  out.clear();

  for (;;) {
    char c = '\0';
    if (!in.get(c)) {
      // A streambuf can only say "no more bytes"; the probe answers whether
      // that was a clean close or a failed recv. Tests use the badbit instead.
      std::error_code ec = probe ? probe() : std::error_code{};
      if (ec == std::errc::resource_unavailable_try_again ||
          ec == std::errc::operation_would_block)
        return Stage::kTimeout;
      if (ec || in.bad())
        return Stage::kRead;
      // End of input. At the first byte this is a browser's speculative
      // connection, which is ordinary and gets its own stage.
      return Stage::kClosed;
    }

    if (out.size() == cap)
      return Stage::kTooLarge;
    out.push_back(c);

    // The blank line that ends the header block, in every spelling. Requiring a
    // '\n' before it keeps this off the request line's own CRLF.
    if (out.ends_with("\n\n") || out.ends_with("\n\r\n"))
      break;
  }

  // Whatever follows stays unread. It is a request body, and leaving it there
  // is why the close is a shutdown plus a drain rather than a bare close.
  return Stage::kOk;
}

Stage write_response(std::ostream &out, const Response &resp,
                     bool suppress_body) {
  // The header order is fixed so the response bytes are golden and one
  // assertion can cover all of them.
  out << "HTTP/1.1 " << resp.status << ' ' << resp.reason << "\r\n"
      << "Server: " << kServerName << "\r\n"
      << "Content-Type: " << kContentType
      << "\r\n"
      // Always the full body length, even for a HEAD.
      << "Content-Length: " << resp.body.size() << "\r\n"
      << "Connection: close\r\n";
  if (!resp.allow.empty())
    out << "Allow: " << resp.allow << "\r\n";
  out << "\r\n";

  if (!suppress_body && !resp.body.empty())
    out.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));

  // Flushed here: the response must be on the socket before the shutdown, and
  // a buffered stream reports a failed write at the flush.
  out.flush();
  return out.good() ? Stage::kOk : Stage::kWrite;
}

Transaction serve_connection(std::istream &in, std::ostream &out,
                             std::ostream &log, std::string_view page,
                             const ReadErrorProbe &probe) {
  Transaction tx;
  std::string block;
  std::string scratch;

  Stage read_stage = read_request(in, block, kRequestMax, probe);
  // Every read failure but one leaves nobody to answer. kTooLarge is the
  // exception: that client is still there and is owed a 431.
  if (read_stage != Stage::kOk && read_stage != Stage::kTooLarge) {
    log << kProgName << ": " << describe(read_stage) << "\n";
    tx.stage = read_stage;
    return tx;
  }

  Response resp;
  bool suppress_body = false;
  if (read_stage == Stage::kTooLarge) {
    log << kProgName << ": request header block over " << kRequestMax
        << " bytes\n";
    // The rest of that request is still on its way, and the 431 below is worth
    // nothing if the close beats it there.
    tx.left_unread = true;
    resp = error_response(431, scratch);
  } else if (std::optional<Request> req = parse_request(block)) {
    log << kProgName << ": request " << sanitize(req->line) << "\n";
    resp = route(*req, page, scratch);
    suppress_body = req->method == Method::kHead;
  } else {
    // Sanitized and quoted, so an empty line is visible as one. Located the way
    // the parser locates it: the raw first line would log `malformed request
    // ""` for a request that opened with a stray CRLF.
    log << kProgName << ": malformed request \""
        << sanitize(first_line(skip_one_blank_line(block))) << "\"\n";
    resp = error_response(400, scratch);
  }

  Stage written = write_response(out, resp, suppress_body);
  if (written != Stage::kOk) {
    log << kProgName << ": " << describe(written) << "\n";
    tx.stage = written;
    return tx;
  }

  // The count is what went out, so a HEAD reports 0: the log records the wire,
  // the header records the resource.
  log << kProgName << ": response " << resp.status << ' ' << resp.reason << " ("
      << (suppress_body ? std::size_t{0} : resp.body.size()) << " bytes)\n";
  return tx;
}

} // namespace http_server
