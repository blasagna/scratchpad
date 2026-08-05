#include <gtest/gtest.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <string>

extern "C" {
#include "http.h"
}

/* The built-in page's size, spelled out so a change to the markup has to be a
 * deliberate edit here rather than a golden that quietly follows it. */
static const size_t kPageLen = 178;

/* Runs body against an in-memory output stream and returns what it wrote. */
template<typename F> static std::string captured(F body) {
  char buf[16384];
  FILE *out = fmemopen(buf, sizeof(buf), "w");
  /* Returning early rather than running body against a NULL stream: EXPECT_ is
   * non-fatal, so the alternative is a segfault in place of a test failure. */
  EXPECT_NE(out, nullptr);
  if (out == nullptr)
    return "";
  body(out);
  long written = ftell(out);
  EXPECT_EQ(fclose(out), 0);
  /* A full buffer means fmemopen silently dropped the overflow, so the
   * comparison downstream would be against truncated text. Fail here instead,
   * where the message says why. */
  EXPECT_LT(static_cast<size_t>(written < 0 ? 0 : written), sizeof(buf))
      << "captured() buffer is too small; the output was truncated";
  return std::string(buf, static_cast<size_t>(written < 0 ? 0 : written));
}

/* An in-memory input stream over data; the caller fcloses it. */
static FILE *make_input(const std::string &data) {
  FILE *in = fmemopen(const_cast<char *>(data.data()), data.size(), "r");
  EXPECT_NE(in, nullptr);
  return in;
}

/* Parses a request line and routes it, which is what every routing assertion
 * below actually cares about. Fails the test if the line does not parse. */
static HttpResponse routed(const std::string &raw, const HttpPage &page,
                           char *scratch) {
  HttpRequest req;
  HttpResult parsed = http_parse_request(raw.data(), raw.size(), &req);
  EXPECT_EQ(parsed, HTTP_OK) << "expected '" << raw << "' to parse";
  if (parsed != HTTP_OK) {
    /* A 500 is not a status this server can send, so it cannot be confused
     * with a real answer if an assertion above was non-fatal. */
    HttpResponse none = {500, "", nullptr, 0, nullptr};
    return none;
  }
  return http_route(&req, &page, scratch, HTTP_ERROR_PAGE_MAX);
}

/* The bytes an ordinary request produces, as a whole header block. */
static std::string request(const std::string &line) {
  return line + "\r\n\r\n";
}

/* --- http_read_request --- */

TEST(ReadRequest, StopsAtTheBlankLineAndLeavesTheRestUnread) {
  std::string data = "GET / HTTP/1.1\r\nHost: x\r\n\r\nBODYBYTES";
  FILE *in = make_input(data);
  char buf[HTTP_REQUEST_MAX];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_OK);
  EXPECT_EQ(std::string(buf, len), "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  /* The body is still there for whoever wants it, which nothing here does. */
  char rest[16] = {0};
  EXPECT_EQ(fread(rest, 1, sizeof(rest) - 1, in), 9u);
  EXPECT_STREQ(rest, "BODYBYTES");
  fclose(in);
}

TEST(ReadRequest, AcceptsLoneLfLineEndings) {
  std::string data = "GET / HTTP/1.1\n\n";
  FILE *in = make_input(data);
  char buf[HTTP_REQUEST_MAX];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_OK);
  EXPECT_EQ(len, data.size());
  fclose(in);
}

TEST(ReadRequest, AcceptsAMixedCrlfAndLfTerminator) {
  for (const char *data : {"GET / HTTP/1.1\r\n\n", "GET / HTTP/1.1\n\r\n"}) {
    std::string s = data;
    FILE *in = make_input(s);
    char buf[HTTP_REQUEST_MAX];
    size_t len = 0;
    EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_OK) << data;
    EXPECT_EQ(len, s.size()) << data;
    fclose(in);
  }
}

