#include "copyfile.h"

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bytes moved per fread()/fwrite() pair. Copying in blocks rather than one
 * byte at a time keeps the loop off the stdio locking path. */
#define COPY_BUF_SIZE (64 * 1024)

/* Returns a heap copy of s, or NULL on allocation failure. Avoids depending on
 * strdup's feature-test macros. */
static char *dup_cstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *out = malloc(n);
  if (out)
    memcpy(out, s, n);
  return out;
}

/* Returns a pointer into path to the component after the last '/', or path
 * itself when there is no '/'. */
static const char *path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

CopyResult copy_stream(FILE *src, FILE *dst) {
  char buf[COPY_BUF_SIZE];
  size_t n;

  while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
    if (fwrite(buf, 1, n, dst) != n)
      return COPY_ERR_WRITE;
  }

  /* fread returning 0 means EOF or error; only ferror distinguishes them. */
  return ferror(src) ? COPY_ERR_READ : COPY_OK;
}

char *copy_expand_tilde(const char *path) {
  if (path[0] != '~')
    return dup_cstr(path);

  /* The tilde prefix runs from '~' up to the first '/' (or the end). */
  const char *slash = strchr(path, '/');
  size_t prefix_len = slash ? (size_t)(slash - path) : strlen(path);
  const char *rest = path + prefix_len; /* "" or "/..." */

  const char *home = NULL;
  if (prefix_len == 1) {
    /* Bare "~": prefer $HOME, fall back to the password database. */
    home = getenv("HOME");
    if (!home || home[0] == '\0') {
      struct passwd *pw = getpwuid(getuid());
      home = pw ? pw->pw_dir : NULL;
    }
  } else {
    /* "~user": look the name up in the password database. */
    char *user = malloc(prefix_len); /* prefix_len - 1 chars + NUL */
    if (!user)
      return NULL;
    memcpy(user, path + 1, prefix_len - 1);
    user[prefix_len - 1] = '\0';
    struct passwd *pw = getpwnam(user);
    free(user);
    home = pw ? pw->pw_dir : NULL;
  }

  /* Unresolvable home: leave the path as written so the open reports it. */
  if (!home)
    return dup_cstr(path);

  size_t home_len = strlen(home);
  size_t rest_len = strlen(rest);
  char *out = malloc(home_len + rest_len + 1);
  if (!out)
    return NULL;
  memcpy(out, home, home_len);
  memcpy(out + home_len, rest, rest_len + 1); /* copies the NUL too */
  return out;
}

char *copy_resolve_dest(const char *dst_path, const char *src_path) {
  struct stat st;
  if (stat(dst_path, &st) != 0 || !S_ISDIR(st.st_mode))
    return dup_cstr(dst_path);

  const char *base = path_basename(src_path);
  size_t dlen = strlen(dst_path);
  int need_sep = dlen > 0 && dst_path[dlen - 1] != '/';
  size_t blen = strlen(base);

  char *out = malloc(dlen + (size_t)need_sep + blen + 1);
  if (!out)
    return NULL;
  memcpy(out, dst_path, dlen);
  if (need_sep)
    out[dlen] = '/';
  memcpy(out + dlen + need_sep, base, blen + 1); /* copies the NUL too */
  return out;
}

/* Opens src and dst by path, copies, and closes both. errno is preserved so the
 * caller can report the failing stage. */
static CopyResult copy_resolved(const char *src_path, const char *dst_path) {
  FILE *src = fopen(src_path, "rb");
  if (!src)
    return COPY_ERR_OPEN_SRC;

  /* Refuse to copy a file onto itself: opening the destination with "wb" would
   * truncate it first, destroying the source before a single byte is read.
   * Compare device+inode so hard links and "./x" vs "x" are caught too. A dst
   * that does not exist yet simply fails the stat and is not the same file. */
  struct stat src_st, dst_st;
  if (fstat(fileno(src), &src_st) == 0 && stat(dst_path, &dst_st) == 0 &&
      src_st.st_dev == dst_st.st_dev && src_st.st_ino == dst_st.st_ino) {
    fclose(src);
    return COPY_ERR_SAME_FILE;
  }

  FILE *dst = fopen(dst_path, "wb");
  if (!dst) {
    int saved = errno;
    fclose(src);
    errno = saved;
    return COPY_ERR_OPEN_DST;
  }

  CopyResult result = copy_stream(src, dst);
  int saved = errno;

  fclose(src);

  /* Flush buffered writes via fclose; a failure here can be the first sign
   * that data never reached disk, so surface it as a write error. */
  if (fclose(dst) != 0 && result == COPY_OK) {
    result = COPY_ERR_WRITE;
    saved = errno;
  }

  errno = saved;
  return result;
}

CopyResult copy_path(const char *src_path, const char *dst_path) {
  char *src = copy_expand_tilde(src_path);
  char *dst_expanded = copy_expand_tilde(dst_path);
  char *dst = NULL;
  if (src && dst_expanded)
    dst = copy_resolve_dest(dst_expanded, src);

  CopyResult result;
  if (!src || !dst_expanded || !dst) {
    result = COPY_ERR_NOMEM;
    errno = ENOMEM;
  } else {
    result = copy_resolved(src, dst);
  }

  /* free() may touch errno on some platforms, so preserve it across cleanup. */
  int saved = errno;
  free(src);
  free(dst_expanded);
  free(dst);
  errno = saved;
  return result;
}

const char *copy_result_str(CopyResult r) {
  switch (r) {
  case COPY_OK:
    return "success";
  case COPY_ERR_OPEN_SRC:
    return "cannot open source file";
  case COPY_ERR_OPEN_DST:
    return "cannot open destination file";
  case COPY_ERR_READ:
    return "error reading source file";
  case COPY_ERR_WRITE:
    return "error writing destination file";
  case COPY_ERR_NOMEM:
    return "out of memory";
  case COPY_ERR_SAME_FILE:
    return "source and destination are the same file";
  }
  return "unknown error";
}
