#include <gtest/gtest.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

extern "C" {
#include "shell.h"
}

/*
 * A wait status as system() returns one. The encoding is not in the standard —
 * <sys/wait.h> only promises the WIF* macros — so these two build the layout
 * every platform this runs on uses, and RealSystem.EncodingMatchesTheMacros
 * below checks that assumption against the libc actually linked in.
 */
static int exited(int code) { return code << 8; }
static int signaled(int sig) { return sig; }

/* Runs body against an in-memory output stream and returns what it wrote. */
template<typename F> static std::string captured(F body) {
  char buf[8192];
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

/*
 * A ShellRunner that records what it was asked to run and hands back canned
 * statuses, so the command loop is exercised without forking anything. Statuses
 * are consumed in order; once they run out, every further command succeeds.
 */
namespace {
  struct FakeRunner {
    std::vector<std::string> commands;
    std::vector<int> statuses;
    size_t next = 0;
  };
} // namespace

static int fake_run(const char *command, void *ctx) {
  FakeRunner *fake = static_cast<FakeRunner *>(ctx);
  fake->commands.push_back(std::string(command));
  if (fake->next < fake->statuses.size())
    return fake->statuses[fake->next++];
  return exited(0);
}

static ShellOptions fake_options(FakeRunner *fake) {
  ShellOptions opts;
  opts.show_banner = 0;
  opts.runner = fake_run;
  opts.runner_ctx = fake;
  return opts;
}

/*
 * Result of one shell_run over a fixed input: everything a caller can observe.
 * err is captured separately from out because the two streams are the contract:
 * commands and prompts on one, the shell's own complaints on the other.
 */
namespace {
  struct RunOutput {
    ShellResult result = SHELL_OK;
    std::string out;
    std::string err;
  };
} // namespace

static RunOutput run_shell(const std::string &input, const ShellOptions *opts) {
  RunOutput captured_run;
  captured_run.out = captured([&](FILE *out) {
    captured_run.err = captured([&](FILE *err) {
      FILE *in = make_input(input);
      if (in == nullptr)
        return;
      captured_run.result = shell_run(in, out, err, opts);
      EXPECT_EQ(fclose(in), 0);
    });
  });
  return captured_run;
}

/* --- shell_decode_status --- */

TEST(DecodeStatus, CleanExitIsExitedZero) {
  ShellStatus status = shell_decode_status(exited(0));
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 0);
}

TEST(DecodeStatus, NonzeroExitKeepsItsCode) {
  ShellStatus status = shell_decode_status(exited(3));
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 3);
}

TEST(DecodeStatus, CommandNotFoundIsAnOrdinaryExit) {
  /* 127 is what the interpreter exits with when it cannot find the command. It
   * is not special-cased: the interpreter has already said so itself. */
  ShellStatus status = shell_decode_status(exited(127));
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 127);
}

TEST(DecodeStatus, SignalIsNotReadAsAnExitCode) {
  ShellStatus status = shell_decode_status(signaled(9));
  EXPECT_EQ(status.outcome, SHELL_SIGNALED);
  EXPECT_EQ(status.code, 9);
}

TEST(DecodeStatus, MinusOneMeansTheCommandNeverRan) {
  ShellStatus status = shell_decode_status(-1);
  EXPECT_EQ(status.outcome, SHELL_UNRUNNABLE);
}

/* --- shell_report_status --- */

static std::string reported(ShellStatus status) {
  return captured([&](FILE *err) {
    EXPECT_EQ(shell_report_status(err, status), SHELL_OK);
  });
}

TEST(ReportStatus, SuccessIsSilent) {
  ShellStatus status = {SHELL_EXITED, 0};
  EXPECT_EQ(reported(status), "");
}

TEST(ReportStatus, NamesTheExitStatus) {
  ShellStatus status = {SHELL_EXITED, 3};
  EXPECT_EQ(reported(status), "mini_shell: command exited with status 3\n");
}

TEST(ReportStatus, NamesTheSignal) {
  ShellStatus status = {SHELL_SIGNALED, 9};
  EXPECT_EQ(reported(status), "mini_shell: command terminated by signal 9\n");
}