TEST(ReadRequest, DoesNotStopAtTheEndOfTheRequestLine) {
  /* The whole point of requiring a '\n' before the terminator: the CRLF
   * ending the request line must not look like a blank line. */
  std::string data = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  FILE *in = make_input(data);
  char buf[HTTP_REQUEST_MAX];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_OK);
  EXPECT_EQ(len, data.size());
  fclose(in);
}

TEST(ReadRequest, ReportsTheClientClosedBeforeAnyBytes) {
  /* A browser's speculative connection. The ordinary case, not an error. */
  FILE *in = make_input("");
  char buf[HTTP_REQUEST_MAX];
  size_t len = 99;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_ERR_CLOSED);
  EXPECT_EQ(len, 0u);
  fclose(in);
}

TEST(ReadRequest, ReportsTheClientClosedMidHeader) {
  FILE *in = make_input("GET / HTT");
  char buf[HTTP_REQUEST_MAX];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_ERR_CLOSED);
  EXPECT_EQ(len, 9u);
  fclose(in);
}

TEST(ReadRequest, ReportsTooLargeAtTheCap) {
  std::string data(64, 'x');
  FILE *in = make_input(data);
  char buf[16];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_ERR_TOO_LARGE);
  EXPECT_EQ(len, sizeof(buf));
  fclose(in);
}

TEST(ReadRequest, AcceptsExactlyTheCap) {
  /* Sixteen bytes ending in a blank line: the last byte that fits is also the
   * one that completes the terminator. */
  std::string data = "GET / HT/1.1\r\n\r\n";
  ASSERT_EQ(data.size(), 16u);
  FILE *in = make_input(data);
  char buf[16];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_OK);
  EXPECT_EQ(len, 16u);
  fclose(in);
}

TEST(ReadRequest, KeepsNulBytesAndReportsTheTrueLength) {
  std::string data("GET /\0x HTTP/1.1\r\n\r\n", 20);
  FILE *in = make_input(data);
  char buf[HTTP_REQUEST_MAX];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_OK);
  /* Length, not strlen: the NUL survives to the parser, which refuses it. */
  EXPECT_EQ(len, 20u);
  EXPECT_EQ(std::string(buf, len), data);
  fclose(in);
}

TEST(ReadRequest, ReportsAReadError) {
  /* A write-only stream fails every read, which is how a read error is
   * arranged without an unreadable file. */
  char sink[8];
  FILE *in = fmemopen(sink, sizeof(sink), "w");
  ASSERT_NE(in, nullptr);
  char buf[HTTP_REQUEST_MAX];
  size_t len = 0;
  EXPECT_EQ(http_read_request(in, buf, sizeof(buf), &len), HTTP_ERR_READ);
  fclose(in);
}

/* --- http_parse_request --- */

TEST(ParseRequest, AcceptsAMinimalGetLine) {
  std::string raw = request("GET / HTTP/1.1");
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(req.method, HTTP_METHOD_GET);
  EXPECT_EQ(std::string(req.target, req.target_len), "/");
  EXPECT_EQ(std::string(req.path, req.path_len), "/");
  EXPECT_EQ(req.major, 1);
  EXPECT_EQ(req.minor, 1);
  EXPECT_EQ(std::string(req.line, req.line_len), "GET / HTTP/1.1");
}

TEST(ParseRequest, SplitsThePathFromTheQuery) {
  std::string raw = request("GET /index.html?a=1&b=2 HTTP/1.1");
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(std::string(req.target, req.target_len), "/index.html?a=1&b=2");
  EXPECT_EQ(std::string(req.path, req.path_len), "/index.html");
}

TEST(ParseRequest, KeepsTheTargetVerbatim) {
  /* Not percent-decoded: nothing here reaches the filesystem, so there is no
   * path to normalize and no decoding to get wrong. */
  std::string raw = request("GET /a%20b/../c HTTP/1.1");
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(std::string(req.target, req.target_len), "/a%20b/../c");
}

TEST(ParseRequest, RecordsHeadAsTheMethod) {
  std::string raw = request("HEAD / HTTP/1.1");
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(req.method, HTTP_METHOD_HEAD);
}

