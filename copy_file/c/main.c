#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "copyfile.h"

int main(int argc, const char *const argv[]) {
  if (argc != 3) {
    fprintf(stderr, "usage: copy_file <source> <destination>\n");
    return 2;
  }

  const char *src_path = argv[1];
  const char *dst_path = argv[2];

  CopyResult result = copy_path(src_path, dst_path);
  if (result == COPY_OK) {
    /* Report where the file actually landed: the same tilde/directory
     * resolution copy_path used, so a directory destination shows the full
     * path. Fall back to the raw argument if resolution can't be reproduced. */
    char *src = copy_expand_tilde(src_path);
    char *dst_expanded = copy_expand_tilde(dst_path);
    char *final =
        (src && dst_expanded) ? copy_resolve_dest(dst_expanded, src) : NULL;
    printf("copied '%s' to '%s'\n", src_path, final ? final : dst_path);
    free(src);
    free(dst_expanded);
    free(final);
    return 0;
  }

  /* These failures have no meaningful errno and aren't tied to one file. */
  if (result == COPY_ERR_NOMEM || result == COPY_ERR_SAME_FILE) {
    fprintf(stderr, "copy_file: %s\n", copy_result_str(result));
    return 1;
  }

  /* Errors naming the destination point at dst_path; everything else is about
   * the source. errno is still the value the failing libc call left. */
  const char *file = (result == COPY_ERR_OPEN_DST || result == COPY_ERR_WRITE)
                         ? dst_path
                         : src_path;
  fprintf(stderr, "copy_file: %s: %s: %s\n", copy_result_str(result), file,
          strerror(errno));
  return 1;
}
