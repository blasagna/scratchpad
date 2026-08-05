#ifndef MINI_SHELL_CPP_SHELL_HPP
#define MINI_SHELL_CPP_SHELL_HPP

#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <system_error>

namespace shell {

// What is written before every command is read.
inline constexpr std::string_view kPrompt = "$ ";

// Prefix on every message the shell itself reports, matching the binary name.
inline constexpr std::string_view kProgName = "mini_shell";

// The stage at which a shell operation failed. kRead and kWrite are backed by a
// stream, and carry the failing errno in Result::ec; the rest carry none.
//
// A command that fails is not one of these: the command ran, so the shell did
// its job. Those are reported per command as a Status and never end the loop.
enum class Stage {
  kOk,
  kRead,    // a read error occurred on the command input stream
  kWrite,   // a write error occurred on the output stream
  kNoMem,   // out of memory
  kNoShell, // std::system reported no command interpreter is available
};

// Returns a short human-readable label for a stage.
std::string_view describe(Stage stage);

// Outcome of an operation the shell performed on its own behalf. A stage other
// than kOk names what failed; ec carries the underlying errno when there was
// one.
struct Result {
  Stage stage = Stage::kOk;
  std::error_code ec{};

  bool ok() const noexcept { return stage == Stage::kOk; }
  explicit operator bool() const noexcept { return ok(); }
};

// How a command ended, decoded from the raw value std::system returns. That
// value is a wait status, not an exit code: reading it as one would report a
// command killed by SIGKILL as "exited with status 0".
enum class Outcome {
  kExited,     // ran to completion; code is the exit status
  kSignaled,   // killed by a signal; code is the signal number
  kUnrunnable, // std::system could not fork or wait; code is unused
};

struct Status {
  Outcome outcome = Outcome::kExited;
  int code = 0;
};

// Splits std::system's return value into an outcome and a code.
//
// Pure, so every case is testable without arranging for a real process to be
// killed. The standard calls the return value implementation-defined; POSIX
// makes it a wait status in the layout <sys/wait.h> describes, and -1 means the
// command could not be run at all.
//
// Returns kUnrunnable for -1; kSignaled with the signal number for a command
// killed by a signal; kExited with the exit status otherwise. A status that is
// neither an exit nor a signal (a stop, which std::system waits past) is
// reported as kExited with code 0, since there is no code to name.
Status decode_status(int raw);

// Writes one line to err about a command that did not succeed.
//
// A clean exit 0 writes nothing: the shell is silent when there is nothing to
// say, so the only output between prompts is the command's own. Status 127 is
// not special-cased either - the interpreter has already printed its own
// "command not found", and this line names the status behind it.
//
// For kUnrunnable the message carries strerror(errno), so call this while errno
// still belongs to the failed std::system.
//
// Returns kOk or kWrite.
Stage report_status(std::ostream &err, Status status);

// Narrows line to its content, dropping ASCII whitespace from both ends. The
// set is spelled out rather than taken from std::isspace, which is
// locale-dependent in principle.
std::string_view trim(std::string_view line);

// Reports whether a line asks the shell to quit: "exit" surrounded by any
// amount of ASCII whitespace.
//
// "EXIT", "exitx", and "exit 3" are commands like any other and are handed to
// the interpreter; matching them here would quietly diverge from what a real
// shell does with them.
bool is_exit_command(std::string_view line);

// Reports whether a line holds nothing worth running - empty, or entirely ASCII
// whitespace. Such a line is skipped rather than run: an interpreter would exit
// 0 for it anyway, and skipping saves a fork per stray Enter.
bool is_blank(std::string_view line);

// Writes the startup banner. Returns kOk or kWrite.
Stage write_banner(std::ostream &out);

// Runs one command and returns exactly what std::system returns.
//
// This is the seam that keeps the command loop testable: run() never calls
// std::system itself, so a test can supply a runner that records the command
// and hands back a canned status without forking anything.
using Runner = std::function<int(const std::string &command)>;

// The Runner that actually runs commands. The only impure function here;
// everything else transforms its arguments or the streams it is handed. Pass it
// from main, not from library code.
//
// Returns std::system's return value, undecoded.
int system_runner(const std::string &command);

// How the command loop behaves. runner must not be empty.
struct Options {
  bool show_banner = true;
  Runner runner;
};

// The command loop.
//
// Writes the banner (when opts.show_banner), then repeats: write kPrompt to out
// and flush it, read one line from in, and act on it. The flush matters - the
// command inherits out's file descriptor, so an unflushed prompt would surface
// after the command's own output.
//
// std::getline consumes the '\n'; one trailing '\r' is then stripped, so CRLF
// input runs the same commands as LF input. A blank line is skipped, "exit"
// ends the loop, and a line containing an embedded NUL is refused: std::system
// takes a NUL-terminated string, so the alternative is silently running a
// truncated command. Everything else goes to opts.runner and its status is
// reported to err.
//
// A command that fails does not end the loop and does not change the result;
// only the shell's own I/O can.
//
// Input:  in - the command source; end of input ends the loop, as "exit" does.
//         out - where the banner and prompts go.
//         err - where per-command failures are reported.
//
// Returns kOk when the loop ended at "exit" or end of input, or kRead / kWrite.
// Allocation failure propagates as std::bad_alloc rather than a Stage; main
// catches it and reports kNoMem.
Result run(std::istream &in, std::ostream &out, std::ostream &err,
           const Options &opts);

} // namespace shell

#endif // MINI_SHELL_CPP_SHELL_HPP