TEST(ParseRequest, RecordsAnUnknownMethodAndLeavesTheJudgingToRouting) {
  for (const char *method : {"POST", "PUT", "DELETE", "get", "OPTIONS"}) {
    std::string raw = request(std::string(method) + " / HTTP/1.1");
    HttpRequest req;
    EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK)
        << method;
    EXPECT_EQ(req.method, HTTP_METHOD_OTHER) << method;
  }
}

TEST(ParseRequest, RecordsTheVersionNumbers) {
  std::string raw = request("GET / HTTP/1.0");
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(req.major, 1);
  EXPECT_EQ(req.minor, 0);
}

TEST(ParseRequest, SkipsOneLeadingEmptyLine) {
  for (const char *prefix : {"\r\n", "\n"}) {
    std::string raw = prefix + request("GET / HTTP/1.1");
    HttpRequest req;
    EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK)
        << prefix;
  }
  /* Exactly one, so a block of nothing but blank lines stays malformed. */
  std::string many = "\r\n\r\n\r\n";
  HttpRequest req;
  EXPECT_EQ(http_parse_request(many.data(), many.size(), &req),
            HTTP_ERR_MALFORMED);
}

TEST(ParseRequest, RejectsAnEmptyRequest) {
  HttpRequest req;
  EXPECT_EQ(http_parse_request("", 0, &req), HTTP_ERR_MALFORMED);
}

TEST(ParseRequest, RejectsARequestLineWithTwoFields) {
  /* HTTP/0.9, which this server does not speak. */
  std::string raw = request("GET /");
  HttpRequest req;
  EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req),
            HTTP_ERR_MALFORMED);
}

TEST(ParseRequest, RejectsARequestLineWithFourFields) {
  /* A space in a target has to be percent-encoded. */
  std::string raw = request("GET /a b HTTP/1.1");
  HttpRequest req;
  EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req),
            HTTP_ERR_MALFORMED);
}

TEST(ParseRequest, RejectsAnEmptyMethodOrTarget) {
  for (const char *line : {" / HTTP/1.1", "GET  HTTP/1.1"}) {
    std::string raw = request(line);
    HttpRequest req;
    EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req),
              HTTP_ERR_MALFORMED)
        << line;
  }
}

TEST(ParseRequest, RejectsARequestWithNoLineTerminator) {
  std::string raw = "GET / HTTP/1.1";
  HttpRequest req;
  EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req),
            HTTP_ERR_MALFORMED);
}

TEST(ParseRequest, RejectsAMalformedVersionToken) {
  /* Not "unsupported": none of these says which version it meant, so there is
   * nothing for routing to judge and the answer is a 400 rather than a 505. */
  for (const char *version :
       {"HTTP/1", "HTTP1.1", "http/1.1", "HTTP/x.y", "HTTP/11.1", "HTTP/1.",
        "HTTP/.1", "", "HTTP/1.1x"}) {
    std::string raw = request(std::string("GET / ") + version);
    HttpRequest req;
    EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req),
              HTTP_ERR_MALFORMED)
        << "version '" << version << "'";
  }
}

TEST(ParseRequest, RejectsANulByteInTheRequestLine) {
  std::string raw("GET /\0x HTTP/1.1\r\n\r\n", 20);
  HttpRequest req;
  EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req),
            HTTP_ERR_MALFORMED);
}

TEST(ParseRequest, AcceptsAnAbsurdlyLongTargetWithinTheCap) {
  /* It parses and becomes an ordinary 404, which is what pins the decision not
   * to implement 414. */
  std::string raw = request("GET /" + std::string(4000, 'a') + " HTTP/1.1");
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(req.target_len, 4001u);
}

TEST(ParseRequest, AcceptsATargetThatIsNotAnOriginPath) {
  /* Absolute-form and asterisk-form are legal targets a server must accept.
   * Routing 404s them; rejecting them here would 400 a request from a proxy
   * and would make the version check unreachable for an HTTP/2 preface. */
  for (const char *target : {"http://example.com/", "*", "example.com:80"}) {
    std::string raw = request(std::string("GET ") + target + " HTTP/1.1");
    HttpRequest req;
    EXPECT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK)
        << target;
  }
}

