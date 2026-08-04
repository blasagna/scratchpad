#include "shell.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/types.h>
#include <sys/wait.h>

namespace shell {
namespace {

// The startup banner. Written as a table so the art stays readable in the
// source. Every backslash is doubled: these are string literals, and the
// compiler would otherwise read "\_" as an unknown escape, which -Werror makes
// fatal. Unlike the C port, a '?''?' pair needs no care (trigraphs are gone in
// C++17) and a '%' is not a format specifier here, since these are written with
// ostream::write rather than printf.
constexpr std::array<std::string_view, 8> kBanner{
    " __  __  _        _   ____   _            _  _ ",
    "|  \\/  |(_) _ __  (_) / ___| | |__    ___ | || |",
    "| |\\/| || || '_ \\ | | \\___ \\ | '_ \\  / _ \\| || |",
    "| |  | || || | | || |  ___) || | | ||  __/| || |",
    "|_|  |_||_||_| |_||_| |____/ |_| |_| \\___||_||_|",
    "",
    "commands run through the system shell; type 'exit' to quit",
    "",
};

// The word that ends the loop, matched after trimming whitespace.
constexpr std::string_view kExitWord = "exit";

// ASCII whitespace, spelled out. std::isspace is locale-dependent in principle;
// nothing here calls std::setlocale, so the "C" locale would be in effect and
// the set would be exactly these - but saying so costs nothing and does not
// depend on that staying true.
constexpr std::string_view kSpace = " \t\n\v\f\r";

// Writes text to out, reporting whether the stream is still good. Callers stop
// at the first failure so errno still belongs to the call that failed rather
// than to a later write against an already-errored stream.
bool put(std::ostream &out, std::string_view text) {
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out.good();
}

// Writes the prompt and flushes it. The command inherits out's file descriptor,
// so an unflushed prompt would surface after the command's own output.
bool put_prompt(std::ostream &out) {
  return put(out, kPrompt) && out.flush().good();
}

// The errno the failing stream operation left behind, as an error_code. Unlike
// the C port, which leaves errno in place for main to pair with strerror, the
// code is captured here at the point of failure.
std::error_code last_error() {
  return std::error_code(errno, std::generic_category());
}

} // namespace

std::string_view describe(Stage stage) {
  switch (stage) {
  case Stage::kOk:
    return "success";
  case Stage::kRead:
    return "error reading command input";
  case Stage::kWrite:
    return "error writing output";
  case Stage::kNoMem:
    return "out of memory";
  case Stage::kNoShell:
    return "no command interpreter available";
  }
  return "unknown error";
}

Status decode_status(int raw) {
  Status status;

  if (raw == -1) {
    status.outcome = Outcome::kUnrunnable;
    return status;
  }
  // Signals first: a wait status is not an exit code, and reading one as the
  // other would report a command killed by SIGKILL as "exited with status 0".
  if (WIFSIGNALED(raw)) {
    status.outcome = Outcome::kSignaled;
    status.code = WTERMSIG(raw);
    return status;
  }
  status.outcome = Outcome::kExited;
  if (WIFEXITED(raw))
    status.code = WEXITSTATUS(raw);
  return status;
}

Stage report_status(std::ostream &err, Status status) {
  switch (status.outcome) {
  case Outcome::kExited:
    if (status.code == 0)
      return Stage::kOk;
    err << kProgName << ": command exited with status " << status.code << "\n";
    break;
  case Outcome::kSignaled:
    err << kProgName << ": command terminated by signal " << status.code
        << "\n";
    break;
  case Outcome::kUnrunnable:
    // std::system failed to fork or wait, so errno is about the shell, not the
    // command; the command never ran.
    err << kProgName << ": failed to run command: " << last_error().message()
        << "\n";
    break;
  }
  return err.good() ? Stage::kOk : Stage::kWrite;
}

std::string_view trim(std::string_view line) {
  const std::size_t first = line.find_first_not_of(kSpace);
  if (first == std::string_view::npos)
    return {};
  return line.substr(first, line.find_last_not_of(kSpace) - first + 1);
}

bool is_exit_command(std::string_view line) { return trim(line) == kExitWord; }

bool is_blank(std::string_view line) { return trim(line).empty(); }

Stage write_banner(std::ostream &out) {
  for (const std::string_view line : kBanner) {
    if (!put(out, line) || !put(out, "\n"))
      return Stage::kWrite;
  }
  return Stage::kOk;
}

int system_runner(const std::string &command) {
  return std::system(command.c_str());
}

Result run(std::istream &in, std::ostream &out, std::ostream &err,
           const Options &opts) {
  if (opts.show_banner && write_banner(out) != Stage::kOk)
    return {Stage::kWrite, last_error()};

  std::string line;

  for (;;) {
    if (!put_prompt(out))
      return {Stage::kWrite, last_error()};

    // std::getline reads into a std::string, which carries its own length, so a
    // line containing a NUL arrives intact and can be refused below rather than
    // silently truncated. It also extracts a final line with no trailing
    // newline: eofbit is set, but failbit only when nothing was extracted.
    if (!std::getline(in, line)) {
      if (in.bad())
        return {Stage::kRead, last_error()};
      // End of input: leave the cursor on a fresh line, since the prompt just
      // written is the last thing on this one.
      if (!put(out, "\n") || !out.flush().good())
        return {Stage::kWrite, last_error()};
      return {};
    }

    // getline consumed the '\n'; strip one '\r' so CRLF input runs the same
    // commands as LF input. A '\r' anywhere else belongs to the command.
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r')
      view.remove_suffix(1);

    if (is_blank(view))
      continue;
    if (is_exit_command(view))
      return {};

    if (view.find('\0') != std::string_view::npos) {
      err << kProgName << ": command contains a NUL byte\n";
      if (!err.good())
        return {Stage::kWrite, last_error()};
      continue;
    }

    // std::system takes a NUL-terminated string, so the command is handed over
    // as a std::string rather than the view.
    const Status status = decode_status(opts.runner(std::string(view)));
    if (report_status(err, status) != Stage::kOk)
      return {Stage::kWrite, last_error()};
  }
}

} // namespace shell
