#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "copyfile.h"

int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "usage: copy_file <source> <destination>\n");
    return 2;
  }

  const char *src_path = argv[1];
  const char *dst_path = argv[2];

  CopyResult result = copy_path(src_path, dst_path);
  if (result == COPY_OK) {
    printf("copied '%s' to '%s'\n", src_path, dst_path);
    return 0;
  }

  /* Errors naming the destination point at dst_path; everything else is about
   * the source. errno is still the value the failing libc call left. */
  const char *file =
      (result == COPY_ERR_OPEN_DST || result == COPY_ERR_WRITE) ? dst_path
                                                                : src_path;
  fprintf(stderr, "copy_file: %s: %s: %s\n", copy_result_str(result), file,
          strerror(errno));
  return 1;
}