TEST(ParseRequest, IgnoresEverythingAfterTheRequestLine) {
  std::string raw =
      "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 999\r\nJunk\r\n\r\n";
  HttpRequest req;
  ASSERT_EQ(http_parse_request(raw.data(), raw.size(), &req), HTTP_OK);
  EXPECT_EQ(std::string(req.line, req.line_len), "GET / HTTP/1.1");
}

/* --- http_route --- */

TEST(Route, SlashServesTheConfiguredPage) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  HttpResponse r = routed(request("GET / HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.status, 200);
  EXPECT_STREQ(r.reason, "OK");
  EXPECT_EQ(r.body, page.body);
  EXPECT_EQ(r.body_len, kPageLen);
  EXPECT_EQ(r.allow, nullptr);
}

TEST(Route, IndexHtmlIsTheSamePage) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  HttpResponse r = routed(request("GET /index.html HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body_len, kPageLen);
}

TEST(Route, AQueryStringDoesNotChangeTheRoute) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  EXPECT_EQ(routed(request("GET /?x=1 HTTP/1.1"), page, scratch).status, 200);
}

TEST(Route, HeadRoutesLikeGetAndKeepsTheLength) {
  /* Withholding the body is the writer's job; the length a HEAD reports is
   * the one the GET would have sent. */
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  HttpResponse r = routed(request("HEAD / HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body_len, kPageLen);
}

TEST(Route, AnUnknownPathIsNotFound) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  for (const char *target :
       {"/nope", "/index.htm", "/index.html/", "//", "http://example.com/"}) {
    HttpResponse r = routed(request(std::string("GET ") + target + " HTTP/1.1"),
                            page, scratch);
    EXPECT_EQ(r.status, 404) << target;
  }
}

TEST(Route, FaviconIsAnOrdinaryNotFound) {
  /* Every browser asks for it unprompted, so this line is the log's most
   * common. It is intentional, not a bug to special-case away. */
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  EXPECT_EQ(routed(request("GET /favicon.ico HTTP/1.1"), page, scratch).status,
            404);
}

TEST(Route, PercentEncodedIndexIsNotDecoded) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  EXPECT_EQ(routed(request("GET /%69ndex.html HTTP/1.1"), page, scratch).status,
            404);
}

TEST(Route, LowercaseGetIsNotAMethod) {
  /* Methods are case-sensitive. Correcting this would answer a request the
   * client did not send. */
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  EXPECT_EQ(routed(request("get / HTTP/1.1"), page, scratch).status, 405);
}

TEST(Route, PostIsMethodNotAllowedAndNamesTheAllowedMethods) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  HttpResponse r = routed(request("POST / HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.status, 405);
  ASSERT_NE(r.allow, nullptr);
  EXPECT_STREQ(r.allow, "GET, HEAD");
}

TEST(Route, AnUnsupportedMajorVersionIsFiveOhFive) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  for (const char *version : {"HTTP/2.0", "HTTP/0.9", "HTTP/9.9"}) {
    HttpResponse r =
        routed(request(std::string("GET / ") + version), page, scratch);
    EXPECT_EQ(r.status, 505) << version;
  }
}

TEST(Route, AMinorVersionIsCompatible) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  for (const char *version : {"HTTP/1.0", "HTTP/1.1", "HTTP/1.9"}) {
    HttpResponse r =
        routed(request(std::string("GET / ") + version), page, scratch);
    EXPECT_EQ(r.status, 200) << version;
  }
}

TEST(Route, TheVersionOutranksTheMethod) {
  /* An HTTP/2 preface. A method belongs to a protocol, so a version we do not
   * speak leaves nothing to judge the method against. */
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  EXPECT_EQ(routed(request("PRI * HTTP/2.0"), page, scratch).status, 505);
}

