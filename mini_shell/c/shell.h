#ifndef MINI_SHELL_SHELL_H
#define MINI_SHELL_SHELL_H

#include <stddef.h>
#include <stdio.h>

/* What is written before every command is read. */
#define SHELL_PROMPT "$ "

/* Prefix on every message the shell itself reports, matching the binary name.
 */
#define SHELL_PROGNAME "mini_shell"

/*
 * Outcome of a shell operation. A nonzero value names the stage that failed.
 * For the stages backed by a libc call (SHELL_ERR_READ, SHELL_ERR_WRITE) the
 * failing call's errno is left in place, so the caller may pair the result with
 * strerror(errno). The remaining stages carry no errno.
 *
 * A command that fails is not one of these: the shell did its job by running
 * it. Those are reported per command as a ShellStatus and never end the loop.
 */
typedef enum {
  SHELL_OK = 0,
  SHELL_ERR_READ,  /* a read error occurred on the command input stream */
  SHELL_ERR_WRITE, /* a write error occurred on the output stream */
  SHELL_ERR_NOMEM, /* out of memory */
} ShellResult;

/*
 * How a command ended.
 *
 * The first two are decoded from the wait status waitpid reports, which is not
 * an exit code: reading it as one would report a command killed by SIGKILL as
 * "exited with status 0". The last three are the ways a command can fail to
 * start at all, and are told apart by the errno execvp left behind - which
 * reaches the parent through the errno pipe in shell_exec_runner, since execvp
 * fails in the child.
 */
typedef enum {
  SHELL_EXITED,         /* ran to completion; code is the exit status */
  SHELL_SIGNALED,       /* killed by a signal; code is the signal number */
  SHELL_NOT_FOUND,      /* execvp: ENOENT; code is unused */
  SHELL_NOT_EXECUTABLE, /* execvp: EACCES; code is unused */
  SHELL_UNRUNNABLE,     /* any other failure to start; code is the errno */
} ShellOutcome;

typedef struct {
  ShellOutcome outcome;
  int code;
} ShellStatus;

/*
 * A command line split into words, laid out the way execvp wants it: argc
 * pointers into the caller's line buffer, followed by a NULL terminator.
 */
typedef struct {
  char **argv;
  size_t argc;
} ShellArgv;

/*
 * shell_split - splits a command line into words on ASCII whitespace.
 *
 * This is the whole of mini_shell's grammar. There is no quoting, no escaping,
 * and no expansion of any kind: a run of whitespace separates two words and
 * every other byte is literal, so `echo a | wc` runs echo with the three
 * arguments "a", "|", and "wc". Splitting is the job system() used to hand to
 * /bin/sh, and taking it back is the point of this port.
 *
 * Input:  line - a NUL-terminated line, modified in place: the whitespace
 *         between words is overwritten with NUL terminators. It must outlive
 *         *out, whose members point into it.
 *
 * Output: SHELL_OK with *out filled in, or SHELL_ERR_NOMEM. A line that is
 *         empty or entirely whitespace yields argc 0 and an argv holding only
 *         the NULL terminator; callers skip such lines before getting here.
 *         Release *out with shell_argv_free.
 */
ShellResult shell_split(char *line, ShellArgv *out);

/* Frees what shell_split allocated. The line it pointed into is untouched. */
void shell_argv_free(ShellArgv *args);

/*
 * Runs one command and returns the wait status waitpid reported, or -1 with
 * errno set if the command could not be started at all.
 *
 * This is the seam that keeps the command loop testable: shell_run never forks
 * anything itself, so a test can supply a runner that records the argv and
 * returns a canned status without spawning a process.
 *
 * Input:  argv - the words of the command, NULL-terminated, as shell_split
 *         produced them. argv[0] is the program.
 *         ctx - the runner's own state, passed through from
 *         ShellOptions.runner_ctx and ignored by shell_exec_runner.
 */
typedef int (*ShellRunner)(char *const argv[], void *ctx);

/*
 * How the command loop behaves.
 *
 * The flag is int, not bool, because this header is compiled in two dialects.
 * The C here is C17 (.bazelrc sets -std=c++20 as a cxxopt, which does not reach
 * C), where bool needs <stdbool.h> and is _Bool; test_shell.c compiles the same
 * header as C++ (-x c++), where bool is a distinct builtin. The extern "C"
 * wrapper there fixes linkage, not layout, so the two would agree only by ABI
 * accident. int is the same type in both.
 */
typedef struct {
  int show_banner;
  ShellRunner runner;
  void *runner_ctx;
} ShellOptions;

/* Returns a short human-readable label for a ShellResult. */
const char *shell_result_str(ShellResult r);

