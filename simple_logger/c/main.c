#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

static const char *DEFAULT_DELIMITER = " ";
static const char *DEFAULT_SEPARATOR = "\n";
static const LogLevel DEFAULT_LEVEL = LOG_LEVEL_INFO;

/* The default separator as the user would type it, for the help text. */
static const char *DEFAULT_SEPARATOR_ESCAPED = "\\n";

static void print_help(void) {
  printf("usage: simple_logger [options] <logfile> [message...]\n");
  printf("       simple_logger -h | --help\n");
  printf("\n");
  printf("Appends timestamped messages to a log file. Each message argument "
         "becomes\n");
  printf("one entry; with no message arguments, one entry is read per line "
         "from\n");
  printf("stdin. The log file is opened for append, so previous entries are "
         "kept.\n");
  printf("\n");
  printf("Each entry is written as:\n");
  printf("  [<timestamp>]<delim>[<LEVEL>]<delim><message><separator>\n");
  printf("\n");
  printf("The timestamp is UTC ISO 8601 (e.g. [2026-07-30T18:22:05Z]) and is "
         "read\n");
  printf("once per run, so every entry one run writes shares it.\n");
  printf("\n");
  printf("Options:\n");
  printf("  -l, --level LEVEL    debug, info, warning, or error (default: "
         "info)\n");
  printf("  -d, --delimiter STR  text between fields (default: \"%s\")\n",
         DEFAULT_DELIMITER);
  printf("  -s, --separator STR  text after each entry (default: \"%s\")\n",
         DEFAULT_SEPARATOR_ESCAPED);
  printf("      --no-timestamp   omit the [timestamp] field\n");
  printf("      --no-level       omit the [LEVEL] field\n");
  printf("  -h, --help           show this help\n");
  printf("\n");
  printf("STR values accept the escapes \\n, \\t, \\r, and \\\\; any other "
         "backslash\n");
  printf("escape is an error. Use -- before a message that begins with '-'.\n");
  printf("\n");
  printf("Environment:\n");
  printf("  %s  epoch seconds to use instead of the real\n", LOG_FAKE_TIME_VAR);
  printf("                           clock; used by the cross-port parity "
         "script.\n");
}

static void print_usage_error(void) {
  fprintf(stderr, "usage: simple_logger [options] <logfile> [message...]\n");
  fprintf(stderr, "       simple_logger --help\n");
}

/*
 * Unescapes an option value into *out, which the caller frees. On failure
 * reports the problem against opt_name and returns the exit code to use.
 */
static int parse_escaped(const char *opt_name, const char *value, char **out) {
  LogResult result = log_unescape(value, out);
  if (result == LOG_OK)
    return 0;
  if (result == LOG_ERR_BAD_ESCAPE) {
    fprintf(stderr,
            "error: invalid value '%s' for %s (only \\n, \\t, \\r, and \\\\ "
            "are recognized)\n",
            value, opt_name);
    return 2;
  }
  fprintf(stderr, "simple_logger: %s\n", log_result_str(result));
  return 1;
}

int main(int argc, char *argv[]) {
  LogFormat fmt = {
      .delimiter = NULL,
      .separator = NULL,
      .level = DEFAULT_LEVEL,
      .show_timestamp = 1,
      .show_level = 1,
  };

  /* Owned copies of the unescaped option values fmt borrows. */
  char *delimiter = NULL;
  char *separator = NULL;
  int status = 0;

  static struct option long_opts[] = {
      {"level", required_argument, NULL, 'l'},
      {"delimiter", required_argument, NULL, 'd'},
      {"separator", required_argument, NULL, 's'},
      {"no-timestamp", no_argument, NULL, 'T'},
      {"no-level", no_argument, NULL, 'L'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "l:d:s:h", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'l':
      if (log_level_parse(optarg, &fmt.level) != LOG_OK) {
        fprintf(stderr,
                "error: invalid value '%s' for --level (expected debug, info, "
                "warning, or error)\n",
                optarg);
        status = 2;
      }
      break;
    case 'd':
      free(delimiter);
      delimiter = NULL;
      status = parse_escaped("--delimiter", optarg, &delimiter);
      break;
    case 's':
      free(separator);
      separator = NULL;
      status = parse_escaped("--separator", optarg, &separator);
      break;
    case 'T':
      fmt.show_timestamp = 0;
      break;
    case 'L':
      fmt.show_level = 0;
      break;
    case 'h':
      print_help();
      goto cleanup;
    default:
      print_usage_error();
      status = 2;
      break;
    }
    if (status != 0)
      goto cleanup;
  }

  if (optind >= argc) {
    fprintf(stderr, "error: missing <logfile>\n");
    print_usage_error();
    status = 2;
    goto cleanup;
  }

  const char *path = argv[optind];
  if (path[0] == '\0') {
    fprintf(stderr, "error: <logfile> must not be empty\n");
    status = 2;
    goto cleanup;
  }

  fmt.delimiter = delimiter ? delimiter : DEFAULT_DELIMITER;
  fmt.separator = separator ? separator : DEFAULT_SEPARATOR;

  /* One clock reading for the whole run, so a slow stdin pipe cannot spread
   * one invocation's entries across several seconds. */
  time_t when;
  LogResult result = log_clock_now(&when);
  if (result != LOG_OK) {
    /* A bad override is the user's mistake; a failing system clock is not. */
    const char *fake = getenv(LOG_FAKE_TIME_VAR);
    if (fake) {
      fprintf(stderr, "error: invalid %s value '%s' (expected epoch seconds)\n",
              LOG_FAKE_TIME_VAR, fake);
      status = 2;
    } else {
      fprintf(stderr, "simple_logger: %s\n", log_result_str(result));
      status = 1;
    }
    goto cleanup;
  }

  char timestamp[LOG_TIMESTAMP_BUF];
  result = log_format_timestamp(when, timestamp, sizeof(timestamp));
  if (result != LOG_OK) {
    fprintf(stderr, "simple_logger: %s\n", log_result_str(result));
    status = 1;
    goto cleanup;
  }

  FILE *out = NULL;
  result = log_open_append(path, &out);
  if (result == LOG_OK) {
    const char *const *messages = (const char *const *)&argv[optind + 1];
    size_t count = (size_t)(argc - optind - 1);

    result = count > 0
                 ? log_write_messages(out, &fmt, timestamp, messages, count)
                 : log_write_lines(out, &fmt, timestamp, stdin);

    /* Report a write failure over a close failure: it names the earlier and
     * more specific stage, and all three ports agree on that precedence. */
    LogResult close_result = log_close(out);
    if (result == LOG_OK)
      result = close_result;
  }

  if (result != LOG_OK) {
    /* A read failure is about stdin, everything else about the log file. */
    if (result == LOG_ERR_READ)
      fprintf(stderr, "simple_logger: %s: <stdin>: %s\n",
              log_result_str(result), strerror(errno));
    else if (result == LOG_ERR_NOMEM)
      fprintf(stderr, "simple_logger: %s\n", log_result_str(result));
    else
      fprintf(stderr, "simple_logger: %s: %s: %s\n", log_result_str(result),
              path, strerror(errno));
    status = 1;
  }

cleanup:
  free(delimiter);
  free(separator);
  return status;
}