TEST(Route, EveryErrorCarriesAnHtmlBody) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  for (const char *line :
       {"GET /nope HTTP/1.1", "POST / HTTP/1.1", "GET / HTTP/2.0"}) {
    HttpResponse r = routed(request(line), page, scratch);
    ASSERT_NE(r.body, nullptr) << line;
    EXPECT_GT(r.body_len, 0u) << line;
    EXPECT_EQ(std::string(r.body, r.body_len).find("<!DOCTYPE html>"), 0u)
        << line;
  }
}

TEST(Route, ServesTheConfiguredPageRatherThanTheBuiltInOne) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  const char *bytes = "<p>from --file</p>";
  HttpPage page = {bytes, strlen(bytes)};
  HttpResponse r = routed(request("GET / HTTP/1.1"), page, scratch);
  EXPECT_EQ(r.body, bytes);
  EXPECT_EQ(r.body_len, strlen(bytes));
}

/* --- http_error_response --- */

TEST(ErrorResponse, NamesEveryStatusItCanSend) {
  struct {
    int status;
    const char *reason;
  } cases[] = {{400, "Bad Request"},
               {404, "Not Found"},
               {405, "Method Not Allowed"},
               {431, "Request Header Fields Too Large"},
               {505, "HTTP Version Not Supported"}};
  char scratch[HTTP_ERROR_PAGE_MAX];
  for (auto &c : cases) {
    HttpResponse r = http_error_response(c.status, scratch, sizeof(scratch));
    EXPECT_EQ(r.status, c.status);
    EXPECT_STREQ(r.reason, c.reason);
    /* The status and reason appear twice: once in the title, once in the h1. */
    std::string body(r.body, r.body_len);
    EXPECT_NE(body.find(std::string("<title>") + std::to_string(c.status) +
                        " " + c.reason + "</title>"),
              std::string::npos)
        << c.status;
  }
}

TEST(ErrorResponse, OnlyMethodNotAllowedCarriesAnAllow) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  EXPECT_EQ(http_error_response(404, scratch, sizeof(scratch)).allow, nullptr);
  EXPECT_STREQ(http_error_response(405, scratch, sizeof(scratch)).allow,
               "GET, HEAD");
}

TEST(ErrorResponse, FitsInTheScratchBuffer) {
  /* 431 has the longest reason phrase, so it is the page that would overflow
   * first if HTTP_ERROR_PAGE_MAX ever stopped being enough. */
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpResponse r = http_error_response(431, scratch, sizeof(scratch));
  EXPECT_EQ(r.body_len, 185u);
  EXPECT_LT(r.body_len, sizeof(scratch));
}

/* --- http_write_response --- */

TEST(WriteResponse, WritesTheExactBytesOfAHelloWorldPage) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  HttpResponse r = routed(request("GET / HTTP/1.1"), page, scratch);
  std::string out = captured(
      [&](FILE *f) { EXPECT_EQ(http_write_response(f, &r, 0), HTTP_OK); });
  EXPECT_EQ(
      out,
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
  const char *bytes = "abcde";
  HttpResponse r = {200, "OK", bytes, 5, nullptr};
  std::string out = captured(
      [&](FILE *f) { EXPECT_EQ(http_write_response(f, &r, 0), HTTP_OK); });
  EXPECT_NE(out.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_EQ(out.substr(out.size() - 5), "abcde");
}

TEST(WriteResponse, SuppressesTheBodyForHeadButKeepsTheLength) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpPage page = http_builtin_page();
  HttpResponse r = routed(request("HEAD / HTTP/1.1"), page, scratch);
  std::string head = captured(
      [&](FILE *f) { EXPECT_EQ(http_write_response(f, &r, 1), HTTP_OK); });
  std::string get = captured(
      [&](FILE *f) { EXPECT_EQ(http_write_response(f, &r, 0), HTTP_OK); });
  /* Byte-identical headers, Content-Length included; only the body differs. */
  EXPECT_EQ(get.substr(0, head.size()), head);
  EXPECT_EQ(get.size(), head.size() + kPageLen);
  EXPECT_NE(head.find("Content-Length: 178\r\n"), std::string::npos);
}

