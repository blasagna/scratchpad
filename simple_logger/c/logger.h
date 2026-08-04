#ifndef SIMPLE_LOGGER_LOGGER_H
#define SIMPLE_LOGGER_LOGGER_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* Bytes needed for "YYYY-MM-DDTHH:MM:SSZ" plus the NUL. Years past 9999 need
 * more, and log_format_timestamp reports LOG_ERR_BAD_TIME rather than truncate.
 * A #define because it sizes arrays. */
#define LOG_TIMESTAMP_BUF 21

/* Name of the environment variable holding fake epoch seconds for tests. */
#define LOG_FAKE_TIME_VAR "SIMPLE_LOGGER_FAKE_TIME"

/*
 * Outcome of a logging operation. A nonzero value names the stage that failed.
 * For the stages backed by a libc call (LOG_ERR_OPEN, LOG_ERR_WRITE,
 * LOG_ERR_CLOSE, LOG_ERR_READ) the failing call's errno is left in place, so
 * the caller may pair the result with strerror(errno). The remaining stages
 * describe bad input and carry no errno.
 */
typedef enum {
  LOG_OK = 0,
  LOG_ERR_OPEN,      /* opening the log file for append failed */
  LOG_ERR_WRITE,     /* a write or flush error occurred on the log file */
  LOG_ERR_CLOSE,     /* closing the log file failed; data may not be on disk */
  LOG_ERR_READ,      /* a read error occurred on the message input stream */
  LOG_ERR_BAD_LEVEL, /* the level name was not debug, info, warning, or error */
  LOG_ERR_BAD_TIME,  /* the epoch seconds could not be rendered */
  LOG_ERR_NOMEM,     /* out of memory */
} LogResult;

/* Severity tag written with each entry, ordered least to most severe. */
typedef enum {
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARNING,
  LOG_LEVEL_ERROR,
} LogLevel;

/*
 * How an entry is laid out. An entry is written as
 *
 *   [<timestamp>] [<LEVEL>] <message>\n
 *
 * with the timestamp field omitted when show_timestamp is 0 and the level field
 * omitted when show_level is 0; omitting a field drops its trailing space too.
 * The newline follows every entry, including the last, so the next run appends
 * onto a fresh line. The space and the newline are fixed: making them options
 * bought nothing that a pipe through sed could not do, and cost every port an
 * unescaper to spell them on a command line.
 */
typedef struct {
  LogLevel level;
  /* The flags are int, not bool, because this header is compiled in two
   * dialects. The C here is C17 (.bazelrc sets -std=c++20 as a cxxopt, which
   * does not reach C), where bool needs <stdbool.h> and is _Bool; test_logger.c
   * compiles the same header as C++ (-x c++), where bool is a distinct builtin.
   * The extern "C" wrapper there fixes linkage, not layout, so the two would
   * agree only by ABI accident. int is the same type in both. */
  int show_timestamp;
  int show_level;
} LogFormat;

/* Returns the uppercase label written for a level ("DEBUG", "INFO", ...). */
const char *log_level_str(LogLevel level);

/*
 * log_level_parse - maps a level name to a LogLevel.
 *
 * Input:  name - one of "debug", "info", "warning", "error", matched exactly.
 *         Uppercase spellings are rejected so the accepted set is the same in
 *         every port.
 *
 * Output: Returns LOG_OK and stores the level in *out, or LOG_ERR_BAD_LEVEL
 *         leaving *out untouched.
 */
LogResult log_level_parse(const char *name, LogLevel *out);

/* Returns a short human-readable label for a LogResult. */
const char *log_result_str(LogResult r);

/*
 * log_format_timestamp - renders epoch seconds as a UTC ISO 8601 timestamp.
 *
 * The rendering is "YYYY-MM-DDTHH:MM:SSZ" — always UTC, never local time, so
 * all three ports agree without a timezone database.
 *
 * Input:  when - seconds since the Unix epoch; negative values are valid.
 *         buf, buf_size - destination, normally a char[LOG_TIMESTAMP_BUF].
 *
 * Output: Returns LOG_OK with a NUL-terminated timestamp in buf, or
 *         LOG_ERR_BAD_TIME if the time cannot be converted or does not fit,
 *         leaving buf's contents unspecified.
 */
