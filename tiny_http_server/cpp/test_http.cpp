#include <gtest/gtest.h>

#include <cerrno>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "http.hpp"

namespace {

using http_server::Method;
using http_server::Request;
using http_server::Response;
using http_server::Stage;

// The built-in page's size, spelled out so a change to the markup has to be a
// deliberate edit here rather than a golden that quietly follows it.
constexpr std::size_t kPageLen = 178;

// The bytes an ordinary request produces, as a whole header block.
std::string request(std::string_view line) {
  return std::string(line) + "\r\n\r\n";
}

// Parses a request line and routes it, which is what every routing assertion
// below actually cares about. Fails the test if the line does not parse.
Response routed(const std::string &raw, std::string_view page,
                std::string &scratch) {
  std::optional<Request> req = http_server::parse_request(raw);
  EXPECT_TRUE(req.has_value()) << "expected '" << raw << "' to parse";
  if (!req)
    // A 500 is not a status this server can send, so it cannot be confused with
    // a real answer if the assertion above was non-fatal.
    return Response{500, "", {}, {}};
  return http_server::route(*req, page, scratch);
}

// --- read_request --------------------------------------------------------

TEST(ReadRequest, StopsAtTheBlankLineAndLeavesTheRestUnread) {
  std::istringstream in("GET / HTTP/1.1\r\nHost: x\r\n\r\nBODYBYTES");
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block), Stage::kOk);
  EXPECT_EQ(block, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  // The body is still there for whoever wants it, which nothing here does.
  std::string rest;
  in >> rest;
  EXPECT_EQ(rest, "BODYBYTES");
}

TEST(ReadRequest, AcceptsLoneLfLineEndings) {
  std::string data = "GET / HTTP/1.1\n\n";
  std::istringstream in(data);
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block), Stage::kOk);
  EXPECT_EQ(block, data);
}

TEST(ReadRequest, AcceptsAMixedCrlfAndLfTerminator) {
  for (std::string data : {"GET / HTTP/1.1\r\n\n", "GET / HTTP/1.1\n\r\n"}) {
    std::istringstream in(data);
    std::string block;
    EXPECT_EQ(http_server::read_request(in, block), Stage::kOk) << data;
    EXPECT_EQ(block, data) << data;
  }
}

TEST(ReadRequest, DoesNotStopAtTheEndOfTheRequestLine) {
  // The whole point of requiring a '\n' before the terminator: the CRLF ending
  // the request line must not look like a blank line.
  std::string data = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  std::istringstream in(data);
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block), Stage::kOk);
  EXPECT_EQ(block, data);
}

TEST(ReadRequest, ReportsTheClientClosedBeforeAnyBytes) {
  // A browser's speculative connection. The ordinary case, not an error.
  std::istringstream in("");
  std::string block = "stale";
  EXPECT_EQ(http_server::read_request(in, block), Stage::kClosed);
  EXPECT_EQ(block, "");
}

TEST(ReadRequest, ReportsTheClientClosedMidHeader) {
  std::istringstream in("GET / HTT");
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block), Stage::kClosed);
  EXPECT_EQ(block, "GET / HTT");
}

TEST(ReadRequest, ReportsTooLargeAtTheCap) {
  std::istringstream in(std::string(64, 'x'));
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block, 16), Stage::kTooLarge);
  EXPECT_EQ(block.size(), 16u);
}

TEST(ReadRequest, AcceptsExactlyTheCap) {
  // Sixteen bytes ending in a blank line: the last byte that fits is also the
  // one that completes the terminator.
  std::string data = "GET / HT/1.1\r\n\r\n";
  ASSERT_EQ(data.size(), 16u);
  std::istringstream in(data);
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block, 16), Stage::kOk);
  EXPECT_EQ(block.size(), 16u);
}

TEST(ReadRequest, KeepsNulBytesAndReportsTheTrueLength) {
  std::string data("GET /\0x HTTP/1.1\r\n\r\n", 20);
  std::istringstream in(data);
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block), Stage::kOk);
  // A std::string carries its length rather than stopping at the NUL, so the
  // byte survives to the parser, which refuses it.
  EXPECT_EQ(block.size(), 20u);
  EXPECT_EQ(block, data);
}

