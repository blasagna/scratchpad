#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "shell.hpp"

namespace {

using shell::Options;
using shell::Outcome;
using shell::Result;
using shell::Stage;
using shell::Status;

// A wait status as waitpid reports one. The encoding is not in the standard
// -- <sys/wait.h> only promises the WIF* macros -- so these two build the
// layout every platform this runs on uses, and
// RealExec.EncodingMatchesTheMacros below checks that assumption against the
// libc actually linked in.
int exited(int code) { return code << 8; }
int signaled(int sig) { return sig; }

// One command as the runner receives it: a program and its arguments.
using Argv = std::vector<std::string>;

// Records what it was asked to run and hands back canned statuses, so the
// command loop is exercised without forking anything. Statuses are consumed in
// order; once they run out, every further command succeeds.
struct Recorder {
  std::vector<Argv> commands;
  std::vector<int> statuses;
  std::size_t next = 0;
  // The errno a canned status of -1 arrives with, since that is half of what a
  // real runner reports when the command never started.
  int fail_errno = 0;

  int operator()(const Argv &argv) {
    commands.push_back(argv);
    int status = exited(0);
    if (next < statuses.size())
      status = statuses[next++];
    if (status == -1)
      errno = fail_errno;
    return status;
  }
};

// The std::function seam, bound to a recorder the caller keeps.
Options fake_options(Recorder *fake) {
  Options opts;
  opts.show_banner = false;
  opts.runner = [fake](const Argv &argv) { return (*fake)(argv); };
  return opts;
}

// Result of one run() over a fixed input: everything a caller can observe. err
// is captured separately from out because the two streams are the contract:
// commands and prompts on one, the shell's own complaints on the other.
struct RunOutput {
  Result result;
  std::string out;
  std::string err;
};

RunOutput run_shell(const std::string &input, const Options &opts) {
  std::istringstream in(input);
  std::ostringstream out;
  std::ostringstream err;

  RunOutput captured;
  captured.result = shell::run(in, out, err, opts);
  captured.out = out.str();
  captured.err = err.str();
  return captured;
}

// --- decode_status -------------------------------------------------------

TEST(DecodeStatus, CleanExitIsExitedZero) {
  const Status status = shell::decode_status(exited(0), 0);
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 0);
}

TEST(DecodeStatus, NonzeroExitKeepsItsCode) {
  const Status status = shell::decode_status(exited(3), 0);
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 3);
}

TEST(DecodeStatus, AnExitOf127IsNotSpecialCased) {
  // It used to be how the interpreter said "command not found". Nothing says
  // that now -- a missing program never reaches waitpid at all -- so 127 is
  // whatever the command chose to exit with, like any other code.
  const Status status = shell::decode_status(exited(127), 0);
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 127);
}

TEST(DecodeStatus, SignalIsNotReadAsAnExitCode) {
  const Status status = shell::decode_status(signaled(9), 0);
  EXPECT_EQ(status.outcome, Outcome::kSignaled);
  EXPECT_EQ(status.code, 9);
}

TEST(DecodeStatus, MissingProgramIsToldApartByErrno) {
  EXPECT_EQ(shell::decode_status(-1, ENOENT).outcome, Outcome::kNotFound);
}

TEST(DecodeStatus, UnexecutableProgramIsToldApartByErrno) {
  EXPECT_EQ(shell::decode_status(-1, EACCES).outcome, Outcome::kNotExecutable);
}

TEST(DecodeStatus, AnyOtherErrnoIsUnrunnableAndIsCarried) {
  // A failed fork lands here, and the errno is carried rather than left in the
  // global for the reporting to pick up later.
  const Status status = shell::decode_status(-1, EAGAIN);
  EXPECT_EQ(status.outcome, Outcome::kUnrunnable);
  EXPECT_EQ(status.code, EAGAIN);
}

// --- report_status -------------------------------------------------------

std::string reported(Status status, std::string_view program = "acommand") {
  std::ostringstream err;
  EXPECT_EQ(shell::report_status(err, status, program), Stage::kOk);
  return err.str();
}

TEST(ReportStatus, SuccessIsSilent) {
  EXPECT_EQ(reported({Outcome::kExited, 0}), "");
}

TEST(ReportStatus, NamesTheExitStatus) {
  EXPECT_EQ(reported({Outcome::kExited, 3}),
            "mini_shell: command exited with status 3\n");
}

TEST(ReportStatus, NamesTheSignal) {
  EXPECT_EQ(reported({Outcome::kSignaled, 9}),
            "mini_shell: command terminated by signal 9\n");
}

