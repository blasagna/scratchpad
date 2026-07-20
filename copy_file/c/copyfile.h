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
  COPY_ERR_OPEN_SRC,  /* opening the source for reading failed */
  COPY_ERR_OPEN_DST,  /* opening/creating the destination for writing failed */
  COPY_ERR_READ,      /* a read error occurred on the source */
  COPY_ERR_WRITE,     /* a write or flush error occurred on the destination */
  COPY_ERR_NOMEM,     /* out of memory while resolving a path */
  COPY_ERR_SAME_FILE, /* source and destination are the same file */
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
 * Both paths are first passed through copy_expand_tilde. The destination is
 * then passed through copy_resolve_dest, so naming an existing directory copies
 * the source into it. The source is opened for reading, the resolved
 * destination is opened (created or truncated) for writing, all bytes are
 * copied, and both files are closed. A failing close on the destination is
 * reported as COPY_ERR_WRITE, since it may signal buffered data that never
 * reached disk.
 *
 * Input:  src_path - path to read from.
 *         dst_path - path to create or overwrite (or an existing directory).
 *
 * Output: Returns COPY_OK on success, or the CopyResult for the failing stage
 *         with errno preserved from the underlying call (ENOMEM for
 *         COPY_ERR_NOMEM).
 */
CopyResult copy_path(const char *src_path, const char *dst_path);

/*
 * copy_expand_tilde - expands a leading ~ or ~user in a path.
 *
 * A path of "~" or "~/..." expands using $HOME (falling back to the current
 * user's home directory from the password database); "~user" or "~user/..."
 * expands using that user's home directory. A tilde anywhere but the start is
 * left alone, and if the home directory cannot be resolved the path is returned
 * unchanged so the eventual open fails with a clear error.
 *
 * Output: A newly malloc'd, NUL-terminated path the caller must free(), or NULL
 *         on allocation failure. The result is a copy even when no expansion
 *         applies.
 */
char *copy_expand_tilde(const char *path);

/*
 * copy_resolve_dest - resolves the final destination path for a copy.
 *
 * If dst_path names an existing directory, returns "dst_path/<basename of
 * src_path>" (without doubling an existing trailing slash). Otherwise returns a
 * plain copy of dst_path.
 *
 * Output: A newly malloc'd, NUL-terminated path the caller must free(), or NULL
 *         on allocation failure.
 */
char *copy_resolve_dest(const char *dst_path, const char *src_path);

/* Returns a short human-readable label for a CopyResult. */
const char *copy_result_str(CopyResult r);

#endif
