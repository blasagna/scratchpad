#include "logger.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* The fixed entry layout: fields are separated by a space and every entry ends
 * with a newline. Not options — see LogFormat in logger.h. */
static const char *const kDelimiter = " ";
static const char *const kSeparator = "\n";

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

/* Writes "[text]" followed by the delimiter. */
static int put_field(FILE *out, const char *text) {
  return put_str(out, "[") && put_str(out, text) && put_str(out, "]") &&
         put_str(out, kDelimiter);
}

const char *log_level_str(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_WARNING:
    return "WARNING";
  case LOG_LEVEL_ERROR:
    return "ERROR";
  }
  return "UNKNOWN";
}

LogResult log_level_parse(const char *name, LogLevel *out) {
  static const struct {
    const char *name;
    LogLevel level;
  } kLevels[] = {
      {"debug", LOG_LEVEL_DEBUG},
      {"info", LOG_LEVEL_INFO},
      {"warning", LOG_LEVEL_WARNING},
      {"error", LOG_LEVEL_ERROR},
  };

  for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
    if (strcmp(name, kLevels[i].name) == 0) {
      *out = kLevels[i].level;
      return LOG_OK;
    }
  }
  return LOG_ERR_BAD_LEVEL;
}

const char *log_result_str(LogResult r) {
  switch (r) {
  case LOG_OK:
    return "success";
  case LOG_ERR_OPEN:
    return "cannot open log file";
  case LOG_ERR_WRITE:
    return "error writing log file";
  case LOG_ERR_CLOSE:
    return "error closing log file";
  case LOG_ERR_READ:
    return "error reading input";
  case LOG_ERR_BAD_LEVEL:
    return "unknown log level";
  case LOG_ERR_BAD_TIME:
    return "cannot determine the time";
  case LOG_ERR_NOMEM:
    return "out of memory";
  }
  return "unknown error";
}

LogResult log_format_timestamp(time_t when, char *buf, size_t buf_size) {
  struct tm parts;
  if (!gmtime_r(&when, &parts))
    return LOG_ERR_BAD_TIME;

  /* A year outside four digits has no agreed rendering across the ports, so it
   * is refused rather than truncated or widened. */
  int year = parts.tm_year + 1900;
  if (year < 0 || year > 9999)
    return LOG_ERR_BAD_TIME;

  /* snprintf rather than strftime: "%Y" does not zero-pad a year below 1000 in
   * glibc, and the Rust port pads unconditionally. Spelling the padding out
   * here is what keeps the three ports agreeing on such dates. */
  int n = snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", year,
                   parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                   parts.tm_sec);
  if (n < 0 || (size_t)n >= buf_size)
    return LOG_ERR_BAD_TIME;
  return LOG_OK;
}

LogResult log_clock_resolve(const char *fake, time_t real_now, time_t *out) {
  if (!fake) {
    *out = real_now;
    return LOG_OK;
  }

  /* Accept exactly -?[0-9]+. strtoll alone would also take leading whitespace
   * and a '+' sign, and Rust's parse would take '+' but not whitespace, so the
   * shape is checked by hand to keep the three ports on the same set. */
  const char *digits = (fake[0] == '-') ? fake + 1 : fake;
  if (digits[0] == '\0')
    return LOG_ERR_BAD_TIME;
  for (const char *d = digits; *d != '\0'; d++) {
    if (*d < '0' || *d > '9')
      return LOG_ERR_BAD_TIME;
  }

  errno = 0;
  char *endp;
  long long seconds = strtoll(fake, &endp, 10);
  if (endp == fake || *endp != '\0' || errno == ERANGE)
    return LOG_ERR_BAD_TIME;

  time_t when = (time_t)seconds;
  if ((long long)when != seconds)
    return LOG_ERR_BAD_TIME;

  *out = when;
  return LOG_OK;
}

LogResult log_clock_now(time_t *out) {
  const char *fake = getenv(LOG_FAKE_TIME_VAR);
  if (fake)
    return log_clock_resolve(fake, 0, out);

  time_t now = time(NULL);
  if (now == (time_t)-1)
    return LOG_ERR_BAD_TIME;
  return log_clock_resolve(NULL, now, out);
}

LogResult log_write_entry(FILE *out, const LogFormat *fmt,
                          const char *timestamp, const char *message,
                          size_t message_len) {
  if (fmt->show_timestamp && !put_field(out, timestamp))
    return LOG_ERR_WRITE;
  if (fmt->show_level && !put_field(out, log_level_str(fmt->level)))
    return LOG_ERR_WRITE;
  if (!put_bytes(out, message, message_len))
    return LOG_ERR_WRITE;
  if (!put_str(out, kSeparator))
    return LOG_ERR_WRITE;
  return LOG_OK;
}

LogResult log_write_messages(FILE *out, const LogFormat *fmt,
                             const char *timestamp, const char *const *messages,
                             size_t count) {
  for (size_t i = 0; i < count; i++) {
    LogResult result =
        log_write_entry(out, fmt, timestamp, messages[i], strlen(messages[i]));
    if (result != LOG_OK)
      return result;
  }
  return LOG_OK;
}

LogResult log_write_lines(FILE *out, const LogFormat *fmt,
                          const char *timestamp, FILE *in) {
  char *line = NULL;
  size_t cap = 0;
  LogResult result = LOG_OK;

  /* getline is POSIX rather than ISO C, but it reports a byte count, so a line
   * containing a NUL survives intact. */
  ssize_t n;
  while ((n = getline(&line, &cap, in)) > 0) {
    size_t len = (size_t)n;
    /* Strip one '\n', then one '\r', so CRLF input logs the same bytes as LF
     * input. A '\r' anywhere else belongs to the message. */
    if (line[len - 1] == '\n')
      len--;
    if (len > 0 && line[len - 1] == '\r')
      len--;

    result = log_write_entry(out, fmt, timestamp, line, len);
    if (result != LOG_OK)
      break;
  }

  /* getline returns -1 for EOF, a read error, and an allocation failure alike;
   * only the stream flags tell them apart. */
  if (result == LOG_OK && ferror(in))
    result = LOG_ERR_READ;
  else if (result == LOG_OK && !feof(in))
    result = LOG_ERR_NOMEM;

  int saved = errno;
  free(line);
  errno = saved;
  return result;
}

LogResult log_open_append(const char *path, FILE **out) {
  *out = NULL;
  FILE *f = fopen(path, "ab");
  if (!f)
    return LOG_ERR_OPEN;
  *out = f;
  return LOG_OK;
}

LogResult log_close(FILE *out) {
  if (fflush(out) != 0) {
    int saved = errno;
    fclose(out);
    errno = saved;
    return LOG_ERR_WRITE;
  }
  if (fclose(out) != 0)
    return LOG_ERR_CLOSE;
  return LOG_OK;
}
