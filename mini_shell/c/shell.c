#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
    "commands run directly, one program per line; type 'exit' to quit",
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
  }
  return "unknown error";
}

ShellResult shell_split(char *line, ShellArgv *out) {
  out->argv = NULL;
  out->argc = 0;

  /* Two passes: count the words, then allocate exactly the array execvp wants.
   * A line is short and this runs once per command, so the second walk costs
   * nothing worth avoiding a growing realloc for. */
  size_t words = 0;
  for (const char *p = line; *p != '\0';) {
    while (*p != '\0' && isspace((unsigned char)*p))
      p++;
    if (*p == '\0')
      break;
    words++;
    while (*p != '\0' && !isspace((unsigned char)*p))
      p++;
  }

  char **argv = malloc((words + 1) * sizeof(*argv));
  if (argv == NULL)
    return SHELL_ERR_NOMEM;

  size_t n = 0;
  for (char *p = line; *p != '\0';) {
    while (*p != '\0' && isspace((unsigned char)*p))
      *p++ = '\0';
    if (*p == '\0')
      break;
    argv[n++] = p;
    while (*p != '\0' && !isspace((unsigned char)*p))
      p++;
  }
  argv[n] = NULL;

  out->argv = argv;
  out->argc = n;
  return SHELL_OK;
}

void shell_argv_free(ShellArgv *args) {
  free(args->argv);
  args->argv = NULL;
  args->argc = 0;
}

ShellStatus shell_decode_status(int raw, int err) {
  ShellStatus status;
  status.code = 0;

  if (raw == -1) {
    /* The command never started. Which errno it was is the difference between
     * the two messages a user can act on and the catch-all one they cannot. */
    if (err == ENOENT)
      status.outcome = SHELL_NOT_FOUND;
    else if (err == EACCES)
      status.outcome = SHELL_NOT_EXECUTABLE;
    else {
      status.outcome = SHELL_UNRUNNABLE;
      status.code = err;
    }
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

ShellResult shell_report_status(FILE *err, ShellStatus status,
                                const char *program) {
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
  /* The two failures worth naming the program for, in mini_shell's own words
   * rather than strerror's. Writing the bytes here is what keeps the three
   * ports byte-identical on the most ordinary failure there is. */
  case SHELL_NOT_FOUND:
    written =
        fprintf(err, "%s: %s: command not found\n", SHELL_PROGNAME, program);
    break;
  case SHELL_NOT_EXECUTABLE:
    written =
        fprintf(err, "%s: %s: permission denied\n", SHELL_PROGNAME, program);
    break;
  default:
    /* Something else stopped the command from starting - a failed fork, an
     * exhausted process table - so the errno is about the shell rather than
     * about the command, which never ran. */
    written = fprintf(err, "%s: failed to run command: %s\n", SHELL_PROGNAME,
                      strerror(status.code));
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

/* Closes both ends of a pipe without disturbing the errno a caller is about to
 * report. close() can fail, and would otherwise overwrite it. */
static void close_pipe(int fds[2]) {
  int saved = errno;
  close(fds[0]);
  close(fds[1]);
  errno = saved;
}

int shell_exec_runner(char *const argv[], void *ctx) {
  (void)ctx;

  /* The channel the child reports a failed exec on. pipe + fcntl rather than
   * pipe2: pipe2 sits behind __USE_GNU in glibc's <unistd.h> and would need
   * _GNU_SOURCE, while these two are plain POSIX. Only the write end needs
   * FD_CLOEXEC - that is the whole trick, since a successful exec then closes
   * it and the parent's read sees end of file instead of an errno. */
  int fds[2];
  if (pipe(fds) != 0)
    return -1;
  if (fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    close_pipe(fds);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close_pipe(fds);
    return -1;
  }

  if (pid == 0) {
    close(fds[0]);
    execvp(argv[0], argv);
    /* Only reached if the exec failed. The write is best effort: if it fails
     * there is nothing left to report the failure with, and the cast silences
     * the warn_unused_result on write that -Werror would otherwise make fatal.
     */
    int failure = errno;
    (void)!write(fds[1], &failure, sizeof failure);
    /* _exit, never exit: this process shares the parent's stdio buffers, and
     * flushing them here would print the parent's pending output a second
     * time. */
    _exit(127);
  }

  /* The parent's copy of the write end must go, or the read below would block
   * waiting on a descriptor the parent itself is holding open. */
  close(fds[1]);

  int child_errno = 0;
  ssize_t n;
  do {
    n = read(fds[0], &child_errno, sizeof child_errno);
  } while (n < 0 && errno == EINTR);
  close(fds[0]);

  /* Reap the child either way: it exists even when the exec failed. */
  int raw;
  while (waitpid(pid, &raw, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }

  if (n == (ssize_t)sizeof child_errno) {
    errno = child_errno;
    return -1;
  }
  return raw;
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

    /* Splitting is mini_shell's whole grammar, and it is done here rather than
     * by an interpreter: the runner is handed a program and its arguments, not
     * a command line. The line is blank-checked above, so argc is at least 1
     * and argv[0] is a program to name in any diagnostic. */
    ShellArgv args;
    ShellResult split = shell_split(line, &args);
    if (split != SHELL_OK) {
      result = split;
      break;
    }

    errno = 0;
    int raw = opts->runner(args.argv, opts->runner_ctx);
    ShellStatus status = shell_decode_status(raw, errno);
    ShellResult reported = shell_report_status(err, status, args.argv[0]);
    shell_argv_free(&args);
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