TEST(ReadRequest, ReportsAReadError) {
  // A stream that has already gone bad fails every read, which is how a read
  // error is arranged without a socket.
  std::istringstream in("GET / HTTP/1.1\r\n\r\n");
  in.setstate(std::ios::badbit);
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block), Stage::kRead);
}

TEST(ReadRequest, ReportsATimeoutWhenTheProbeSaysTheReadWouldBlock) {
  // SO_RCVTIMEO surfaces as EAGAIN on the underlying recv. A streambuf has no
  // way to say "failure, not end of input", so the probe is what carries it -
  // and this is the one case that would otherwise be logged as an ordinary
  // hang-up.
  std::istringstream in("");
  std::string block;
  EXPECT_EQ(http_server::read_request(
                in, block, http_server::kRequestMax,
                [] {
                  return std::make_error_code(
                      std::errc::resource_unavailable_try_again);
                }),
            Stage::kTimeout);
}

TEST(ReadRequest, ReportsAReadErrorWhenTheProbeSaysAnythingElse) {
  std::istringstream in("");
  std::string block;
  EXPECT_EQ(http_server::read_request(
                in, block, http_server::kRequestMax,
                [] { return std::make_error_code(std::errc::io_error); }),
            Stage::kRead);
}

TEST(ReadRequest, ReportsAClosedConnectionWhenTheProbeHasNothingToReport) {
  // A clean close leaves no error behind, which is what separates the browser
  // preconnect from the two cases above.
  std::istringstream in("");
  std::string block;
  EXPECT_EQ(http_server::read_request(in, block, http_server::kRequestMax,
                                      [] { return std::error_code{}; }),
            Stage::kClosed);
}

// --- parse_request -------------------------------------------------------

TEST(ParseRequest, AcceptsAMinimalGetLine) {
  // The block is a named local, not a temporary: a Request is all views into it
  // and nothing is copied, so the bytes have to outlive the parse. That is the
  // header's contract and it is easy to break by accident here.
  std::string raw = request("GET / HTTP/1.1");
  std::optional<Request> req = http_server::parse_request(raw);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->method, Method::kGet);
  EXPECT_EQ(req->target, "/");
  EXPECT_EQ(req->path, "/");
  EXPECT_EQ(req->major, 1);
  EXPECT_EQ(req->minor, 1);
  EXPECT_EQ(req->line, "GET / HTTP/1.1");
}

TEST(ParseRequest, SplitsThePathFromTheQuery) {
  std::string raw = request("GET /index.html?a=1&b=2 HTTP/1.1");
  std::optional<Request> req = http_server::parse_request(raw);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->target, "/index.html?a=1&b=2");
  EXPECT_EQ(req->path, "/index.html");
}

TEST(ParseRequest, KeepsTheTargetVerbatim) {
  // Not percent-decoded: nothing here reaches the filesystem, so there is no
  // path to normalize and no decoding to get wrong.
  std::string raw = request("GET /a%20b/../c HTTP/1.1");
  std::optional<Request> req = http_server::parse_request(raw);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->target, "/a%20b/../c");
}

TEST(ParseRequest, RecordsHeadAsTheMethod) {
  std::optional<Request> req =
      http_server::parse_request(request("HEAD / HTTP/1.1"));
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->method, Method::kHead);
}

TEST(ParseRequest, RecordsAnUnknownMethodAndLeavesTheJudgingToRouting) {
  for (std::string_view method : {"POST", "PUT", "DELETE", "get", "OPTIONS"}) {
    std::optional<Request> req = http_server::parse_request(
        request(std::string(method) + " / HTTP/1.1"));
    ASSERT_TRUE(req.has_value()) << method;
    EXPECT_EQ(req->method, Method::kOther) << method;
  }
}

