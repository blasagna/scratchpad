#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * The startup banner. Written as a table so the art stays readable in the
 * source, and with fputs rather than printf so a '%' in a future banner is not
 * a format specifier. Every backslash is doubled: these are C string literals,
 * and the compiler would otherwise read "\_" as an unknown escape.
 */
static const char *const kBanner[] = {
    " __  __  _        _   ____   _            _  _ ",
    "|  \\/  |(_) _ __  (_) / ___| | |__    ___ | || |",
    "| |\\/| || || '_ \\ | | \\___ \\ | '_ \\  / _ \\| || |",
    "| |  | || || | | || |  ___) || | | ||  __/| || |",
    "|_|  |_||_||_| |_||_| |____/ |_| |_| \\___||_||_|",
    "",
    "commands run through the system shell; type 'exit' to quit",
    "",
};

/* The word that ends the loop, matched after trimming whitespace. */
static const char *const kExitWord = "exit";

/*
 * Writes n bytes to out, returning 1 on success and 0 on failure. Callers stop
 * at the first failure so errno still belongs to the call that failed rather
 * than to a later write against an already-errored stream.
 */
static int put_bytes(FILE *out, const char *data, size_t n) {
  return n == 0 || fwrite(data, 1, n, out) == n;
}

static int put_str(FILE *out, const char *s) {
  return put_bytes(out, s, strlen(s));
}

/*
 * Trims ASCII whitespace from both ends, narrowing *line and *len in place.
 * isspace is locale-dependent in principle; nothing here calls setlocale, so
 * the "C" locale is in effect and the set is the seven ASCII space characters.
 */
static void trim(const char **line, size_t *len) {
  while (*len > 0 && isspace((unsigned char)(*line)[0])) {
    (*line)++;
    (*len)--;
  }
  while (*len > 0 && isspace((unsigned char)(*line)[*len - 1]))
    (*len)--;
}

const char *shell_result_str(ShellResult r) {
  switch (r) {
  case SHELL_OK:
    return "success";
  case SHELL_ERR_READ:
    return "error reading command input";
  case SHELL_ERR_WRITE:
    return "error writing output";
  case SHELL_ERR_NOMEM:
    return "out of memory";
  case SHELL_ERR_NO_SHELL:
    return "no command interpreter available";
  }
  return "unknown error";
}

ShellStatus shell_decode_status(int raw) {
  ShellStatus status;
  status.code = 0;

  if (raw == -1) {
    status.outcome = SHELL_UNRUNNABLE;
    return status;
  }
  /* Signals first: a wait status is not an exit code, and reading one as the
   * other would report a command killed by SIGKILL as "exited with status 0".
   */
  if (WIFSIGNALED(raw)) {
    status.outcome = SHELL_SIGNALED;
    status.code = WTERMSIG(raw);
    return status;
  }
  status.outcome = SHELL_EXITED;
  if (WIFEXITED(raw))
    status.code = WEXITSTATUS(raw);
  return status;
}

ShellResult shell_report_status(FILE *err, ShellStatus status) {
  int written;
  switch (status.outcome) {
  case SHELL_EXITED:
    if (status.code == 0)
      return SHELL_OK;
    written = fprintf(err, "%s: command exited with status %d\n",
                      SHELL_PROGNAME, status.code);
    break;
  case SHELL_SIGNALED:
    written = fprintf(err, "%s: command terminated by signal %d\n",
                      SHELL_PROGNAME, status.code);
    break;
  default:
    /* system() failed to fork or wait, so errno is about the shell, not the
     * command; the command never ran. */
    written = fprintf(err, "%s: failed to run command: %s\n", SHELL_PROGNAME,
                      strerror(errno));
    break;
  }
  return written < 0 ? SHELL_ERR_WRITE : SHELL_OK;
}

int shell_is_exit_command(const char *line, size_t len) {
  trim(&line, &len);
  return len == strlen(kExitWord) && memcmp(line, kExitWord, len) == 0;
}

int shell_is_blank(const char *line, size_t len) {
  trim(&line, &len);
  return len == 0;
}

ShellResult shell_write_banner(FILE *out) {
  for (size_t i = 0; i < sizeof(kBanner) / sizeof(kBanner[0]); i++) {
    if (!put_str(out, kBanner[i]) || !put_str(out, "\n"))
      return SHELL_ERR_WRITE;
  }
  return SHELL_OK;
}

int shell_system_runner(const char *command, void *ctx) {
  (void)ctx;
  return system(command);
}

/* Writes the prompt and flushes it. The command inherits out's file descriptor,
 * so an unflushed prompt would surface after the command's own output. */
static int put_prompt(FILE *out) {
  return put_str(out, SHELL_PROMPT) && fflush(out) == 0;
}

ShellResult shell_run(FILE *in, FILE *out, FILE *err,
                      const ShellOptions *opts) {
  if (opts->show_banner) {
    ShellResult banner = shell_write_banner(out);
    if (banner != SHELL_OK)
      return banner;
  }

  char *line = NULL;
  size_t cap = 0;
  ShellResult result = SHELL_OK;

  for (;;) {
    if (!put_prompt(out)) {
      result = SHELL_ERR_WRITE;
      break;
    }

    /* getline is POSIX rather than ISO C, but it reports a byte count, so a
     * line containing a NUL arrives intact and can be refused below rather
     * than silently truncated. */
    ssize_t n = getline(&line, &cap, in);
    if (n < 0) {
      /* getline returns -1 for end of input, a read error, and an allocation
       * failure alike; only the stream flags tell them apart. */
      if (ferror(in))
        result = SHELL_ERR_READ;
      else if (!feof(in))
        result = SHELL_ERR_NOMEM;
      else if (!put_str(out, "\n") || fflush(out) != 0)
        /* End of input: leave the cursor on a fresh line, since the prompt
         * just written is the last thing on this one. */
        result = SHELL_ERR_WRITE;
      break;
    }

    size_t len = (size_t)n;
    /* Strip one '\n', then one '\r', so CRLF input runs the same commands as
     * LF input. A '\r' anywhere else belongs to the command. */
    if (len > 0 && line[len - 1] == '\n')
      len--;
    if (len > 0 && line[len - 1] == '\r')
      len--;

    if (shell_is_blank(line, len))
      continue;
    if (shell_is_exit_command(line, len))
      break;

    if (memchr(line, '\0', len) != NULL) {
      if (fprintf(err, "%s: command contains a NUL byte\n", SHELL_PROGNAME) <
          0) {
        result = SHELL_ERR_WRITE;
        break;
      }
      continue;
    }

    /* getline's buffer holds n bytes plus a NUL, so len is always in range. */
    line[len] = '\0';
    ShellStatus status =
        shell_decode_status(opts->runner(line, opts->runner_ctx));
    ShellResult reported = shell_report_status(err, status);
    if (reported != SHELL_OK) {
      result = reported;
      break;
    }
  }

  int saved = errno;
  free(line);
  errno = saved;
  return result;
}
