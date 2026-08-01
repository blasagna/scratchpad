#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matrix.h"
#include "matrix_io.h"

/* Argument that means "read stdin", and the label used for it in errors. */
static const char *STDIN_ARG = "-";
static const char *STDIN_LABEL = "<stdin>";

/* The most operands any operation takes. */
#define MAX_OPERANDS 2

typedef enum {
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_SCALE,
} Operation;

/* The operations, their spellings, and how many matrices each consumes. */
static const struct {
  const char *name;
  Operation op;
  size_t operands;
  int needs_scalar;
} kOperations[] = {
    {"add", OP_ADD, 2, 0},
    {"sub", OP_SUB, 2, 0},
    {"mul", OP_MUL, 2, 0},
    {"scale", OP_SCALE, 1, 1},
};

static void print_help(void) {
  printf("usage: matrix_ops <add|sub|mul|scale> [operand...] [options]\n");
  printf("       matrix_ops -h | --help\n");
  printf("\n");
  printf("Performs an operation on 2D matrices of real numbers and prints the "
         "result.\n");
  printf("\n");
  printf("Operations:\n");
  printf("  add      element-wise sum of two matrices of the same shape\n");
  printf("  sub      element-wise difference of two matrices of the same "
         "shape\n");
  printf("  mul      matrix product; the first operand's column count must "
         "equal\n");
  printf("           the second operand's row count\n");
  printf("  scale    multiplies one matrix by --scalar\n");
  printf("\n");
  printf("Operands:\n");
  printf("  Each --values or --file introduces one operand, and any --rows or "
         "--cols\n");
  printf("  written before it describes that operand, so the two operands of "
         "a\n");
  printf("  product may have different shapes.\n");
  printf("\n");
  printf(
      "  -v, --values \"...\"  values separated by whitespace or newlines\n");
  printf("  -f, --file PATH     read the values from a file ('%s' for "
         "stdin)\n",
         STDIN_ARG);
  printf("  -r, --rows N        rows for the next operand (optional)\n");
  printf("  -c, --cols N        columns for the next operand (optional)\n");
  printf("\n");
  printf("Shape:\n");
  printf("  Dimensions are optional. Without them the layout decides: one "
         "line of\n");
  printf("  values is a row vector, and several lines are rows. Given both "
         "--rows\n");
  printf("  and --cols, the values are reshaped row-major and their count "
         "must be\n");
  printf("  exactly rows x cols; given only one, the other is derived. Rows "
         "of\n");
  printf("  differing length are always an error.\n");
  printf("\n");
  printf("Options:\n");
  printf("  -k, --scalar X      the multiplier for 'scale'\n");
  printf("  -p, --precision N   decimal places in the output, trailing zeros "
         "trimmed\n");
  printf("                      (default: %d)\n", MATRIX_DEFAULT_PRECISION);
  printf("  -h, --help          show this help\n");
  printf("\n");
  printf("Examples:\n");
  printf("  matrix_ops add --values \"1 2 3\" --values \"4 5 6\"\n");
  printf("  matrix_ops mul --rows 2 --cols 3 --values \"1 2 3 4 5 6\" \\\n");
  printf("                 --rows 3 --cols 2 --file b.txt\n");
  printf("  matrix_ops scale --scalar 2.5 --file a.txt\n");
}

static void print_usage_error(void) {
  fprintf(stderr,
          "usage: matrix_ops <add|sub|mul|scale> [operand...] [options]\n");
  fprintf(stderr, "       matrix_ops --help\n");
}

/*
 * Parses value as a positive integer (>= 1, fitting in int). On success stores
 * it in *out and returns 0; on failure reports the problem against opt_name and
 * returns -1. Rejects empty input, trailing junk, and non-positive values.
 */
static int parse_positive(const char *opt_name, const char *value, int *out) {
  char *endp;
  long n = strtol(value, &endp, 10);
  if (endp == value || *endp != '\0' || n < 1 || n > INT_MAX) {
    fprintf(stderr,
            "error: invalid value '%s' for %s (expected a positive integer)\n",
            value, opt_name);
    return -1;
  }
  *out = (int)n;
  return 0;
}

/*
 * Parses value as a finite double, applying the same rule the matrix values
 * themselves are held to: no trailing junk, and no nan or infinity.
 */
static int parse_scalar(const char *opt_name, const char *value, double *out) {
  errno = 0;
  char *endp;
  double n = strtod(value, &endp);
  if (endp == value || *endp != '\0' || !isfinite(n)) {
    fprintf(stderr,
            "error: invalid value '%s' for %s (expected a finite number)\n",
            value, opt_name);
    return -1;
  }
  *out = n;
  return 0;
}