TEST(ReportStatus, ReportsAnUnrunnableCommandWithErrno) {
  ShellStatus status = {SHELL_UNRUNNABLE, 0};
  errno = EAGAIN;
  EXPECT_EQ(reported(status),
            std::string("mini_shell: failed to run command: ") +
                strerror(EAGAIN) + "\n");
}

/* --- shell_is_exit_command / shell_is_blank --- */

static int is_exit(const std::string &line) {
  return shell_is_exit_command(line.data(), line.size());
}

static int is_blank(const std::string &line) {
  return shell_is_blank(line.data(), line.size());
}

TEST(ExitCommand, MatchesTheBareWord) { EXPECT_TRUE(is_exit("exit")); }

TEST(ExitCommand, IgnoresSurroundingWhitespace) {
  EXPECT_TRUE(is_exit("  exit"));
  EXPECT_TRUE(is_exit("exit\t"));
  EXPECT_TRUE(is_exit(" \texit \t"));
}

TEST(ExitCommand, DoesNotMatchOtherSpellings) {
  /* Each of these is a command like any other and goes to the interpreter;
   * matching them here would quietly diverge from what a real shell does. */
  EXPECT_FALSE(is_exit("EXIT"));
  EXPECT_FALSE(is_exit("exitx"));
  EXPECT_FALSE(is_exit("exit 3"));
  EXPECT_FALSE(is_exit("exi"));
  EXPECT_FALSE(is_exit(""));
}

TEST(Blank, EmptyAndWhitespaceOnly) {
  EXPECT_TRUE(is_blank(""));
  EXPECT_TRUE(is_blank("   "));
  EXPECT_TRUE(is_blank("\t \t"));
  EXPECT_FALSE(is_blank("ls"));
  EXPECT_FALSE(is_blank("  ls  "));
}

/* --- shell_run --- */

TEST(Run, PassesCommandsToTheRunnerInOrder) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("echo one\necho two\nexit\n", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  ASSERT_EQ(fake.commands.size(), 2u);
  EXPECT_EQ(fake.commands[0], "echo one");
  EXPECT_EQ(fake.commands[1], "echo two");
}

TEST(Run, PromptsOncePerCommandAndOnceMoreForExit) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("ls\nexit\n", &opts);

  EXPECT_EQ(run.out, "$ $ ");
  EXPECT_EQ(run.err, "");
}

TEST(Run, ExitStopsTheLoopAndLeavesLaterLinesUnread) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("exit\nrm -rf /\n", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  EXPECT_TRUE(fake.commands.empty());
}

TEST(Run, EndOfInputStopsTheLoopAndEndsTheLine) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("ls\n", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  ASSERT_EQ(fake.commands.size(), 1u);
  /* The prompt for the command, then the prompt that end of input answered,
   * then a newline so the cursor does not stop on the prompt's line. */
  EXPECT_EQ(run.out, "$ $ \n");
}

TEST(Run, RunsAFinalLineWithNoNewline) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  run_shell("echo hi", &opts);

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], "echo hi");
}

TEST(Run, ExitNeedsNoTrailingNewline) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("exit", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  EXPECT_TRUE(fake.commands.empty());
  /* The loop ended at "exit", not at end of input, so no closing newline. */
  EXPECT_EQ(run.out, "$ ");
}

TEST(Run, BlankLinesArePromptedForButNotRun) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("\n   \nls\nexit\n", &opts);

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], "ls");
  EXPECT_EQ(run.out, "$ $ $ $ ");
}

TEST(Run, StripsCrlfButKeepsAnInteriorCarriageReturn) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  run_shell("echo a\rb\r\n", &opts);

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], "echo a\rb");
}

