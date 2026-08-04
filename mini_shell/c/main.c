#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

static void print_help(void) {
  printf("usage: mini_shell [options]\n");
  printf("       mini_shell -h | --help\n");
  printf("\n");
  printf("A prototype shell. Prints a '$' prompt, reads one command per "
         "line,\n");
  printf("hands it to the system command interpreter, and reports the exit "
         "status\n");
  printf("of any command that does not succeed. Repeats until you type 'exit' "
         "or\n");
  printf("close the input.\n");
  printf("\n");
  printf(
      "Every command runs in a fresh subshell, so state a command sets --\n");
  printf("the working directory, an environment variable -- is gone by the "
         "next\n");
  printf("prompt. 'cd' therefore appears to do nothing.\n");
  printf("\n");
  printf("Options:\n");
  printf("      --no-banner  skip the startup banner\n");
  printf("  -h, --help       show this help\n");
  printf("\n");
  printf("Commands come from stdin, so the prompt and banner are printed "
         "whether\n");
  printf("or not that is a terminal.\n");
}

static void print_usage_error(void) {
  fprintf(stderr, "usage: mini_shell [options]\n");
  fprintf(stderr, "       mini_shell --help\n");
}

int main(int argc, char *argv[]) {
  ShellOptions opts = {
      .show_banner = 1,
      .runner = shell_system_runner,
      .runner_ctx = NULL,
  };

  static struct option long_opts[] = {
      {"no-banner", no_argument, NULL, 'B'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'B':
      opts.show_banner = 0;
      break;
    case 'h':
      print_help();
      return 0;
    default:
      print_usage_error();
      return 2;
    }
  }

  /* Commands are read from stdin, never from argv, so a stray operand is a
   * mistake worth naming rather than something to ignore. */
  if (optind < argc) {
    fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
    print_usage_error();
    return 2;
  }

  /* system(NULL) reports whether an interpreter exists at all. Asking once is
   * worth it: without one, every command would fail the same way, one line of
   * errno noise at a time. */
  if (system(NULL) == 0) {
    fprintf(stderr, "%s: %s\n", SHELL_PROGNAME,
            shell_result_str(SHELL_ERR_NO_SHELL));
    return 1;
  }

  /* Read stdin unbuffered, so a command inherits the input mini_shell has not
   * consumed yet. Buffered, stdio pulls the whole pipe in before the first fork
   * and `printf 'cat\necho done\n' | mini_shell` hands `cat` an empty stdin.
   * POSIX requires exactly this of a shell ("It shall not read ahead in such a
   * manner that any characters intended to be read by the invoked command are
   * consumed by the shell"), and bash honors it; dash does not, so do not take
   * /bin/sh as the reference here. Unconditional, not gated on isatty: a
   * terminal read already returns a line at a time, so there is nothing to
   * gate. It belongs here rather than in shell_run, which takes a stream its
   * caller owns. */
  if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
    fprintf(stderr, "%s: cannot unbuffer stdin: %s\n", SHELL_PROGNAME,
            strerror(errno));
    return 1;
  }

  ShellResult result = shell_run(stdin, stdout, stderr, &opts);
  if (result != SHELL_OK) {
    if (result == SHELL_ERR_NOMEM)
      fprintf(stderr, "%s: %s\n", SHELL_PROGNAME, shell_result_str(result));
    else
      fprintf(stderr, "%s: %s: %s\n", SHELL_PROGNAME, shell_result_str(result),
              strerror(errno));
    return 1;
  }
  return 0;
}
