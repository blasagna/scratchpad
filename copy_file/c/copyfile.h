#ifndef COPY_FILE_COPYFILE_H
#define COPY_FILE_COPYFILE_H

#include <stdio.h>

/*
 * Outcome of a copy operation. A nonzero value names the stage that failed so
 * callers can report which file and action went wrong. On any failure the
 * relevant libc call's errno is left in place, so the caller may pair the
 * result with strerror(errno)/perror() for a precise message.
 */
typedef enum {
  COPY_OK = 0,
  COPY_ERR_OPEN_SRC, /* opening the source for reading failed */
  COPY_ERR_OPEN_DST, /* opening/creating the destination for writing failed */
  COPY_ERR_READ,     /* a read error occurred on the source */
  COPY_ERR_WRITE,    /* a write or flush error occurred on the destination */
} CopyResult;

/*
 * copy_stream - copies every byte from src to dst.
 *
 * Input:  src - open, readable FILE* positioned at the bytes to copy.
 *         dst - open, writable FILE*.
 *         The caller retains ownership of both streams and must close them.
 *
 * Output: Returns COPY_OK on success. Returns COPY_ERR_READ on a source read
 *         error or COPY_ERR_WRITE on a destination write error, with errno left
 *         as the failing call set it. An empty source succeeds.
 */
CopyResult copy_stream(FILE *src, FILE *dst);

/*
 * copy_path - copies the contents of src_path to dst_path.
 *
 * Opens src_path for reading, opens (creating or truncating) dst_path for
 * writing, copies all bytes, and closes both files. A failing close on the
 * destination is reported as COPY_ERR_WRITE, since it may signal buffered data
 * that never reached disk.
 *
 * Input:  src_path - path to read from.
 *         dst_path - path to create or overwrite.
 *
 * Output: Returns COPY_OK on success, or the CopyResult for the failing stage
 *         with errno preserved from the underlying call.
 */
CopyResult copy_path(const char *src_path, const char *dst_path);

/* Returns a short human-readable label for a CopyResult. */
const char *copy_result_str(CopyResult r);

#endif
