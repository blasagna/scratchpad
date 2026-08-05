#ifndef MINI_SHELL_CPP_SHELL_HPP
#define MINI_SHELL_CPP_SHELL_HPP

#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace shell {

// What is written before every command is read.
inline constexpr std::string_view kPrompt = "$ ";

// Prefix on every message the shell itself reports, matching the binary name.
inline constexpr std::string_view kProgName = "mini_shell";

// The stage at which a shell operation failed. kRead and kWrite are backed by a
// stream, and carry the failing errno in Result::ec; the rest carry none.
//
// A command that fails is not one of these: the shell did its job by running
// it. Those are reported per command as a Status and never end the loop.
enum class Stage {
  kOk,
  kRead,  // a read error occurred on the command input stream
  kWrite, // a write error occurred on the output stream
  kNoMem, // out of memory
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

// How a command ended.
//
// The first two are decoded from the wait status waitpid reports, which is not
// an exit code: reading it as one would report a command killed by SIGKILL as
// "exited with status 0". The last three are the ways a command can fail to
// start at all, told apart by the errno execvp left behind - which reaches the
// parent through the errno pipe in exec_runner, since execvp fails in the
// child.
enum class Outcome {
  kExited,        // ran to completion; code is the exit status
  kSignaled,      // killed by a signal; code is the signal number
  kNotFound,      // execvp: ENOENT; code is unused
  kNotExecutable, // execvp: EACCES; code is unused
  kUnrunnable,    // any other failure to start; code is the errno
};

struct Status {
  Outcome outcome = Outcome::kExited;
  int code = 0;
};

// Splits a runner's return value into an outcome and a code.
//
// Pure - the errno is passed in rather than read from the global, so every case
// is testable without arranging for a real process to be killed or for a fork
// to fail. raw is a wait status in the layout <sys/wait.h> describes, or -1 if
// the command never started; err is the errno the runner left behind, and is
// read only in that case.
//
// For -1: kNotFound for ENOENT, kNotExecutable for EACCES, kUnrunnable carrying
// err for anything else. Otherwise kSignaled with the signal number for a
// command killed by a signal, and kExited with the exit status for one that
// ran. A status that is neither an exit nor a signal (a stop, which the runner
// waits past) is reported as kExited with code 0, since there is no code to
// name.
Status decode_status(int raw, int err);

// Writes one line to err about a command that did not succeed.
//
// A clean exit 0 writes nothing: the shell is silent when there is nothing to
// say, so the only output between prompts is the command's own.
//
// The two common ways to fail to start - the program does not exist, and it
// exists but cannot be executed - name the program in mini_shell's own words
// rather than with the system's error text. That is deliberate and load-bearing
// for cross-port parity: every port writes these bytes itself, where the text
// of an errno differs between them.
//
// Returns kOk or kWrite.
Stage report_status(std::ostream &err, Status status, std::string_view program);

// Narrows line to its content, dropping ASCII whitespace from both ends. The
// set is spelled out rather than taken from std::isspace, which is
// locale-dependent in principle.
std::string_view trim(std::string_view line);

// Splits a command line into words on ASCII whitespace, using the same set as
// trim.
//
// This is the whole of mini_shell's grammar. There is no quoting, no escaping,
// and no expansion of any kind: a run of whitespace separates two words and
// every other byte is literal, so `echo a | wc` runs echo with the three
// arguments "a", "|", and "wc". Splitting is the job std::system used to hand
// to /bin/sh, and taking it back is the point of this port.
//
// A line that is empty or entirely whitespace yields no words; callers skip
// such lines before getting here.
std::vector<std::string> split(std::string_view line);

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

// Runs one command and returns the wait status waitpid reported, or -1 with
// errno set if the command could not be started at all.
//
// This is the seam that keeps the command loop testable: run() never forks
// anything itself, so a test can supply a runner that records the argv and
// hands back a canned status without spawning a process. argv[0] is the
// program; the rest are its arguments.
using Runner = std::function<int(const std::vector<std::string> &argv)>;

// The Runner that actually runs commands: forks, execvp's argv in the child,
// and waits. The only impure function here; everything else transforms its
// arguments or the streams it is handed. Pass it from main, not from library
// code.
//
// execvp fails in the child, where the parent cannot see its errno, so the two
// are joined by a close-on-exec pipe: the child writes the errno and _exits, a
// successful exec closes the pipe instead, and the parent tells the cases apart
// by whether the read returned anything. This is the same mechanism Rust's
// std::process::Command uses internally, and it is what lets all three ports
// report a missing program identically. Inferring it from an exit status of 127
// was the alternative and was rejected: a command that really does exit 127 is
// then indistinguishable from one that never ran.
//
// Returns the wait status, undecoded, or -1 with errno set. An empty argv is
// one of those: it is EINVAL rather than execvp(nullptr, ...), since run's
// blank check is no guarantee for a caller reaching this directly.
int exec_runner(const std::vector<std::string> &argv);

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
// ends the loop, and a line containing an embedded NUL is refused: execvp takes
// NUL-terminated strings, so the alternative is silently running a truncated
// command. Everything else is split into words, handed to opts.runner, and its
// status reported to err.
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
