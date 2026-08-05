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

// A wait status as std::system returns one. The encoding is not in the standard
// -- <sys/wait.h> only promises the WIF* macros -- so these two build the
// layout every platform this runs on uses, and
// RealSystem.EncodingMatchesTheMacros below checks that assumption against the
// libc actually linked in.
int exited(int code) { return code << 8; }
int signaled(int sig) { return sig; }

// Records what it was asked to run and hands back canned statuses, so the
// command loop is exercised without forking anything. Statuses are consumed in
// order; once they run out, every further command succeeds.
struct Recorder {
  std::vector<std::string> commands;
  std::vector<int> statuses;
  std::size_t next = 0;

  int operator()(const std::string &command) {
    commands.push_back(command);
    if (next < statuses.size())
      return statuses[next++];
    return exited(0);
  }
};

// The std::function seam, bound to a recorder the caller keeps.
Options fake_options(Recorder *fake) {
  Options opts;
  opts.show_banner = false;
  opts.runner = [fake](const std::string &command) { return (*fake)(command); };
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
  const Status status = shell::decode_status(exited(0));
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 0);
}

TEST(DecodeStatus, NonzeroExitKeepsItsCode) {
  const Status status = shell::decode_status(exited(3));
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 3);
}

TEST(DecodeStatus, CommandNotFoundIsAnOrdinaryExit) {
  // 127 is what the interpreter exits with when it cannot find the command. It
  // is not special-cased: the interpreter has already said so itself.
  const Status status = shell::decode_status(exited(127));
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 127);
}

TEST(DecodeStatus, SignalIsNotReadAsAnExitCode) {
  const Status status = shell::decode_status(signaled(9));
  EXPECT_EQ(status.outcome, Outcome::kSignaled);
  EXPECT_EQ(status.code, 9);
}

TEST(DecodeStatus, MinusOneMeansTheCommandNeverRan) {
  const Status status = shell::decode_status(-1);
  EXPECT_EQ(status.outcome, Outcome::kUnrunnable);
}

// --- report_status -------------------------------------------------------

std::string reported(Status status) {
  std::ostringstream err;
  EXPECT_EQ(shell::report_status(err, status), Stage::kOk);
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

TEST(ReportStatus, ReportsAnUnrunnableCommandWithErrno) {
  errno = EAGAIN;
  EXPECT_EQ(reported({Outcome::kUnrunnable, 0}),
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

// --- run -----------------------------------------------------------------

TEST(Run, PassesCommandsToTheRunnerInOrder) {
  Recorder fake;
  const RunOutput run =
      run_shell("echo one\necho two\nexit\n", fake_options(&fake));

  EXPECT_TRUE(run.result.ok());
  ASSERT_EQ(fake.commands.size(), 2u);
  EXPECT_EQ(fake.commands[0], "echo one");
  EXPECT_EQ(fake.commands[1], "echo two");
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
  EXPECT_EQ(fake.commands[0], "echo hi");
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
  EXPECT_EQ(fake.commands[0], "ls");
  EXPECT_EQ(run.out, "$ $ $ $ ");
}

TEST(Run, StripsCrlfButKeepsAnInteriorCarriageReturn) {
  Recorder fake;
  run_shell("echo a\rb\r\n", fake_options(&fake));

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], "echo a\rb");
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
  // std::system takes a NUL-terminated string, so running this would run
  // "echo a" and silently drop the rest.
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
  EXPECT_EQ(fake.commands[0], "echo \xc3\xa9");
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
  EXPECT_EQ(shell::describe(Stage::kNoShell),
            "no command interpreter available");
}

TEST(Describe, FallsBackForAnUnknownValue) {
  EXPECT_EQ(shell::describe(static_cast<Stage>(999)), "unknown error");
}

// --- system_runner -------------------------------------------------------

TEST(RealSystem, EncodingMatchesTheMacros) {
  // The one test that really forks. It pins the assumption behind exited() and
  // signaled() above: if this libc encoded a wait status differently, every
  // status this suite builds by hand would be meaningless.
  Status status = shell::decode_status(shell::system_runner("exit 3"));
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 3);

  status = shell::decode_status(shell::system_runner("exit 0"));
  EXPECT_EQ(status.outcome, Outcome::kExited);
  EXPECT_EQ(status.code, 0);

  status = shell::decode_status(shell::system_runner("kill -9 $$"));
  EXPECT_EQ(status.outcome, Outcome::kSignaled);
  EXPECT_EQ(status.code, 9);
}

} // namespace