TEST(ParseRequest, RecordsTheVersionNumbers) {
  std::optional<Request> req =
      http_server::parse_request(request("GET / HTTP/1.0"));
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->major, 1);
  EXPECT_EQ(req->minor, 0);
}

TEST(ParseRequest, SkipsOneLeadingEmptyLine) {
  for (std::string_view prefix : {"\r\n", "\n"}) {
    std::string raw = std::string(prefix) + request("GET / HTTP/1.1");
    EXPECT_TRUE(http_server::parse_request(raw).has_value()) << prefix;
  }
  // Exactly one, so a block of nothing but blank lines stays malformed.
  EXPECT_FALSE(http_server::parse_request("\r\n\r\n\r\n").has_value());
}

TEST(ParseRequest, RejectsAnEmptyRequest) {
  EXPECT_FALSE(http_server::parse_request("").has_value());
}

TEST(ParseRequest, RejectsARequestLineWithTwoFields) {
  // HTTP/0.9, which this server does not speak.
  EXPECT_FALSE(http_server::parse_request(request("GET /")).has_value());
}

TEST(ParseRequest, RejectsARequestLineWithFourFields) {
  // A space in a target has to be percent-encoded.
  EXPECT_FALSE(
      http_server::parse_request(request("GET /a b HTTP/1.1")).has_value());
}

TEST(ParseRequest, RejectsAnEmptyMethodOrTarget) {
  for (std::string_view line : {" / HTTP/1.1", "GET  HTTP/1.1"}) {
    EXPECT_FALSE(http_server::parse_request(request(line)).has_value()) << line;
  }
}

TEST(ParseRequest, RejectsARequestWithNoLineTerminator) {
  EXPECT_FALSE(http_server::parse_request("GET / HTTP/1.1").has_value());
}

TEST(ParseRequest, RejectsAMalformedVersionToken) {
  // Not "unsupported": none of these says which version it meant, so there is
  // nothing for routing to judge and the answer is a 400 rather than a 505.
  for (std::string_view version :
       {"HTTP/1", "HTTP1.1", "http/1.1", "HTTP/x.y", "HTTP/11.1", "HTTP/1.",
        "HTTP/.1", "", "HTTP/1.1x"}) {
    std::string raw = request("GET / " + std::string(version));
    EXPECT_FALSE(http_server::parse_request(raw).has_value())
        << "version '" << version << "'";
  }
}

TEST(ParseRequest, RejectsANulByteInTheRequestLine) {
  std::string raw("GET /\0x HTTP/1.1\r\n\r\n", 20);
  EXPECT_FALSE(http_server::parse_request(raw).has_value());
}

TEST(ParseRequest, AcceptsAnAbsurdlyLongTargetWithinTheCap) {
  // It parses and becomes an ordinary 404, which is what pins the decision not
  // to implement 414.
  std::string raw = request("GET /" + std::string(4000, 'a') + " HTTP/1.1");
  std::optional<Request> req = http_server::parse_request(raw);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->target.size(), 4001u);
}

TEST(ParseRequest, AcceptsATargetThatIsNotAnOriginPath) {
  // Absolute-form and asterisk-form are legal targets a server must accept.
  // Routing 404s them; rejecting them here would 400 a request from a proxy and
  // would make the version check unreachable for an HTTP/2 preface.
  for (std::string_view target :
       {"http://example.com/", "*", "example.com:80"}) {
    std::string raw = request("GET " + std::string(target) + " HTTP/1.1");
    EXPECT_TRUE(http_server::parse_request(raw).has_value()) << target;
  }
}

TEST(ParseRequest, IgnoresEverythingAfterTheRequestLine) {
  std::optional<Request> req = http_server::parse_request(
      "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 999\r\nJunk\r\n\r\n");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->line, "GET / HTTP/1.1");
}

// --- route ---------------------------------------------------------------

