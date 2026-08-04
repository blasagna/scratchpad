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
 * A command that fails is not one of these: the command ran, so the shell did
 * its job. Those are reported per command as a ShellStatus and never end the
 * loop.
 */
typedef enum {
  SHELL_OK = 0,
  SHELL_ERR_READ,     /* a read error occurred on the command input stream */
  SHELL_ERR_WRITE,    /* a write error occurred on the output stream */
  SHELL_ERR_NOMEM,    /* out of memory */
  SHELL_ERR_NO_SHELL, /* system() reported no command interpreter is available
                       */
} ShellResult;

/*
 * How a command ended, decoded from the raw value system() returns. The raw
 * value is a wait status, not an exit code: reading it as one would report a
 * command killed by SIGKILL as "exited with status 0".
 */
typedef enum {
  SHELL_EXITED,     /* ran to completion; code is the exit status */
  SHELL_SIGNALED,   /* killed by a signal; code is the signal number */
  SHELL_UNRUNNABLE, /* system() could not fork or wait; code is unused */
} ShellOutcome;

typedef struct {
  ShellOutcome outcome;
  int code;
} ShellStatus;

/*
 * Runs one command and returns exactly what system() returns.
 *
 * This is the seam that keeps the command loop testable: shell_run never calls
 * system() itself, so a test can supply a runner that records the command and
 * returns a canned status without forking anything.
 *
 * Input:  command - a NUL-terminated command line.
 *         ctx - the runner's own state, passed through from
 *         ShellOptions.runner_ctx and ignored by shell_system_runner.
 */
typedef int (*ShellRunner)(const char *command, void *ctx);

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
 * shell_decode_status - splits system()'s return value into outcome and code.
 *
 * Pure, so every case is testable without arranging for a real process to be
 * killed. system() returns -1 when it could not run the command at all, and
 * otherwise a wait status in the layout <sys/wait.h> describes.
 *
 * Input:  raw - the value system() returned.
 *
 * Output: SHELL_UNRUNNABLE for -1; SHELL_SIGNALED with the signal number for a
 *         command killed by a signal; SHELL_EXITED with the exit status
 *         otherwise. A status that is neither an exit nor a signal (a stop,
 *         which system() waits past) is reported as SHELL_EXITED with code 0,
 *         since there is no code to name.
 */
ShellStatus shell_decode_status(int raw);

/*
 * shell_report_status - writes one line about a command that did not succeed.
 *
 * A clean exit 0 writes nothing: the shell is silent when there is nothing to
 * say, so the only output between prompts is the command's own. Status 127 is
 * not special-cased either - the interpreter has already printed its own
 * "command not found", and this line names the status behind it.
 *
 * Input:  err - open, writable FILE* the caller retains ownership of.
 *         status - as returned by shell_decode_status.
 *
 * Output: Returns SHELL_OK, or SHELL_ERR_WRITE with errno as the failing call
 *         left it.
 */
ShellResult shell_report_status(FILE *err, ShellStatus status);

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
 * shell_system_runner - the ShellRunner that actually runs commands.
 *
 * The only impure function here; everything else is a transformation of its
 * arguments or of the streams it is handed. Pass it from main, not from
 * library code.
 *
 * Input:  ctx - unused; present to satisfy the ShellRunner signature.
 *
 * Output: system()'s return value, undecoded.
 */
int shell_system_runner(const char *command, void *ctx);

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
 * ends the loop, and a line containing an embedded NUL is refused: system()
 * takes a NUL-terminated string, so the alternative is silently running a
 * truncated command. Everything else goes to opts->runner and its status is
 * reported to err.
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
