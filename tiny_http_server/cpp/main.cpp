#include <csignal>
#include <iostream>
#include <new>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <CLI/CLI.hpp>

#include "http.hpp"
#include "server.hpp"

namespace {

// Exit code for anything the server reports about itself. A usage error is 2 in
// the C port, but here that one is CLI11's to report and carries its own code.
constexpr int kExitFailure = 1;

// Checks that --host is a dotted quad. Names are not resolved: getaddrinfo
// would bring DNS and a blocking network lookup into the startup of a program
// that binds exactly one socket, and would hand back a list of candidates to
// choose between. inet_pton also turns down "127.1" and "0177.0.0.1", which the
// older inet_aton would have accepted as 127.0.0.1.
//
// A hand-written validator is the last resort the root CLAUDE.md describes, and
// this is what it is for: no built-in states this rule. CLI11's own ValidIPV4
// is the tempting alternative and is not the same grammar - it splits on '.'
// and range-checks four numbers of its own parsing - so taking it would quietly
// move which addresses this port accepts away from the C one, which is the
// whole mistake text_analyzer recorded.
CLI::Validator ipv4_address() {
  return CLI::Validator(
      [](std::string &value) -> std::string {
        in_addr addr{};
        if (::inet_pton(AF_INET, value.c_str(), &addr) == 1)
          return {};
        return "expected an IPv4 address such as 127.0.0.1";
      },
      "ADDR", "IPV4");
}

// Reports a --file that could not be loaded, in the loader's own terms.
void report_page_error(const http_server::Result &result,
                       const std::string &path) {
  switch (result.stage) {
  case http_server::Stage::kNotRegular:
    std::cerr << http_server::kProgName << ": " << path
              << ": not a regular file\n";
    return;
  case http_server::Stage::kTooLarge:
    std::cerr << http_server::kProgName << ": " << path << ": larger than the "
              << http_server::kMaxPageBytes << " byte limit\n";
    return;
  case http_server::Stage::kNoMem:
    std::cerr << http_server::kProgName << ": "
              << http_server::describe(result.stage) << "\n";
    return;
  default:
    std::cerr << http_server::kProgName << ": " << path << ": "
              << result.ec.message() << "\n";
    return;
  }
}

void report(const http_server::Result &result) {
  std::cerr << http_server::kProgName << ": "
            << http_server::describe(result.stage);
  if (result.ec)
    std::cerr << ": " << result.ec.message();
  std::cerr << "\n";
}

} // namespace

int main(int argc, char *argv[]) {
  // The name is passed explicitly because CLI11 otherwise takes argv[0], which
  // under `bazel run` is the full runfiles path.
  CLI::App app{
      "A very small HTTP server. Binds a socket, then repeats: accept one\n"
      "connection, read the request, answer it, close, accept the next.\n"
      "Open http://127.0.0.1:8080 in a browser to see the page.\n"
      "\n"
      "GET and HEAD of '/' or '/index.html' return 200 with a hello world\n"
      "page. Any other path is 404, any other method is 405, a request\n"
      "line that cannot be parsed is 400, and a version other than\n"
      "HTTP/1.x is 505. Every event is logged to stderr.",
      "tiny_http_server"};

  http_server::Options opts;
  std::string page_path;

  // Bound to an int and range-checked by the library, which is the repo's rule
  // and a deliberate divergence from the C port: CLI11 reads base 0 and strips
  // group separators, so this accepts --port 0x1F90 and --port 8_080, and reads
  // 010 as eight where strtol with base 10 reads it as ten. Hand-writing a
  // validator to match C instead is the mistake text_analyzer documented.
  app.add_option("-p,--port", opts.port,
                 "port to listen on, 0 to let the kernel pick")
      ->check(CLI::Range(0, 65535))
      ->option_text("N")
      ->capture_default_str();
  app.add_option("--host", opts.host, "IPv4 address to bind")
      ->check(ipv4_address())
      ->option_text("ADDR")
      ->capture_default_str();
  app.add_option("--file", page_path,
                 "serve this file instead of the built-in page")
      ->option_text("PATH");
  app.add_flag("--once", opts.serve_once, "serve one request, then exit");

  app.footer("Connections are served one at a time, so this is a toy rather "
             "than\na web server: any one slow client stalls the next. That is "
             "why the\ndefault binds loopback only -- pass --host 0.0.0.0 to "
             "expose it on\nevery interface, deliberately.");

  // CLI11 word-wraps the description and footer by default, which would rewrap
  // prose that is already laid out. Print it verbatim.
  app.get_formatter()->enable_description_formatting(false);
  app.get_formatter()->enable_footer_formatting(false);

  try {
    // There are no operands: requests arrive over the socket, never from argv,
    // so a stray one is a mistake worth naming. CLI11 rejects it as an extra.
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  // Writing to a socket whose peer has already gone raises SIGPIPE, whose
  // default action is to terminate the process with no message at all - so
  // without this the server vanishes the first time somebody navigates away
  // mid-response, and the symptom is "it just disappears sometimes". Ignoring
  // it turns the same event into an EPIPE the write path reports and the loop
  // survives.
  //
  // SocketStreambuf could pass MSG_NOSIGNAL to its send and need no signal
  // disposition at all - that is the thing the C port's FILE* seam had nowhere
  // to put the flag for. It still needs this, because the log goes to
  // std::cerr: `tiny_http_server 2>&1 | head` would then kill the server on the
  // closed log pipe, and a server that dies because nobody is reading its log
  // is exactly what the best-effort log rule exists to prevent. One
  // process-wide disposition covers both directions; two mechanisms would not
  // cover more.
  //
  // It is here rather than in the library because it is process-global state,
  // and the library takes streams its caller owns.
  if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    std::cerr << http_server::kProgName << ": cannot ignore SIGPIPE\n";
    return kExitFailure;
  }

  // The page is read once, here, before the socket exists. A file that cannot
  // be read is then a startup failure with a message, rather than a 500 that
  // turns up later depending on which path somebody visits - and routing stays
  // pure, with no I/O and no failure path, which is why this server has no 500
  // at all. The cost is that the page cannot change while the server runs.
  std::string page;
  if (!page_path.empty()) {
    http_server::Result loaded;
    try {
      loaded =
          http_server::load_page(page_path, http_server::kMaxPageBytes, page);
    } catch (const std::bad_alloc &) {
      loaded = {http_server::Stage::kNoMem, {}};
    }
    if (!loaded) {
      report_page_error(loaded, page_path);
      return kExitFailure;
    }
    opts.page = page;
  }

  http_server::Result result;
  try {
    result = http_server::run(opts, std::cerr);
  } catch (const std::bad_alloc &) {
    // The C port gets this as a failed allocation and returns HTTP_ERR_NOMEM;
    // here it arrives as an exception, and is reported the same way.
    result = {http_server::Stage::kNoMem, {}};
  }

  if (!result) {
    report(result);
    return kExitFailure;
  }
  return 0;
}