TEST(Route, SlashServesTheConfiguredPage) {
  std::string scratch;
  std::string_view page = http_server::builtin_page();
  Response r = routed(request("GET / HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.reason, "OK");
  EXPECT_EQ(r.body, page);
  EXPECT_EQ(r.body.size(), kPageLen);
  EXPECT_TRUE(r.allow.empty());
}

TEST(Route, IndexHtmlIsTheSamePage) {
  std::string scratch;
  Response r = routed(request("GET /index.html HTTP/1.1"),
                      http_server::builtin_page(), scratch);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body.size(), kPageLen);
}

TEST(Route, AQueryStringDoesNotChangeTheRoute) {
  std::string scratch;
  EXPECT_EQ(routed(request("GET /?x=1 HTTP/1.1"), http_server::builtin_page(),
                   scratch)
                .status,
            200);
}

TEST(Route, HeadRoutesLikeGetAndKeepsTheLength) {
  // Withholding the body is the writer's job; the length a HEAD reports is the
  // one the GET would have sent.
  std::string scratch;
  Response r =
      routed(request("HEAD / HTTP/1.1"), http_server::builtin_page(), scratch);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body.size(), kPageLen);
}

TEST(Route, AnUnknownPathIsNotFound) {
  std::string scratch;
  for (std::string_view target :
       {"/nope", "/index.htm", "/index.html/", "//", "http://example.com/"}) {
    Response r = routed(request("GET " + std::string(target) + " HTTP/1.1"),
                        http_server::builtin_page(), scratch);
    EXPECT_EQ(r.status, 404) << target;
  }
}

TEST(Route, FaviconIsAnOrdinaryNotFound) {
  // Every browser asks for it unprompted, so this line is the log's most
  // common. It is intentional, not a bug to special-case away.
  std::string scratch;
  EXPECT_EQ(routed(request("GET /favicon.ico HTTP/1.1"),
                   http_server::builtin_page(), scratch)
                .status,
            404);
}

TEST(Route, PercentEncodedIndexIsNotDecoded) {
  std::string scratch;
  EXPECT_EQ(routed(request("GET /%69ndex.html HTTP/1.1"),
                   http_server::builtin_page(), scratch)
                .status,
            404);
}

TEST(Route, LowercaseGetIsNotAMethod) {
  // Methods are case-sensitive. Correcting this would answer a request the
  // client did not send.
  std::string scratch;
  EXPECT_EQ(
      routed(request("get / HTTP/1.1"), http_server::builtin_page(), scratch)
          .status,
      405);
}

TEST(Route, PostIsMethodNotAllowedAndNamesTheAllowedMethods) {
  std::string scratch;
  Response r =
      routed(request("POST / HTTP/1.1"), http_server::builtin_page(), scratch);
  EXPECT_EQ(r.status, 405);
  EXPECT_EQ(r.allow, "GET, HEAD");
}

TEST(Route, AnUnsupportedMajorVersionIsFiveOhFive) {
  std::string scratch;
  for (std::string_view version : {"HTTP/2.0", "HTTP/0.9", "HTTP/9.9"}) {
    Response r = routed(request("GET / " + std::string(version)),
                        http_server::builtin_page(), scratch);
    EXPECT_EQ(r.status, 505) << version;
  }
}

TEST(Route, AMinorVersionIsCompatible) {
  std::string scratch;
  for (std::string_view version : {"HTTP/1.0", "HTTP/1.1", "HTTP/1.9"}) {
    Response r = routed(request("GET / " + std::string(version)),
                        http_server::builtin_page(), scratch);
    EXPECT_EQ(r.status, 200) << version;
  }
}

TEST(Route, TheVersionOutranksTheMethod) {
  // An HTTP/2 preface. A method belongs to a protocol, so a version we do not
  // speak leaves nothing to judge the method against.
  std::string scratch;
  EXPECT_EQ(
      routed(request("PRI * HTTP/2.0"), http_server::builtin_page(), scratch)
          .status,
      505);
}

TEST(Route, EveryErrorCarriesAnHtmlBody) {
  std::string scratch;
  for (std::string_view line :
       {"GET /nope HTTP/1.1", "POST / HTTP/1.1", "GET / HTTP/2.0"}) {
    Response r = routed(request(line), http_server::builtin_page(), scratch);
    EXPECT_FALSE(r.body.empty()) << line;
    EXPECT_TRUE(r.body.starts_with("<!DOCTYPE html>")) << line;
  }
}

