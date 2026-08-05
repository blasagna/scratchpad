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
 * A wait status as waitpid reports one. The encoding is not in the standard —
 * <sys/wait.h> only promises the WIF* macros — so these two build the layout
 * every platform this runs on uses, and RealExec.EncodingMatchesTheMacros
 * below checks that assumption against the libc actually linked in.
 */
static int exited(int code) { return code << 8; }
static int signaled(int sig) { return sig; }

/* One command as the runner receives it: a program and its arguments. */
using Argv = std::vector<std::string>;

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
    std::vector<Argv> commands;
    std::vector<int> statuses;
    size_t next = 0;
    /* The errno a canned status of -1 arrives with, since that is half of what
     * a real runner reports when the command never started. */
    int fail_errno = 0;
  };
} // namespace

static int fake_run(char *const argv[], void *ctx) {
  FakeRunner *fake = static_cast<FakeRunner *>(ctx);
  Argv words;
  for (char *const *word = argv; *word != nullptr; word++)
    words.push_back(std::string(*word));
  fake->commands.push_back(words);

  int status = exited(0);
  if (fake->next < fake->statuses.size())
    status = fake->statuses[fake->next++];
  if (status == -1)
    errno = fake->fail_errno;
  return status;
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

/* --- shell_split --- */

static Argv split(const std::string &line) {
  /* shell_split writes NULs into the line, so it gets a copy it may chew on. */
  std::vector<char> buffer(line.begin(), line.end());
  buffer.push_back('\0');

  ShellArgv args;
  EXPECT_EQ(shell_split(buffer.data(), &args), SHELL_OK);

  Argv words;
  for (size_t i = 0; i < args.argc; i++)
    words.push_back(std::string(args.argv[i]));
  /* The array execvp wants is NULL-terminated one past the last word. */
  EXPECT_EQ(args.argv[args.argc], nullptr);
  shell_argv_free(&args);
  return words;
}

TEST(Split, OneWordIsTheProgramAlone) { EXPECT_EQ(split("ls"), Argv{"ls"}); }

TEST(Split, LaterWordsAreArguments) {
  EXPECT_EQ(split("echo hello world"), (Argv{"echo", "hello", "world"}));
}

TEST(Split, RunsOfWhitespaceSeparateExactlyOnce) {
  /* Two arguments, not five: the empty strings between the spaces are not
   * words. A shell that passed them along would hand echo blank arguments. */
  EXPECT_EQ(split("echo   a  \t b"), (Argv{"echo", "a", "b"}));
}

TEST(Split, SurroundingWhitespaceIsDropped) {
  EXPECT_EQ(split("  \t ls -l \t "), (Argv{"ls", "-l"}));
}

TEST(Split, VerticalTabAndFormFeedSeparateToo) {
  /* The same set trim() uses, which is isspace in the "C" locale. */
  EXPECT_EQ(split("echo\va\fb"), (Argv{"echo", "a", "b"}));
}

TEST(Split, NothingToSplitYieldsNoWords) {
  EXPECT_TRUE(split("").empty());
  EXPECT_TRUE(split("   \t ").empty());
}

TEST(Split, ShellMetacharactersAreOrdinaryWords) {
  /* The whole of the grammar: whitespace separates, everything else is a byte
   * in a word. There is no interpreter left to give these any meaning. */
  EXPECT_EQ(split("echo a | wc"), (Argv{"echo", "a", "|", "wc"}));
  EXPECT_EQ(split("echo * $HOME > out"),
            (Argv{"echo", "*", "$HOME", ">", "out"}));
  EXPECT_EQ(split("echo \"a b\""), (Argv{"echo", "\"a", "b\""}));
}

TEST(Split, NonAsciiBytesSurvive) {
  EXPECT_EQ(split("echo \xc3\xa9"), (Argv{"echo", "\xc3\xa9"}));
}

/* --- shell_decode_status --- */

TEST(DecodeStatus, CleanExitIsExitedZero) {
  ShellStatus status = shell_decode_status(exited(0), 0);
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 0);
}

TEST(DecodeStatus, NonzeroExitKeepsItsCode) {
  ShellStatus status = shell_decode_status(exited(3), 0);
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 3);
}

TEST(DecodeStatus, AnExitOf127IsNotSpecialCased) {
  /* It used to be how the interpreter said "command not found". Nothing says
   * that now — a missing program never reaches waitpid at all — so 127 is
   * whatever the command chose to exit with, like any other code. */
  ShellStatus status = shell_decode_status(exited(127), 0);
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 127);
}

TEST(DecodeStatus, SignalIsNotReadAsAnExitCode) {
  ShellStatus status = shell_decode_status(signaled(9), 0);
  EXPECT_EQ(status.outcome, SHELL_SIGNALED);
  EXPECT_EQ(status.code, 9);
}

TEST(DecodeStatus, MissingProgramIsToldApartByErrno) {
  ShellStatus status = shell_decode_status(-1, ENOENT);
  EXPECT_EQ(status.outcome, SHELL_NOT_FOUND);
}

TEST(DecodeStatus, UnexecutableProgramIsToldApartByErrno) {
  ShellStatus status = shell_decode_status(-1, EACCES);
  EXPECT_EQ(status.outcome, SHELL_NOT_EXECUTABLE);
}

TEST(DecodeStatus, AnyOtherErrnoIsUnrunnableAndIsCarried) {
  /* A failed fork lands here, and the errno is carried rather than left in the
   * global for the reporting to pick up later. */
  ShellStatus status = shell_decode_status(-1, EAGAIN);
  EXPECT_EQ(status.outcome, SHELL_UNRUNNABLE);
  EXPECT_EQ(status.code, EAGAIN);
}