TEST(ReportStatus, NamesAMissingProgram) {
  // mini_shell's own words, not the system's: every port writes these bytes
  // itself, which is what keeps them byte-identical.
  EXPECT_EQ(reported({Outcome::kNotFound, 0}, "nosuchcmd"),
            "mini_shell: nosuchcmd: command not found\n");
}

TEST(ReportStatus, NamesAProgramItMayNotExecute) {
  EXPECT_EQ(reported({Outcome::kNotExecutable, 0}, "/etc/passwd"),
            "mini_shell: /etc/passwd: permission denied\n");
}

TEST(ReportStatus, ReportsAnUnrunnableCommandWithErrno) {
  // The residual case, and the only message here that borrows the system's
  // wording.
  EXPECT_EQ(reported({Outcome::kUnrunnable, EAGAIN}),
            std::string("mini_shell: failed to run command: ") +
                std::strerror(EAGAIN) + "\n");
}

// --- is_exit_command / is_blank ------------------------------------------

TEST(ExitCommand, MatchesTheBareWord) {
  EXPECT_TRUE(shell::is_exit_command("exit"));
}

TEST(ExitCommand, IgnoresSurroundingWhitespace) {
  EXPECT_TRUE(shell::is_exit_command("  exit"));
  EXPECT_TRUE(shell::is_exit_command("exit\t"));
  EXPECT_TRUE(shell::is_exit_command(" \texit \t"));
}

TEST(ExitCommand, DoesNotMatchOtherSpellings) {
  // Each of these is a command like any other and goes to the interpreter;
  // matching them here would quietly diverge from what a real shell does.
  EXPECT_FALSE(shell::is_exit_command("EXIT"));
  EXPECT_FALSE(shell::is_exit_command("exitx"));
  EXPECT_FALSE(shell::is_exit_command("exit 3"));
  EXPECT_FALSE(shell::is_exit_command("exi"));
  EXPECT_FALSE(shell::is_exit_command(""));
}

TEST(Blank, EmptyAndWhitespaceOnly) {
  EXPECT_TRUE(shell::is_blank(""));
  EXPECT_TRUE(shell::is_blank("   "));
  EXPECT_TRUE(shell::is_blank("\t \t"));
  EXPECT_FALSE(shell::is_blank("ls"));
  EXPECT_FALSE(shell::is_blank("  ls  "));
}

TEST(Trim, KeepsInteriorBytesIncludingNuls) {
  // string_view carries its own length, so trimming never stops at a NUL the
  // way a NUL-terminated string would.
  const std::string_view line("  echo a\0b  ", 12);
  EXPECT_EQ(shell::trim(line), std::string_view("echo a\0b", 8));
}

// --- split ---------------------------------------------------------------

TEST(Split, OneWordIsTheProgramAlone) {
  EXPECT_EQ(shell::split("ls"), Argv{"ls"});
}

TEST(Split, LaterWordsAreArguments) {
  EXPECT_EQ(shell::split("echo hello world"), (Argv{"echo", "hello", "world"}));
}

TEST(Split, RunsOfWhitespaceSeparateExactlyOnce) {
  // Two arguments, not five: the empty strings between the spaces are not
  // words. A shell that passed them along would hand echo blank arguments.
  EXPECT_EQ(shell::split("echo   a  \t b"), (Argv{"echo", "a", "b"}));
}

TEST(Split, SurroundingWhitespaceIsDropped) {
  EXPECT_EQ(shell::split("  \t ls -l \t "), (Argv{"ls", "-l"}));
}

TEST(Split, VerticalTabAndFormFeedSeparateToo) {
  // The same set trim() uses.
  EXPECT_EQ(shell::split("echo\va\fb"), (Argv{"echo", "a", "b"}));
}

TEST(Split, NothingToSplitYieldsNoWords) {
  EXPECT_TRUE(shell::split("").empty());
  EXPECT_TRUE(shell::split("   \t ").empty());
}

TEST(Split, ShellMetacharactersAreOrdinaryWords) {
  // The whole of the grammar: whitespace separates, everything else is a byte
  // in a word. There is no interpreter left to give these any meaning.
  EXPECT_EQ(shell::split("echo a | wc"), (Argv{"echo", "a", "|", "wc"}));
  EXPECT_EQ(shell::split("echo * $HOME > out"),
            (Argv{"echo", "*", "$HOME", ">", "out"}));
  EXPECT_EQ(shell::split("echo \"a b\""), (Argv{"echo", "\"a", "b\""}));
}

