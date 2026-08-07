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

// Exit code for anything the server reports about itself. A usage error is
// CLI11's to report here, and carries its own code.
constexpr int kExitFailure = 1;

// Checks that --host is a dotted quad; names are not resolved. The last-resort
// hand-written validator the root CLAUDE.md describes: CLI11's ValidIPV4 parses
// four numbers of its own, which is a different grammar than inet_pton's.
CLI::Validator ipv4_address() {
  return CLI::Validator(
      [](const std::string &value) -> std::string {
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
  // The name is explicit because CLI11 otherwise takes argv[0], which under
  // `bazel run` is the full runfiles path.
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
  // and a deliberate divergence: CLI11 reads base 0 and strips group
  // separators, so --port 0x1F90, 8_080, and octal 010 all differ from C.
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
    // Requests arrive over the socket, never from argv, so a stray operand is a
    // mistake worth naming. CLI11 rejects it as an extra.
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  // Without this the server vanishes with no message the first time somebody
  // navigates away mid-response. MSG_NOSIGNAL would cover the socket but not
  // std::cerr, which `tiny_http_server 2>&1 | head` closes. See README.
  if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    std::cerr << http_server::kProgName << ": cannot ignore SIGPIPE\n";
    return kExitFailure;
  }

  // The page is read once, before the socket exists, so an unreadable file is a
  // startup failure rather than a 500 later - and routing stays pure, which is
  // why this server has no 500 at all.
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
    // The C port gets a failed allocation; here it is an exception, reported
    // the same way.
    result = {http_server::Stage::kNoMem, {}};
  }

  if (!result) {
    report(result);
    return kExitFailure;
  }
  return 0;
}