/* --- shell_report_status --- */

static std::string reported(ShellStatus status,
                            const char *program = "acommand") {
  return captured([&](FILE *err) {
    EXPECT_EQ(shell_report_status(err, status, program), SHELL_OK);
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

TEST(ReportStatus, NamesAMissingProgram) {
  /* mini_shell's own words, not strerror's: every port writes these bytes
   * itself, which is what keeps them byte-identical. */
  ShellStatus status = {SHELL_NOT_FOUND, 0};
  EXPECT_EQ(reported(status, "nosuchcmd"),
            "mini_shell: nosuchcmd: command not found\n");
}

TEST(ReportStatus, NamesAProgramItMayNotExecute) {
  ShellStatus status = {SHELL_NOT_EXECUTABLE, 0};
  EXPECT_EQ(reported(status, "/etc/passwd"),
            "mini_shell: /etc/passwd: permission denied\n");
}

TEST(ReportStatus, ReportsAnUnrunnableCommandWithErrno) {
  /* The residual case, and the only message here that borrows libc's wording.
   */
  ShellStatus status = {SHELL_UNRUNNABLE, EAGAIN};
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
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "one"}));
  EXPECT_EQ(fake.commands[1], (Argv{"echo", "two"}));
}

TEST(Run, HandsTheRunnerASplitLineRatherThanACommandLine) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  run_shell("  ls   -l  /tmp \nexit\n", &opts);

  /* Surrounding and interior whitespace is gone by the time the runner sees
   * it: splitting is mini_shell's job now, not an interpreter's. */
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"ls", "-l", "/tmp"}));
}

TEST(Run, TreatsShellMetacharactersAsOrdinaryArguments) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  run_shell("echo a | wc\nexit\n", &opts);

  /* No pipe: echo is run once, with three arguments. */
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "a", "|", "wc"}));
}

TEST(Run, ReportsAMissingProgramByName) {
  FakeRunner fake;
  /* The runner returns -1 with errno set, exactly as shell_exec_runner does
   * when the child's execvp failed. */
  fake.statuses = {-1};
  fake.fail_errno = ENOENT;
  ShellOptions opts = fake_options(&fake);
  RunOutput run = run_shell("nosuchcmd arg\nexit\n", &opts);

  EXPECT_EQ(run.result, SHELL_OK);
  EXPECT_EQ(run.err, "mini_shell: nosuchcmd: command not found\n");
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
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "hi"}));
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
  EXPECT_EQ(fake.commands[0], Argv{"ls"});
  EXPECT_EQ(run.out, "$ $ $ $ ");
}

TEST(Run, StripsCrlfAndSplitsOnAnInteriorCarriageReturn) {
  FakeRunner fake;
  ShellOptions opts = fake_options(&fake);
  run_shell("echo a\rb\r\n", &opts);

  /* The trailing "\r\n" is a line terminator and is stripped. The interior
   * '\r' is ASCII whitespace like any other, so it separates two arguments —
   * where the interpreter used to receive it inside the command line. */
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "a", "b"}));
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
  /* execvp takes NUL-terminated strings, so running this would run "echo a"
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
  EXPECT_EQ(fake.commands[0], (Argv{"echo", "\xc3\xa9"}));
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
}

TEST(ResultStr, FallsBackForAnUnknownValue) {
  EXPECT_STREQ(shell_result_str(static_cast<ShellResult>(999)),
               "unknown error");
}

/* --- shell_exec_runner --- */

/* Runs argv for real and decodes the result. The tests below can hand the
 * runner an argv the splitter could never produce, which is how "exit 3"
 * arrives as one argument here. */
static ShellStatus really_run(std::vector<const char *> argv) {
  argv.push_back(nullptr);
  errno = 0;
  int raw = shell_exec_runner(const_cast<char *const *>(argv.data()), NULL);
  return shell_decode_status(raw, errno);
}

TEST(RealExec, EncodingMatchesTheMacros) {
  /* The one test that really forks. It pins the assumption behind exited() and
   * signaled() above: if this libc encoded a wait status differently, every
   * status this suite builds by hand would be meaningless. */
  ShellStatus status = really_run({"/bin/sh", "-c", "exit 3"});
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 3);

  status = really_run({"/bin/sh", "-c", "exit 0"});
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 0);

  status = really_run({"/bin/sh", "-c", "kill -9 $$"});
  EXPECT_EQ(status.outcome, SHELL_SIGNALED);
  EXPECT_EQ(status.code, 9);
}

TEST(RealExec, FindsAProgramOnPath) {
  /* No absolute path: execvp searches PATH, which is what makes "ls" work at
   * the prompt without the shell that used to do the looking. */
  ShellStatus status = really_run({"true"});
  EXPECT_EQ(status.outcome, SHELL_EXITED);
  EXPECT_EQ(status.code, 0);
}

TEST(RealExec, ReportsAMissingProgramThroughTheErrnoPipe) {
  /* The exec fails in the child, so this errno crossed a pipe to get here.
   * Without that channel the parent would see only an exit status of 127 and
   * could not tell it from a command that really exited 127. */
  ShellStatus status = really_run({"nosuchcommand_xyzzy"});
  EXPECT_EQ(status.outcome, SHELL_NOT_FOUND);
}

TEST(RealExec, ReportsAProgramItMayNotExecute) {
  ShellStatus status = really_run({"/etc/passwd"});
  EXPECT_EQ(status.outcome, SHELL_NOT_EXECUTABLE);
}