/* Maps an operation name to its table entry, or -1 if it is not one. */
static int find_operation(const char *name) {
  for (size_t i = 0; i < sizeof(kOperations) / sizeof(kOperations[0]); i++) {
    if (strcmp(name, kOperations[i].name) == 0)
      return (int)i;
  }
  return -1;
}

/*
 * Loads one operand from a file, where "-" means stdin. Returns MATRIX_OK, or
 * a result the caller reports; a file that cannot be opened is MATRIX_ERR_READ
 * with errno left by fopen.
 */
static MatrixResult load_file(const char *path, size_t rows, size_t cols,
                              Matrix *out) {
  if (strcmp(path, STDIN_ARG) == 0)
    return matrix_read_stream(stdin, rows, cols, out);

  FILE *f = fopen(path, "rb");
  if (!f)
    return MATRIX_ERR_READ;

  MatrixResult rc = matrix_read_stream(f, rows, cols, out);
  int saved = errno;
  fclose(f);
  errno = saved;
  return rc;
}

/* Prints the failure of an operand named by source, and returns the exit code
 * to use: an I/O failure is operational, a bad value or shape is the user's. */
static int report_operand_error(MatrixResult rc, const char *source) {
  if (rc == MATRIX_ERR_READ) {
    fprintf(stderr, "matrix_ops: %s: %s\n", source, strerror(errno));
    return 1;
  }
  if (rc == MATRIX_ERR_NOMEM) {
    fprintf(stderr, "matrix_ops: %s\n", matrix_result_str(rc));
    return 1;
  }
  fprintf(stderr, "error: %s: %s\n", source, matrix_result_str(rc));
  return 2;
}