TEST(Split, NonAsciiBytesSurvive) {
  EXPECT_EQ(shell::split("echo \xc3\xa9"), (Argv{"echo", "\xc3\xa9"}));
}

// --- run -----------------------------------------------------------------

TEST(Run, PassesCommandsToTheRunnerInOrder) {
  Recorder fake;
  const RunOutput run =
      run_shell("echo one\necho two\nexit\n", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  ASSERT_EQ(fake.commands.size(), 2u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "one"}));
  EXPECT_EQ(fake.commands[1], (Argv{"echo", "two"}));
}

TEST(Run, HandsTheRunnerASplitLineRatherThanACommandLine) {
  Recorder fake;
  run_shell("  ls   -l  /tmp \nexit\n", fake_options(&fake));

  // Surrounding and interior whitespace is gone by the time the runner sees
  // it: splitting is mini_shell's job now, not an interpreter's.
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"ls", "-l", "/tmp"}));
}

TEST(Run, TreatsShellMetacharactersAsOrdinaryArguments) {
  Recorder fake;
  run_shell("echo a | wc\nexit\n", fake_options(&fake));

  // No pipe: echo is run once, with three arguments.
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "a", "|", "wc"}));
}

TEST(Run, ReportsAMissingProgramByName) {
  Recorder fake;
  // The runner returns -1 with errno set, exactly as exec_runner does when the
  // child's execvp failed.
  fake.statuses = {-1};
  fake.fail_errno = ENOENT;
  const RunOutput run = run_shell("nosuchcmd arg\nexit\n", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  EXPECT_EQ(run.err, "mini_shell: nosuchcmd: command not found\n");
}

TEST(Run, PromptsOncePerCommandAndOnceMoreForExit) {
  Recorder fake;
  const RunOutput run = run_shell("ls\nexit\n", fake_options(&fake));

  EXPECT_EQ(run.out, "$ $ ");
  EXPECT_EQ(run.err, "");
}

TEST(Run, ExitStopsTheLoopAndLeavesLaterLinesUnread) {
  Recorder fake;
  const RunOutput run = run_shell("exit\nrm -rf /\n", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  EXPECT_TRUE(fake.commands.empty());
}

TEST(Run, EndOfInputStopsTheLoopAndEndsTheLine) {
  Recorder fake;
  const RunOutput run = run_shell("ls\n", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  ASSERT_EQ(fake.commands.size(), 1u);
  // The prompt for the command, then the prompt that end of input answered,
  // then a newline so the cursor does not stop on the prompt's line.
  EXPECT_EQ(run.out, "$ $ \n");
}

TEST(Run, RunsAFinalLineWithNoNewline) {
  Recorder fake;
  run_shell("echo hi", fake_options(&fake));

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "hi"}));
}

TEST(Run, ExitNeedsNoTrailingNewline) {
  Recorder fake;
  const RunOutput run = run_shell("exit", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  EXPECT_TRUE(fake.commands.empty());
  // The loop ended at "exit", not at end of input, so no closing newline.
  EXPECT_EQ(run.out, "$ ");
}

TEST(Run, BlankLinesArePromptedForButNotRun) {
  Recorder fake;
  const RunOutput run = run_shell("\n   \nls\nexit\n", fake_options(&fake));

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], Argv{"ls"});
  EXPECT_EQ(run.out, "$ $ $ $ ");
}

TEST(Run, StripsCrlfAndSplitsOnAnInteriorCarriageReturn) {
  Recorder fake;
  run_shell("echo a\rb\r\n", fake_options(&fake));

  // The trailing "\r\n" is a line terminator and is stripped. The interior
  // '\r' is ASCII whitespace like any other, so it separates two arguments --
  // where the interpreter used to receive it inside the command line.
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "a", "b"}));
}

TEST(Run, ReportsAFailedCommandAndKeepsGoing) {
  Recorder fake;
  fake.statuses = {exited(3), exited(0)};
  const RunOutput run = run_shell("false\ntrue\nexit\n", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  EXPECT_EQ(fake.commands.size(), 2u);
  EXPECT_EQ(run.err, "mini_shell: command exited with status 3\n");
}

TEST(Run, ReportsASignaledCommand) {
  Recorder fake;
  fake.statuses = {signaled(9)};
  const RunOutput run = run_shell("sleep 100\nexit\n", fake_options(&fake));

  EXPECT_EQ(run.err, "mini_shell: command terminated by signal 9\n");
}

TEST(Run, RefusesALineContainingANulByte) {
  Recorder fake;
  // execvp takes NUL-terminated strings, so running this would run "echo a"
  // and silently drop the rest.
  const RunOutput run = run_shell(std::string("echo a\0rm -rf /\nexit\n", 21),
                                  fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_EQ(run.err, "mini_shell: command contains a NUL byte\n");
}

TEST(Run, PassesNonAsciiBytesThrough) {
  Recorder fake;
  run_shell("echo \xc3\xa9\nexit\n", fake_options(&fake));

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "\xc3\xa9"}));
}

