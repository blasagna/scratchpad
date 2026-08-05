#include "shell.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
    "commands run directly, one program per line; type 'exit' to quit",
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
  }
  return "unknown error";
}

Status decode_status(int raw, int err) {
  Status status;

  if (raw == -1) {
    // The command never started. Which errno it was is the difference between
    // the two messages a user can act on and the catch-all one they cannot.
    if (err == ENOENT) {
      status.outcome = Outcome::kNotFound;
    } else if (err == EACCES) {
      status.outcome = Outcome::kNotExecutable;
    } else {
      status.outcome = Outcome::kUnrunnable;
      status.code = err;
    }
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

Stage report_status(std::ostream &err, Status status,
                    std::string_view program) {
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
  // The two failures worth naming the program for, in mini_shell's own words
  // rather than the system's. Writing the bytes here is what keeps the three
  // ports byte-identical on the most ordinary failure there is.
  case Outcome::kNotFound:
    err << kProgName << ": " << program << ": command not found\n";
    break;
  case Outcome::kNotExecutable:
    err << kProgName << ": " << program << ": permission denied\n";
    break;
  case Outcome::kUnrunnable:
    // Something else stopped the command from starting - a failed fork, an
    // exhausted process table - so the errno is about the shell rather than
    // about the command, which never ran.
    err << kProgName << ": failed to run command: "
        << std::error_code(status.code, std::generic_category()).message()
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

std::vector<std::string> split(std::string_view line) {
  std::vector<std::string> words;
  for (std::size_t at = line.find_first_not_of(kSpace);
       at != std::string_view::npos; at = line.find_first_not_of(kSpace, at)) {
    const std::size_t end = line.find_first_of(kSpace, at);
    words.emplace_back(line.substr(
        at, end == std::string_view::npos ? std::string_view::npos : end - at));
    at = end;
    if (at == std::string_view::npos)
      break;
  }
  return words;
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

namespace {

// Closes both ends of a pipe without disturbing the errno a caller is about to
// report. close() can fail, and would otherwise overwrite it.
void close_pipe(int fds[2]) {
  const int saved = errno;
  close(fds[0]);
  close(fds[1]);
  errno = saved;
}

} // namespace

int exec_runner(const std::vector<std::string> &argv) {
  // execvp wants a NULL-terminated array of pointers, and takes char *const []
  // for compatibility with C rather than because it modifies anything. The
  // const_cast is the standard way to bridge that, and the strings outlive the
  // call.
  std::vector<char *> raw_argv;
  raw_argv.reserve(argv.size() + 1);
  for (const std::string &word : argv)
    raw_argv.push_back(const_cast<char *>(word.c_str()));
  raw_argv.push_back(nullptr);

  // The channel the child reports a failed exec on. pipe + fcntl rather than
  // pipe2: pipe2 sits behind __USE_GNU in glibc's <unistd.h> and would need
  // _GNU_SOURCE, while these two are plain POSIX. Only the write end needs
  // FD_CLOEXEC - that is the whole trick, since a successful exec then closes
  // it and the parent's read sees end of file instead of an errno.
  int fds[2];
  if (pipe(fds) != 0)
    return -1;
  if (fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    close_pipe(fds);
    return -1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close_pipe(fds);
    return -1;
  }

  if (pid == 0) {
    close(fds[0]);
    execvp(raw_argv[0], raw_argv.data());
    // Only reached if the exec failed. The write is best effort: if it fails
    // there is nothing left to report the failure with, and the cast silences
    // the warn_unused_result on write that -Werror would otherwise make fatal.
    const int failure = errno;
    (void)!write(fds[1], &failure, sizeof failure);
    // _exit, never exit or return: this process shares the parent's stdio and
    // iostream buffers, and flushing them here would print the parent's pending
    // output a second time.
    _exit(127);
  }

  // The parent's copy of the write end must go, or the read below would block
  // waiting on a descriptor the parent itself is holding open.
  close(fds[1]);

  int child_errno = 0;
  ssize_t n;
  do {
    n = read(fds[0], &child_errno, sizeof child_errno);
  } while (n < 0 && errno == EINTR);
  close(fds[0]);

  // Reap the child either way: it exists even when the exec failed.
  int raw;
  while (waitpid(pid, &raw, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }

  if (n == static_cast<ssize_t>(sizeof child_errno)) {
    errno = child_errno;
    return -1;
  }
  return raw;
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

    // Splitting is mini_shell's whole grammar, and it is done here rather than
    // by an interpreter: the runner is handed a program and its arguments, not
    // a command line. The line is blank-checked above, so there is always at
    // least one word and a program to name in any diagnostic.
    const std::vector<std::string> argv = split(view);

    errno = 0;
    const int raw = opts.runner(argv);
    const Status status = decode_status(raw, errno);
    if (report_status(err, status, argv.front()) != Stage::kOk)
      return {Stage::kWrite, last_error()};
  }
}

} // namespace shell