TEST(Run, ReportsAFailedCommandAndKeepsGoing) {
  FakeRunner fake;
  fake.statuses = {exited(3), exited(0)};
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("false\ntrue\nexit\n", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  EXPECT_EQ(fake.commands.size(), 2u);
  EXPECT_EQ(run.err, "mini_shell: command exited with status 3\n");
}

TEST(Run, ReportsASignaledCommand) {
  FakeRunner fake;
  fake.statuses = {signaled(9)};
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("sleep 100\nexit\n", &opts);

  EXPECT_EQ(run.err, "mini_shell: command terminated by signal 9\n");
}

TEST(Run, RefusesALineContainingANulByte) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  /* system() takes a NUL-terminated string, so running this would run "echo a"
   * and silently drop the rest. */
  RunOutput run = run_shell(std::string("echo a\0rm -rf /\nexit\n", 21), &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_EQ(run.err, "mini_shell: command contains a NUL byte\n");
}

TEST(Run, PassesNonAsciiBytesThrough) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  run_shell("echo \xc3\xa9\nexit\n", &opts);

  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], "echo \xc3\xa9");
}

TEST(Run, EmptyInputStillPromptsOnce) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_EQ(run.out, "$ \n");
}

TEST(Run, WritesTheBannerBeforeTheFirstPromptWhenAsked) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  opts.show_banner = 1;
  RunOutput run = run_shell("exit\n", &opts);

  std::string banner =
      captured([](FILE *out) { EXPECT_EQ(shell_write_banner(out), SHELL_OK); });
  EXPECT_FALSE(banner.empty());
  EXPECT_EQ(run.out, banner + "$ ");
}

TEST(Run, ReportsAReadErrorOnTheInputStream) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  /* A stream opened for writing fails on read, which is the only way to get a
   * read error out of an in-memory stream. */
  char buf[16];
  FILE *unreadable = fmemopen(buf, sizeof(buf), "w");
  ASSERT_NE(unreadable, nullptr);

  ShellResult result = SHELL_OK;
  captured([&](FILE *out) {
    captured(
        [&](FILE *err) { result = shell_run(unreadable, out, err, &opts); });
  });
  EXPECT_EQ(result, SHELL_ERR_READ);
  EXPECT_EQ(fclose(unreadable), 0);
}

TEST(Run, ReportsAWriteErrorOnThePrompt) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  /* One byte of room is not enough for the two-byte prompt. */
  char buf[1];
  FILE *full = fmemopen(buf, sizeof(buf), "w");
  ASSERT_NE(full, nullptr);

  FILE *in = make_input("exit\n");
  std::string err_text = captured([&](FILE *err) {
    EXPECT_EQ(shell_run(in, full, err, &opts), SHELL_ERR_WRITE);
  });
  EXPECT_EQ(err_text, "");
  EXPECT_EQ(fclose(in), 0);
  EXPECT_EQ(fclose(full), 0);
}

/* --- shell_result_str --- */

TEST(ResultStr, LabelsEveryResult) {
  EXPECT_STREQ(shell_result_str(SHELL_OK), "success");
  EXPECT_STREQ(shell_result_str(SHELL_ERR_READ), "error reading command input");
  EXPECT_STREQ(shell_result_str(SHELL_ERR_WRITE), "error writing output");
  EXPECT_STREQ(shell_result_str(SHELL_ERR_NOMEM), "out of memory");
  EXPECT_STREQ(shell_result_str(SHELL_ERR_NO_SHELL),
               "no command interpreter available");
}

TEST(ResultStr, FallsBackForAnUnknownValue) {
  EXPECT_STREQ(shell_result_str(static_cast<ShellResult>(999)),
               "unknown error");
}

/* --- shell_system_runner --- */

TEST(RealSystem, EncodingMatchesTheMacros) {
  /* The one test that really forks. It pins the assumption behind exited() and
   * signaled() above: if this libc encoded a wait status differently, every
   * status this suite builds by hand would be meaningless. */
  ShellStatus status = shell_decode_status(shell_system_runner("exit 3", NULL));
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 3);

  status = shell_decode_status(shell_system_runner("exit 0", NULL));
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 0);

  status = shell_decode_status(shell_system_runner("kill -9 $$", NULL));
  EXPECT_EQ(status.outcome, SHELL_SIGNALED);
  EXPECT_EQ(status.code, 9);
}