/*
 * shell_decode_status - splits a runner's return value into outcome and code.
 *
 * Pure - the errno is passed in rather than read from the global, so every
 * case is testable without arranging for a real process to be killed or for a
 * fork to fail.
 *
 * Input:  raw - what the runner returned: a wait status in the layout
 *         <sys/wait.h> describes, or -1 if the command never started.
 *         err - the errno the runner left behind. Read only when raw is -1.
 *
 * Output: For raw of -1, SHELL_NOT_FOUND for ENOENT, SHELL_NOT_EXECUTABLE for
 *         EACCES, and SHELL_UNRUNNABLE carrying err for anything else.
 *         Otherwise SHELL_SIGNALED with the signal number for a command killed
 *         by a signal, and SHELL_EXITED with the exit status for one that ran.
 *         A status that is neither an exit nor a signal (a stop, which the
 *         runner waits past) is reported as SHELL_EXITED with code 0, since
 *         there is no code to name.
 */
ShellStatus shell_decode_status(int raw, int err);

/*
 * shell_report_status - writes one line about a command that did not succeed.
 *
 * A clean exit 0 writes nothing: the shell is silent when there is nothing to
 * say, so the only output between prompts is the command's own.
 *
 * The two common ways to fail to start - the program does not exist, and it
 * exists but cannot be executed - are named in mini_shell's own words rather
 * than with strerror. That is deliberate and load-bearing for cross-port
 * parity: every port writes these bytes itself, where the text of an errno
 * differs between them.
 *
 * Input:  err - open, writable FILE* the caller retains ownership of.
 *         status - as returned by shell_decode_status.
 *         program - argv[0], named in the messages about a command that never
 *         started.
 *
 * Output: Returns SHELL_OK, or SHELL_ERR_WRITE with errno as the failing call
 *         left it.
 */
ShellResult shell_report_status(FILE *err, ShellStatus status,
                                const char *program);

/*
 * shell_is_exit_command - reports whether a line asks the shell to quit.
 *
 * Input:  line, len - the line's bytes and their count, with any line
 *         terminator already stripped.
 *
 * Output: Nonzero for "exit" surrounded by any amount of ASCII whitespace.
 *         "EXIT", "exitx", and "exit now" are commands like any other and are
 *         handed to the interpreter; matching them here would quietly diverge
 *         from what a real shell does with them.
 */
int shell_is_exit_command(const char *line, size_t len);

/*
 * shell_is_blank - reports whether a line holds nothing worth running.
 *
 * Output: Nonzero when the line is empty or entirely ASCII whitespace. Such a
 *         line is skipped rather than run: an interpreter would exit 0 for it
 *         anyway, and skipping saves a fork per stray Enter.
 */
int shell_is_blank(const char *line, size_t len);

/*
 * shell_write_banner - writes the startup banner.
 *
 * Output: Returns SHELL_OK, or SHELL_ERR_WRITE with errno as the failing call
 *         left it.
 */
ShellResult shell_write_banner(FILE *out);

/*
 * shell_exec_runner - the ShellRunner that actually runs commands.
 *
 * Forks, execvp's argv in the child, and waits. The only impure function here;
 * everything else is a transformation of its arguments or of the streams it is
 * handed. Pass it from main, not from library code.
 *
 * execvp fails in the child, where the parent cannot see its errno, so the two
 * are joined by a close-on-exec pipe: the child writes the errno and _exits, a
 * successful exec closes the pipe instead, and the parent tells the cases apart
 * by whether the read returned anything. This is the same mechanism Rust's
 * std::process::Command uses internally, and it is what lets all three ports
 * report a missing program identically. Inferring it from an exit status of
 * 127 was the alternative and was rejected: a command that really does exit 127
 * is then indistinguishable from one that never ran.
 *
 * Input:  ctx - unused; present to satisfy the ShellRunner signature.
 *
 * Output: The wait status, undecoded, or -1 with errno set to whatever stopped
 *         the command from starting.
 */
int shell_exec_runner(char *const argv[], void *ctx);

/*
 * shell_run - the command loop.
 *
 * Writes the banner (when opts->show_banner is nonzero), then repeats: write
 * SHELL_PROMPT to out and flush it, read one line from in, and act on it. The
 * flush matters - the command inherits out's file descriptor, so an unflushed
 * prompt would surface after the command's own output.
 *
 * One trailing '\n' is stripped from each line, then one trailing '\r', so CRLF
 * input runs the same commands as LF input. A blank line is skipped, "exit"
 * ends the loop, and a line containing an embedded NUL is refused: execvp takes
 * NUL-terminated strings, so the alternative is silently running a truncated
 * command. Everything else is split into words by shell_split, handed to
 * opts->runner, and its status reported to err.
 *
 * A command that fails does not end the loop and does not change the result;
 * only the shell's own I/O can.
 *
 * Input:  in - the command source; end of input ends the loop, as "exit" does.
 *         out - where the banner and prompts go.
 *         err - where per-command failures are reported.
 *         opts - the banner flag and the runner; runner must not be NULL.
 *
 * Output: Returns SHELL_OK when the loop ended at "exit" or end of input, or
 *         SHELL_ERR_WRITE, SHELL_ERR_READ, or SHELL_ERR_NOMEM.
 */
ShellResult shell_run(FILE *in, FILE *out, FILE *err, const ShellOptions *opts);

#endif