TEST(Run, EmptyInputStillPromptsOnce) {
  Recorder fake;
  const RunOutput run = run_shell("", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_EQ(run.out, "$ \n");
}

TEST(Run, WritesTheBannerBeforeTheFirstPromptWhenAsked) {
  Recorder fake;
  Options opts = fake_options(&fake);
  opts.show_banner = true;
  const RunOutput run = run_shell("exit\n", opts);

  std::ostringstream banner;
  EXPECT_EQ(shell::write_banner(banner), Stage::kOk);
  EXPECT_FALSE(banner.str().empty());
  EXPECT_EQ(run.out, banner.str() + "$ ");
}

TEST(Run, ReportsAReadErrorOnTheInputStream) {
  Recorder fake;
  const Options opts = fake_options(&fake);
  // badbit is the only failure a string stream can be made to report, and it is
  // exactly what run() reads as a read error rather than end of input.
  std::istringstream in("ls\nexit\n");
  in.setstate(std::ios::badbit);
  std::ostringstream out;
  std::ostringstream err;

  const Result result = shell::run(in, out, err, opts);
  EXPECT_EQ(result.stage, Stage::kRead);
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_EQ(err.str(), "");
}

TEST(Run, ReportsAWriteErrorOnThePrompt) {
  Recorder fake;
  const Options opts = fake_options(&fake);
  std::istringstream in("exit\n");
  std::ostringstream out;
  out.setstate(std::ios::badbit);
  std::ostringstream err;

  const Result result = shell::run(in, out, err, opts);
  EXPECT_EQ(result.stage, Stage::kWrite);
  EXPECT_EQ(err.str(), "");
}

// --- describe ------------------------------------------------------------

TEST(Describe, LabelsEveryStage) {
  EXPECT_EQ(shell::describe(Stage::kOk), "success");
  EXPECT_EQ(shell::describe(Stage::kRead), "error reading command input");
  EXPECT_EQ(shell::describe(Stage::kWrite), "error writing output");
  EXPECT_EQ(shell::describe(Stage::kNoMem), "out of memory");
}

TEST(Describe, FallsBackForAnUnknownValue) {
  EXPECT_EQ(shell::describe(static_cast<Stage>(999)), "unknown error");
}

// --- exec_runner ---------------------------------------------------------

// Runs argv for real and decodes the result. The tests below can hand the
// runner an argv the splitter could never produce, which is how "exit 3"
// arrives as one argument here.
Status really_run(const Argv &argv) {
  errno = 0;
  const int raw = shell::exec_runner(argv);
  return shell::decode_status(raw, errno);
}

TEST(RealExec, EncodingMatchesTheMacros) {
  // The one test that really forks. It pins the assumption behind exited() and
  // signaled() above: if this libc encoded a wait status differently, every
  // status this suite builds by hand would be meaningless.
  Status status = really_run({"/bin/sh", "-c", "exit 3"});
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 3);

  status = really_run({"/bin/sh", "-c", "exit 0"});
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 0);

  status = really_run({"/bin/sh", "-c", "kill -9 $$"});
  EXPECT_EQ(status.outcome, Outcome::kSignaled);
  EXPECT_EQ(status.code, 9);
}

TEST(RealExec, FindsAProgramOnPath) {
  // No absolute path: execvp searches PATH, which is what makes "ls" work at
  // the prompt without the shell that used to do the looking.
  const Status status = really_run({"true"});
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 0);
}

TEST(RealExec, ReportsAMissingProgramThroughTheErrnoPipe) {
  // The exec fails in the child, so this errno crossed a pipe to get here.
  // Without that channel the parent would see only an exit status of 127 and
  // could not tell it from a command that really exited 127.
  EXPECT_EQ(really_run({"nosuchcommand_xyzzy"}).outcome, Outcome::kNotFound);
}

TEST(RealExec, ReportsAProgramItMayNotExecute) {
  EXPECT_EQ(really_run({"/etc/passwd"}).outcome, Outcome::kNotExecutable);
}

} // namespace