int main(int argc, char *argv[]) {
  Matrix operands[MAX_OPERANDS] = {{0, 0, NULL}, {0, 0, NULL}};
  size_t operand_count = 0;
  Matrix result = {0, 0, NULL};

  /* Dimensions seen since the last operand closed; they attach to the next
   * --values or --file. */
  size_t pending_rows = MATRIX_DIM_UNSPECIFIED;
  size_t pending_cols = MATRIX_DIM_UNSPECIFIED;

  MatrixFormat fmt = matrix_format_default();
  double scalar = 0.0;
  int have_scalar = 0;
  int status = 0;

  static struct option long_opts[] = {
      {"rows", required_argument, NULL, 'r'},
      {"cols", required_argument, NULL, 'c'},
      {"values", required_argument, NULL, 'v'},
      {"file", required_argument, NULL, 'f'},
      {"scalar", required_argument, NULL, 'k'},
      {"precision", required_argument, NULL, 'p'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  /* Silence getopt's own diagnostics and report the two failures by hand.
   * glibc's messages are prefixed with argv[0], which is the full path to the
   * binary under bazel-bin, so they can never match the C++ port's wording --
   * and a full build path in a user-facing error is worse than useless. The
   * leading ':' in the option string is what keeps a missing value ( ':' )
   * distinguishable from an unknown option ( '?' ) once opterr is off. */
  opterr = 0;

  int opt;
  while ((opt = getopt_long(argc, argv, ":r:c:v:f:k:p:h", long_opts, NULL)) !=
         -1) {
    int n;
    MatrixResult rc;

    switch (opt) {
    case 'r':
      if (parse_positive("--rows", optarg, &n) != 0)
        status = 2;
      else
        pending_rows = (size_t)n;
      break;

    case 'c':
      if (parse_positive("--cols", optarg, &n) != 0)
        status = 2;
      else
        pending_cols = (size_t)n;
      break;

    case 'v':
    case 'f':
      if (operand_count == MAX_OPERANDS) {
        fprintf(stderr, "error: at most %d matrices may be given\n",
                MAX_OPERANDS);
        status = 2;
        break;
      }
      rc = (opt == 'v') ? matrix_parse_text(optarg, pending_rows, pending_cols,
                                            &operands[operand_count])
                        : load_file(optarg, pending_rows, pending_cols,
                                    &operands[operand_count]);
      if (rc != MATRIX_OK) {
        const char *source = (opt == 'v')                       ? "--values"
                             : (strcmp(optarg, STDIN_ARG) == 0) ? STDIN_LABEL
                                                                : optarg;
        status = report_operand_error(rc, source);
        break;
      }
      operand_count++;
      /* The dimensions described this operand only; the next one states its
       * own or infers them. */
      pending_rows = MATRIX_DIM_UNSPECIFIED;
      pending_cols = MATRIX_DIM_UNSPECIFIED;
      break;

    case 'k':
      if (parse_scalar("--scalar", optarg, &scalar) != 0)
        status = 2;
      else
        have_scalar = 1;
      break;

    case 'p':
      /* Zero decimal places is meaningful — it rounds to integers — so this is
       * the one numeric option that is not required to be positive. */
      if (optarg[0] == '0' && optarg[1] == '\0') {
        fmt.precision = 0;
      } else if (parse_positive("--precision", optarg, &n) != 0) {
        status = 2;
      } else {
        fmt.precision = n;
      }
      break;

    case 'h':
      print_help();
      goto cleanup;

    case ':':
      /* getopt has already advanced past the offending argument, so
       * argv[optind - 1] is it, spelled the way the user typed it. */
      fprintf(stderr, "error: option '%s' requires a value\n",
              argv[optind - 1]);
      print_usage_error();
      status = 2;
      break;

    default:
      fprintf(stderr, "error: unknown option '%s'\n", argv[optind - 1]);
      print_usage_error();
      status = 2;
      break;
    }

    if (status != 0)
      goto cleanup;
  }

  if (pending_rows != MATRIX_DIM_UNSPECIFIED ||
      pending_cols != MATRIX_DIM_UNSPECIFIED) {
    fprintf(stderr, "error: --rows/--cols given with no matrix to apply them "
                    "to (they must come before a --values or --file)\n");
    status = 2;
    goto cleanup;
  }

  /* getopt_long permutes the non-options to the end, so the operation may be
   * written anywhere on the command line, but there must be exactly one. */
  if (optind >= argc) {
    fprintf(stderr, "error: missing operation\n");
    print_usage_error();
    status = 2;
    goto cleanup;
  }
  if (argc - optind > 1) {
    fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind + 1]);
    print_usage_error();
    status = 2;
    goto cleanup;
  }

  int index = find_operation(argv[optind]);
  if (index < 0) {
    fprintf(stderr,
            "error: unknown operation '%s' (expected add, sub, mul, or "
            "scale)\n",
            argv[optind]);
    status = 2;
    goto cleanup;
  }

  if (operand_count != kOperations[index].operands) {
    fprintf(stderr, "error: '%s' takes %zu %s, but %zu %s given\n",
            kOperations[index].name, kOperations[index].operands,
            kOperations[index].operands == 1 ? "matrix" : "matrices",
            operand_count, operand_count == 1 ? "was" : "were");
    status = 2;
    goto cleanup;
  }

  if (kOperations[index].needs_scalar && !have_scalar) {
    fprintf(stderr, "error: '%s' requires --scalar\n", kOperations[index].name);
    status = 2;
    goto cleanup;
  }
  if (!kOperations[index].needs_scalar && have_scalar) {
    fprintf(stderr, "error: --scalar applies only to 'scale'\n");
    status = 2;
    goto cleanup;
  }

  MatrixResult rc;
  switch (kOperations[index].op) {
  case OP_ADD:
    rc = matrix_add(&operands[0], &operands[1], &result);
    break;
  case OP_SUB:
    rc = matrix_sub(&operands[0], &operands[1], &result);
    break;
  case OP_MUL:
    rc = matrix_mul(&operands[0], &operands[1], &result);
    break;
  case OP_SCALE:
    rc = matrix_scale(&operands[0], scalar, &result);
    break;
  default:
    rc = MATRIX_ERR_BAD_SHAPE;
    break;
  }

  if (rc == MATRIX_ERR_DIM_MISMATCH) {
    /* Naming both shapes turns "incompatible dimensions" into something the
     * user can act on without re-reading their own command line. */
    fprintf(stderr, "error: cannot %s a %zux%zu matrix and a %zux%zu one\n",
            kOperations[index].name, operands[0].rows, operands[0].cols,
            operands[1].rows, operands[1].cols);
    status = 2;
    goto cleanup;
  }
  if (rc != MATRIX_OK) {
    fprintf(stderr, "matrix_ops: %s\n", matrix_result_str(rc));
    status = 1;
    goto cleanup;
  }

  rc = matrix_write(stdout, &result, &fmt);
  if (rc == MATRIX_OK && fflush(stdout) != 0)
    rc = MATRIX_ERR_WRITE;
  if (rc != MATRIX_OK) {
    fprintf(stderr, "matrix_ops: %s: %s\n", matrix_result_str(rc),
            strerror(errno));
    status = 1;
  }

cleanup:
  for (size_t i = 0; i < MAX_OPERANDS; i++)
    matrix_free(&operands[i]);
  matrix_free(&result);
  return status;
}
