#include "copyfile.h"

#include <errno.h>

/* Bytes moved per fread()/fwrite() pair. Copying in blocks rather than one
 * byte at a time keeps the loop off the stdio locking path. */
#define COPY_BUF_SIZE (64 * 1024)

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

CopyResult copy_path(const char *src_path, const char *dst_path) {
  FILE *src = fopen(src_path, "rb");
  if (!src)
    return COPY_ERR_OPEN_SRC;

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
  }
  return "unknown error";
}