TEST(WriteResponse, EmitsAllowOnlyOnAMethodNotAllowed) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpResponse allowed = http_error_response(405, scratch, sizeof(scratch));
  std::string with = captured([&](FILE *f) {
    EXPECT_EQ(http_write_response(f, &allowed, 0), HTTP_OK);
  });
  EXPECT_NE(with.find("Connection: close\r\nAllow: GET, HEAD\r\n\r\n"),
            std::string::npos);

  HttpResponse missing = http_error_response(404, scratch, sizeof(scratch));
  std::string without = captured([&](FILE *f) {
    EXPECT_EQ(http_write_response(f, &missing, 0), HTTP_OK);
  });
  EXPECT_EQ(without.find("Allow:"), std::string::npos);
}

TEST(WriteResponse, UsesCrlfEverywhereAndABlankLineBeforeTheBody) {
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpResponse r = http_error_response(404, scratch, sizeof(scratch));
  std::string out = captured(
      [&](FILE *f) { EXPECT_EQ(http_write_response(f, &r, 0), HTTP_OK); });
  size_t blank = out.find("\r\n\r\n");
  ASSERT_NE(blank, std::string::npos);
  /* No bare LF anywhere in the header block. The body has plenty. */
  for (size_t i = 0; i < blank + 4; i++) {
    if (out[i] == '\n') {
      EXPECT_EQ(out[i - 1], '\r') << "bare LF at offset " << i;
    }
  }
}

TEST(WriteResponse, WritesABodyContainingNulBytes) {
  /* --file takes whatever bytes are in the file, and Content-Length rather
   * than a NUL is what says where the body ends. */
  const char bytes[] = "a\0b";
  HttpResponse r = {200, "OK", bytes, 3, nullptr};
  std::string out = captured(
      [&](FILE *f) { EXPECT_EQ(http_write_response(f, &r, 0), HTTP_OK); });
  EXPECT_EQ(out.substr(out.size() - 3), std::string("a\0b", 3));
  EXPECT_NE(out.find("Content-Length: 3\r\n"), std::string::npos);
}

TEST(WriteResponse, ReportsAWriteError) {
  /* A buffer far too small for even the status line. */
  char tiny[4];
  FILE *out = fmemopen(tiny, sizeof(tiny), "w");
  ASSERT_NE(out, nullptr);
  char scratch[HTTP_ERROR_PAGE_MAX];
  HttpResponse r = http_error_response(404, scratch, sizeof(scratch));
  EXPECT_EQ(http_write_response(out, &r, 0), HTTP_ERR_WRITE);
  fclose(out);
}

/* --- http_sanitize --- */

TEST(Sanitize, PassesPrintableAsciiThrough) {
  char dst[64];
  const char *src = "GET /index.html?a=1 HTTP/1.1";
  EXPECT_EQ(http_sanitize(dst, sizeof(dst), src, strlen(src)), strlen(src));
  EXPECT_STREQ(dst, src);
}

TEST(Sanitize, ReplacesControlBytesAndHighBytes) {
  char dst[64];
  const char *src = "GE\x1b[2JT \xff/";
  http_sanitize(dst, sizeof(dst), src, strlen(src));
  /* Dropping the ESC is what defuses the sequence; what follows is ordinary. */
  EXPECT_STREQ(dst, "GE?[2JT ?/");
}

TEST(Sanitize, ReplacesANulByte) {
  char dst[64];
  const char src[] = "a\0b";
  EXPECT_EQ(http_sanitize(dst, sizeof(dst), src, 3), 3u);
  EXPECT_STREQ(dst, "a?b");
}

TEST(Sanitize, ReplacesANewlineSoALogLineCannotBeForged) {
  char dst[64];
  const char *src = "GET /\ntiny_http_server: forged";
  http_sanitize(dst, sizeof(dst), src, strlen(src));
  EXPECT_EQ(std::string(dst).find('\n'), std::string::npos);
}