LogResult log_format_timestamp(time_t when, char *buf, size_t buf_size);

/*
 * log_clock_resolve - picks the timestamp for a run.
 *
 * The clock is read once per run, so every entry a single invocation writes
 * shares one timestamp.
 *
 * Input:  fake - the raw LOG_FAKE_TIME_VAR value, or NULL when unset.
 *         real_now - the system clock reading to use when fake is NULL.
 *
 * Output: Returns LOG_OK and stores the chosen time in *out. A fake value that
 *         is empty, unparseable, or has trailing junk returns
 *         LOG_ERR_BAD_TIME — never a silent fallback to real_now, which would
 *         let a parity check pass while comparing three real clocks.
 */
LogResult log_clock_resolve(const char *fake, time_t real_now, time_t *out);

/*
 * log_clock_now - reads LOG_FAKE_TIME_VAR and the system clock.
 *
 * The only impure function here; everything else is a pure transformation of
 * its arguments. Call it from main, not from library code.
 *
 * Output: Returns LOG_OK and stores the time in *out, or LOG_ERR_BAD_TIME if
 *         the environment variable is set to an unusable value or the system
 *         clock is unavailable.
 */
LogResult log_clock_now(time_t *out);

/*
 * log_write_entry - writes one formatted entry to out.
 *
 * The message is copied verbatim: embedded newlines make the entry span
 * physical lines, and embedded NULs and non-ASCII bytes are preserved, which is
 * why the length is passed explicitly rather than inferred with strlen.
 *
 * Input:  out - open, writable FILE* the caller retains ownership of.
 *         fmt, timestamp - the layout and the run's timestamp; timestamp is
 *         unused when fmt->show_timestamp is 0.
 *         message, message_len - the message bytes and their count.
 *
 * Output: Returns LOG_OK, or LOG_ERR_WRITE with errno as the failing call left
 *         it.
 */
LogResult log_write_entry(FILE *out, const LogFormat *fmt,
                          const char *timestamp, const char *message,
                          size_t message_len);

/*
 * log_write_messages - writes one entry per message, in order.
 *
 * Input:  messages, count - NUL-terminated message strings; a count of 0
 *         writes nothing and succeeds.
 *
 * Output: Returns LOG_OK, or LOG_ERR_WRITE from the first failing entry.
 */
LogResult log_write_messages(FILE *out, const LogFormat *fmt,
                             const char *timestamp, const char *const *messages,
                             size_t count);

/*
 * log_write_lines - writes one entry per line read from in.
 *
 * One trailing '\n' is stripped from each line, then one trailing '\r', so
 * CRLF input logs the same bytes as LF input; a '\r' anywhere else is kept. A
 * blank line becomes an entry with an empty message, and a final line without a
 * trailing newline is still logged. Empty input writes nothing.
 *
 * Output: Returns LOG_OK, LOG_ERR_WRITE, LOG_ERR_READ if in reported an error,
 *         or LOG_ERR_NOMEM.
 */
LogResult log_write_lines(FILE *out, const LogFormat *fmt,
                          const char *timestamp, FILE *in);

/*
 * log_open_append - opens path for appending, creating it if needed.
 *
 * Existing contents are always kept; the file is never truncated. It is created
 * even when no entry ends up being written.
 *
 * Output: Returns LOG_OK and stores the stream in *out, or LOG_ERR_OPEN with
 *         errno set by fopen and *out left NULL.
 */
LogResult log_open_append(const char *path, FILE **out);

/*
 * log_close - flushes and closes a stream from log_open_append.
 *
 * A flush failure is reported as LOG_ERR_WRITE, since it is often the first
 * sign that buffered data never reached disk; the stream is closed either way.
 *
 * Output: Returns LOG_OK, LOG_ERR_WRITE, or LOG_ERR_CLOSE, with errno as the
 *         failing call left it.
 */
LogResult log_close(FILE *out);

#endif