TEST(Route, ServesTheConfiguredPageRatherThanTheBuiltInOne) {
  std::string scratch;
  std::string_view page = "<p>from --file</p>";
  Response r = routed(request("GET / HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.body, page);
}

// --- error_response ------------------------------------------------------

TEST(ErrorResponse, NamesEveryStatusItCanSend) {
  struct Case {
    int status;
    std::string_view reason;
  };
  constexpr Case kCases[] = {{400, "Bad Request"},
                             {404, "Not Found"},
                             {405, "Method Not Allowed"},
                             {431, "Request Header Fields Too Large"},
                             {505, "HTTP Version Not Supported"}};
  std::string scratch;
  for (const Case &c : kCases) {
    Response r = http_server::error_response(c.status, scratch);
    EXPECT_EQ(r.status, c.status);
    EXPECT_EQ(r.reason, c.reason);
    // The status and reason appear twice: once in the title, once in the h1.
    std::string title = "<title>" + std::to_string(c.status) + " " +
                        std::string(c.reason) + "</title>";
    EXPECT_NE(std::string(r.body).find(title), std::string::npos) << c.status;
  }
}

TEST(ErrorResponse, OnlyMethodNotAllowedCarriesAnAllow) {
  std::string scratch;
  EXPECT_TRUE(http_server::error_response(404, scratch).allow.empty());
  EXPECT_EQ(http_server::error_response(405, scratch).allow, "GET, HEAD");
}

TEST(ErrorResponse, TheLongestPageIsTheOneWithTheLongestReason) {
  // 431 has the longest reason phrase, so it is the largest page this can
  // build. The C port sizes a fixed scratch buffer against it; here the string
  // grows, and the number is still worth pinning because it is the
  // Content-Length the contract tabulates.
  std::string scratch;
  EXPECT_EQ(http_server::error_response(431, scratch).body.size(), 185u);
  EXPECT_EQ(http_server::error_response(400, scratch).body.size(), 145u);
  EXPECT_EQ(http_server::error_response(404, scratch).body.size(), 141u);
  EXPECT_EQ(http_server::error_response(405, scratch).body.size(), 159u);
  EXPECT_EQ(http_server::error_response(505, scratch).body.size(), 175u);
}

// --- write_response ------------------------------------------------------

TEST(WriteResponse, WritesTheExactBytesOfAHelloWorldPage) {
  std::string scratch;
  Response r =
      routed(request("GET / HTTP/1.1"), http_server::builtin_page(), scratch);
  std::ostringstream out;
  EXPECT_EQ(http_server::write_response(out, r, false), Stage::kOk);
  EXPECT_EQ(
      out.str(),
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
}

TEST(WriteResponse, ContentLengthCountsTheBodyBytes) {
  Response r{200, "OK", "abcde", {}};
  std::ostringstream out;
  EXPECT_EQ(http_server::write_response(out, r, false), Stage::kOk);
  EXPECT_NE(out.str().find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_TRUE(out.str().ends_with("abcde"));
}

TEST(WriteResponse, SuppressesTheBodyForHeadButKeepsTheLength) {
  std::string scratch;
  Response r =
      routed(request("HEAD / HTTP/1.1"), http_server::builtin_page(), scratch);
  std::ostringstream head_out;
  std::ostringstream get_out;
  EXPECT_EQ(http_server::write_response(head_out, r, true), Stage::kOk);
  EXPECT_EQ(http_server::write_response(get_out, r, false), Stage::kOk);
  std::string head = head_out.str();
  std::string get = get_out.str();
  // Byte-identical headers, Content-Length included; only the body differs.
  EXPECT_EQ(get.substr(0, head.size()), head);
  EXPECT_EQ(get.size(), head.size() + kPageLen);
  EXPECT_NE(head.find("Content-Length: 178\r\n"), std::string::npos);
}

TEST(WriteResponse, EmitsAllowOnlyOnAMethodNotAllowed) {
  std::string scratch;
  std::ostringstream with;
  Response allowed = http_server::error_response(405, scratch);
  EXPECT_EQ(http_server::write_response(with, allowed, false), Stage::kOk);
  EXPECT_NE(with.str().find("Connection: close\r\nAllow: GET, HEAD\r\n\r\n"),
            std::string::npos);

  std::ostringstream without;
  Response missing = http_server::error_response(404, scratch);
  EXPECT_EQ(http_server::write_response(without, missing, false), Stage::kOk);
  EXPECT_EQ(without.str().find("Allow:"), std::string::npos);
}

TEST(WriteResponse, UsesCrlfEverywhereAndABlankLineBeforeTheBody) {
  std::string scratch;
  Response r = http_server::error_response(404, scratch);
  std::ostringstream stream;
  EXPECT_EQ(http_server::write_response(stream, r, false), Stage::kOk);
  std::string out = stream.str();
  std::size_t blank = out.find("\r\n\r\n");
  ASSERT_NE(blank, std::string::npos);
  // No bare LF anywhere in the header block. The body has plenty.
  for (std::size_t i = 0; i < blank + 4; i++) {
    if (out[i] == '\n') {
      EXPECT_EQ(out[i - 1], '\r') << "bare LF at offset " << i;
    }
  }
}

TEST(WriteResponse, WritesABodyContainingNulBytes) {
  // --file takes whatever bytes are in the file, and Content-Length rather than
  // a NUL is what says where the body ends.
  Response r{200, "OK", std::string_view("a\0b", 3), {}};
  std::ostringstream out;
  EXPECT_EQ(http_server::write_response(out, r, false), Stage::kOk);
  EXPECT_EQ(out.str().substr(out.str().size() - 3), std::string("a\0b", 3));
  EXPECT_NE(out.str().find("Content-Length: 3\r\n"), std::string::npos);
}

TEST(WriteResponse, ReportsAWriteError) {
  std::string scratch;
  Response r = http_server::error_response(404, scratch);
  std::ostringstream out;
  out.setstate(std::ios::badbit);
  EXPECT_EQ(http_server::write_response(out, r, false), Stage::kWrite);
}

// --- sanitize ------------------------------------------------------------

TEST(Sanitize, PassesPrintableAsciiThrough) {
  std::string_view src = "GET /index.html?a=1 HTTP/1.1";
  EXPECT_EQ(http_server::sanitize(src), src);
}

TEST(Sanitize, ReplacesControlBytesAndHighBytes) {
  // Dropping the ESC is what defuses the sequence; what follows is ordinary.
  EXPECT_EQ(http_server::sanitize("GE\x1b[2JT \xff/"), "GE?[2JT ?/");
}

TEST(Sanitize, ReplacesANulByte) {
  EXPECT_EQ(http_server::sanitize(std::string_view("a\0b", 3)), "a?b");
}

TEST(Sanitize, ReplacesANewlineSoALogLineCannotBeForged) {
  std::string out = http_server::sanitize("GET /\ntiny_http_server: forged");
  EXPECT_EQ(out.find('\n'), std::string::npos);
}

TEST(Sanitize, TruncatesWithAnEllipsis) {
  EXPECT_EQ(http_server::sanitize("abcdefghijklmnop", 7), "abcd...");
}

TEST(Sanitize, HandlesAZeroLengthLimit) {
  EXPECT_EQ(http_server::sanitize("abc", 0), "");
}

TEST(Sanitize, KeepsWhatFitsOfTheEllipsisWhenTheLimitIsTiny) {
  // The ellipsis displaces the last bytes rather than pushing the line over the
  // limit, so a limit smaller than the ellipsis is all ellipsis.
  EXPECT_EQ(http_server::sanitize("abcdef", 2), "..");
}

// --- serve_connection ----------------------------------------------------

// Runs one whole transaction over in-memory streams and hands back both what
// went to the client and what went to the log.
struct Served {
  http_server::Transaction tx;
  std::string response;
  std::string log;
};

Served serve(const std::string &input, std::string_view page) {
  Served s;
  std::istringstream in(input);
  std::ostringstream out;
  std::ostringstream log;
  s.tx = http_server::serve_connection(in, out, log, page);
  s.response = out.str();
  s.log = log.str();
  return s;
}

TEST(ServeConnection, AnswersTheDefaultPathWithHelloWorld) {
  Served s = serve(request("GET / HTTP/1.1"), http_server::builtin_page());
  EXPECT_EQ(s.tx.stage, Stage::kOk);
  EXPECT_TRUE(s.response.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_NE(s.response.find("<h1>Hello, world!</h1>"), std::string::npos);
}

TEST(ServeConnection, LogsTheRequestLineAndTheResponse) {
  Served s = serve(request("GET / HTTP/1.1"), http_server::builtin_page());
  EXPECT_EQ(s.log, "tiny_http_server: request GET / HTTP/1.1\n"
                   "tiny_http_server: response 200 OK (178 bytes)\n");
}

TEST(ServeConnection, LogsAMalformedRequestSanitizedAndQuoted) {
  Served s = serve("GE\x1bT /\r\n\r\n", http_server::builtin_page());
  EXPECT_EQ(s.log, "tiny_http_server: malformed request \"GE?T /\"\n"
                   "tiny_http_server: response 400 Bad Request (145 bytes)\n");
  EXPECT_EQ(s.log.find('\x1b'), std::string::npos);
}

TEST(ServeConnection, AnswersFourOhFourForAnUnknownPath) {
  Served s = serve(request("GET /nope HTTP/1.1"), http_server::builtin_page());
  EXPECT_EQ(s.tx.stage, Stage::kOk);
  EXPECT_TRUE(s.response.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_NE(s.log.find("response 404 Not Found (141 bytes)"),
            std::string::npos);
}

TEST(ServeConnection, AnswersFourOhFiveWithAllowForPost) {
  Served s = serve(request("POST / HTTP/1.1"), http_server::builtin_page());
  EXPECT_TRUE(s.response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
  EXPECT_NE(s.response.find("Allow: GET, HEAD\r\n"), std::string::npos);
}

TEST(ServeConnection, AnswersFourHundredForAMalformedRequestLine) {
  Served s = serve("garbage\r\n\r\n", http_server::builtin_page());
  EXPECT_EQ(s.tx.stage, Stage::kOk);
  EXPECT_TRUE(s.response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST(ServeConnection, AnswersFiveOhFiveForAnUnsupportedVersion) {
  Served s = serve(request("GET / HTTP/2.0"), http_server::builtin_page());
  EXPECT_TRUE(
      s.response.starts_with("HTTP/1.1 505 HTTP Version Not Supported\r\n"));
}

TEST(ServeConnection, AnswersFourThirtyOneWhenTheHeaderBlockIsTooLarge) {
  // A client that is still there and is owed an answer, unlike every other read
  // failure.
  std::string huge =
      "GET / HTTP/1.1\r\n" + std::string(http_server::kRequestMax, 'x');
  Served s = serve(huge, http_server::builtin_page());
  EXPECT_EQ(s.tx.stage, Stage::kOk);
  EXPECT_TRUE(s.response.starts_with(
      "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
  EXPECT_NE(s.log.find("request header block over 8192 bytes"),
            std::string::npos);
  // The rest of that request is still arriving, which is what tells accept_once
  // to wait for it rather than close on top of it and turn the 431 into a
  // connection reset.
  EXPECT_TRUE(s.tx.left_unread);
}

TEST(ServeConnection, LeavesNothingUnreadOnAnOrdinaryRequest) {
  Served s = serve(request("GET / HTTP/1.1"), http_server::builtin_page());
  EXPECT_FALSE(s.tx.left_unread);
}

TEST(ServeConnection, LogsTheRequestLineAfterALeadingBlankLine) {
  // RFC 7230 3.5's stray CRLF, on a request that is then malformed. Logging the
  // raw first line would report the blank one, hiding the bytes that actually
  // caused the 400.
  Served s = serve("\r\nGET /x HTTP/9\r\n\r\n", http_server::builtin_page());
  EXPECT_TRUE(s.response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
  EXPECT_NE(s.log.find("malformed request \"GET /x HTTP/9\""),
            std::string::npos);
}

TEST(ServeConnection, SuppressesTheBodyForHead) {
  Served s = serve(request("HEAD / HTTP/1.1"), http_server::builtin_page());
  EXPECT_NE(s.response.find("Content-Length: 178\r\n"), std::string::npos);
  EXPECT_EQ(s.response.find("Hello, world!"), std::string::npos);
  // The log records what went on the wire, which for a HEAD is no body.
  EXPECT_NE(s.log.find("response 200 OK (0 bytes)"), std::string::npos);
}

TEST(ServeConnection, WritesNothingWhenTheClientSendsNothing) {
  // The browser-preconnect case: there is nobody to answer.
  Served s = serve("", http_server::builtin_page());
  EXPECT_EQ(s.tx.stage, Stage::kClosed);
  EXPECT_EQ(s.response, "");
  EXPECT_EQ(s.log, "tiny_http_server: client closed the connection without "
                   "sending a request\n");
}

TEST(ServeConnection, WritesNothingWhenTheClientClosesMidHeader) {
  Served s = serve("GET / HTTP/1.1\r\n", http_server::builtin_page());
  EXPECT_EQ(s.tx.stage, Stage::kClosed);
  EXPECT_EQ(s.response, "");
}

TEST(ServeConnection, ReportsATimeoutThroughTheProbe) {
  std::istringstream in("");
  std::ostringstream out;
  std::ostringstream log;
  http_server::Transaction tx = http_server::serve_connection(
      in, out, log, http_server::builtin_page(), [] {
        return std::make_error_code(std::errc::resource_unavailable_try_again);
      });
  EXPECT_EQ(tx.stage, Stage::kTimeout);
  EXPECT_EQ(out.str(), "");
  EXPECT_EQ(log.str(),
            "tiny_http_server: client sent nothing before the read timeout\n");
}

TEST(ServeConnection, ReportsAWriteFailureToTheLog) {
  std::istringstream in(request("GET / HTTP/1.1"));
  std::ostringstream out;
  out.setstate(std::ios::badbit);
  std::ostringstream log;
  http_server::Transaction tx =
      http_server::serve_connection(in, out, log, http_server::builtin_page());
  EXPECT_EQ(tx.stage, Stage::kWrite);
  EXPECT_NE(log.str().find("error writing the response"), std::string::npos);
}

TEST(ServeConnection, ServesTheFilePageWhenOneIsConfigured) {
  std::string_view page = "<p>from --file</p>";
  Served s = serve(request("GET / HTTP/1.1"), page);
  EXPECT_NE(s.response.find("Content-Length: 18\r\n"), std::string::npos);
  EXPECT_TRUE(s.response.ends_with(page));
}

// --- describe ------------------------------------------------------------

TEST(Describe, LabelsEveryStage) {
  constexpr Stage kAll[] = {
      Stage::kOk,    Stage::kClosed,     Stage::kTimeout,   Stage::kRead,
      Stage::kWrite, Stage::kTooLarge,   Stage::kMalformed, Stage::kSocket,
      Stage::kBind,  Stage::kListen,     Stage::kAccept,    Stage::kConnection,
      Stage::kOpen,  Stage::kNotRegular, Stage::kNoMem};
  // -Wswitch catches a new enumerator in the implementation; this catches one
  // that was never added here.
  for (Stage s : kAll)
    EXPECT_NE(http_server::describe(s), "unknown error") << static_cast<int>(s);
}

TEST(Describe, FallsBackForAnUnknownValue) {
  EXPECT_EQ(http_server::describe(static_cast<Stage>(999)), "unknown error");
}

} // namespace