TEST(Sanitize, TruncatesWithAnEllipsis) {
  char dst[8];
  const char *src = "abcdefghijklmnop";
  EXPECT_EQ(http_sanitize(dst, sizeof(dst), src, strlen(src)), 7u);
  EXPECT_STREQ(dst, "abcd...");
}

TEST(Sanitize, HandlesAOneByteDestination) {
  char dst[1] = {'x'};
  EXPECT_EQ(http_sanitize(dst, sizeof(dst), "abc", 3), 0u);
  EXPECT_STREQ(dst, "");
}

/* --- http_serve_connection --- */

/* Runs one whole transaction over in-memory streams and hands back both what
 * went to the client and what went to the log. */
namespace {
  struct Served {
    HttpResult result;
    std::string response;
    std::string log;
  };
} // namespace

static Served serve(const std::string &input, const HttpPage &page) {
  Served s;
  FILE *in = make_input(input);
  EXPECT_NE(in, nullptr);
  s.log = captured([&](FILE *log) {
    s.response = captured([&](FILE *out) {
      s.result = http_serve_connection(in, out, log, &page);
    });
  });
  if (in != nullptr)
    fclose(in);
  return s;
}

TEST(ServeConnection, AnswersTheDefaultPathWithHelloWorld) {
  Served s = serve(request("GET / HTTP/1.1"), http_builtin_page());
  EXPECT_EQ(s.result, HTTP_OK);
  EXPECT_EQ(s.response.find("HTTP/1.1 200 OK\r\n"), 0u);
  EXPECT_NE(s.response.find("<h1>Hello, world!</h1>"), std::string::npos);
}

TEST(ServeConnection, LogsTheRequestLineAndTheResponse) {
  Served s = serve(request("GET / HTTP/1.1"), http_builtin_page());
  EXPECT_EQ(s.log, "tiny_http_server: request GET / HTTP/1.1\n"
                   "tiny_http_server: response 200 OK (178 bytes)\n");
}

TEST(ServeConnection, LogsAMalformedRequestSanitizedAndQuoted) {
  Served s = serve("GE\x1bT /\r\n\r\n", http_builtin_page());
  EXPECT_EQ(s.log, "tiny_http_server: malformed request \"GE?T /\"\n"
                   "tiny_http_server: response 400 Bad Request (145 bytes)\n");
  EXPECT_EQ(s.log.find('\x1b'), std::string::npos);
}

TEST(ServeConnection, AnswersFourOhFourForAnUnknownPath) {
  Served s = serve(request("GET /nope HTTP/1.1"), http_builtin_page());
  EXPECT_EQ(s.result, HTTP_OK);
  EXPECT_EQ(s.response.find("HTTP/1.1 404 Not Found\r\n"), 0u);
  EXPECT_NE(s.log.find("response 404 Not Found (141 bytes)"),
            std::string::npos);
}

TEST(ServeConnection, AnswersFourOhFiveWithAllowForPost) {
  Served s = serve(request("POST / HTTP/1.1"), http_builtin_page());
  EXPECT_EQ(s.response.find("HTTP/1.1 405 Method Not Allowed\r\n"), 0u);
  EXPECT_NE(s.response.find("Allow: GET, HEAD\r\n"), std::string::npos);
}

TEST(ServeConnection, AnswersFourHundredForAMalformedRequestLine) {
  Served s = serve("garbage\r\n\r\n", http_builtin_page());
  EXPECT_EQ(s.result, HTTP_OK);
  EXPECT_EQ(s.response.find("HTTP/1.1 400 Bad Request\r\n"), 0u);
}

TEST(ServeConnection, AnswersFiveOhFiveForAnUnsupportedVersion) {
  Served s = serve(request("GET / HTTP/2.0"), http_builtin_page());
  EXPECT_EQ(s.response.find("HTTP/1.1 505 HTTP Version Not Supported\r\n"), 0u);
}

TEST(ServeConnection, AnswersFourThirtyOneWhenTheHeaderBlockIsTooLarge) {
  /* A client that is still there and is owed an answer, unlike every other
   * read failure. */
  std::string huge = "GET / HTTP/1.1\r\n" + std::string(HTTP_REQUEST_MAX, 'x');
  Served s = serve(huge, http_builtin_page());
  EXPECT_EQ(s.result, HTTP_OK);
  EXPECT_EQ(s.response.find("HTTP/1.1 431 Request Header Fields Too Large\r\n"),
            0u);
  EXPECT_NE(s.log.find("request header block over 8192 bytes"),
            std::string::npos);
}

TEST(ServeConnection, SuppressesTheBodyForHead) {
  Served s = serve(request("HEAD / HTTP/1.1"), http_builtin_page());
  EXPECT_NE(s.response.find("Content-Length: 178\r\n"), std::string::npos);
  EXPECT_EQ(s.response.find("Hello, world!"), std::string::npos);
  /* The log records what went on the wire, which for a HEAD is no body. */
  EXPECT_NE(s.log.find("response 200 OK (0 bytes)"), std::string::npos);
}

TEST(ServeConnection, WritesNothingWhenTheClientSendsNothing) {
  /* The browser-preconnect case: there is nobody to answer. */
  Served s = serve("", http_builtin_page());
  EXPECT_EQ(s.result, HTTP_ERR_CLOSED);
  EXPECT_EQ(s.response, "");
  EXPECT_EQ(s.log, "tiny_http_server: client closed the connection without "
                   "sending a request\n");
}

TEST(ServeConnection, WritesNothingWhenTheClientClosesMidHeader) {
  Served s = serve("GET / HTTP/1.1\r\n", http_builtin_page());
  EXPECT_EQ(s.result, HTTP_ERR_CLOSED);
  EXPECT_EQ(s.response, "");
}

TEST(ServeConnection, ReportsAWriteFailureToTheLog) {
  std::string input = request("GET / HTTP/1.1");
  FILE *in = make_input(input);
  ASSERT_NE(in, nullptr);
  char tiny[4];
  FILE *out = fmemopen(tiny, sizeof(tiny), "w");
  ASSERT_NE(out, nullptr);
  HttpPage page = http_builtin_page();
  HttpResult result = HTTP_OK;
  std::string log = captured(
      [&](FILE *f) { result = http_serve_connection(in, out, f, &page); });
  EXPECT_EQ(result, HTTP_ERR_WRITE);
  EXPECT_NE(log.find("error writing the response"), std::string::npos);
  fclose(out);
  fclose(in);
}

TEST(ServeConnection, ServesTheFilePageWhenOneIsConfigured) {
  const char *bytes = "<p>from --file</p>";
  HttpPage page = {bytes, strlen(bytes)};
  Served s = serve(request("GET / HTTP/1.1"), page);
  EXPECT_NE(s.response.find("Content-Length: 18\r\n"), std::string::npos);
  EXPECT_EQ(s.response.substr(s.response.size() - 18), bytes);
}

/* --- http_result_str --- */

TEST(ResultStr, LabelsEveryResult) {
  HttpResult all[] = {HTTP_OK,
                      HTTP_ERR_CLOSED,
                      HTTP_ERR_TIMEOUT,
                      HTTP_ERR_READ,
                      HTTP_ERR_WRITE,
                      HTTP_ERR_TOO_LARGE,
                      HTTP_ERR_MALFORMED,
                      HTTP_ERR_SOCKET,
                      HTTP_ERR_BIND,
                      HTTP_ERR_LISTEN,
                      HTTP_ERR_ACCEPT,
                      HTTP_ERR_OPEN,
                      HTTP_ERR_NOT_REGULAR,
                      HTTP_ERR_NOMEM};
  /* -Wswitch catches a new enumerator in the implementation; this catches one
   * that was never added here. */
  for (HttpResult r : all)
    EXPECT_STRNE(http_result_str(r), "unknown error") << static_cast<int>(r);
}

TEST(ResultStr, FallsBackForAnUnknownValue) {
  EXPECT_STREQ(http_result_str(static_cast<HttpResult>(999)), "unknown error");
}
