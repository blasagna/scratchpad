#include "matrix_io.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Size of the chunks matrix_read_stream reads, and its initial buffer. */
#define READ_CHUNK 4096

/* Spaces printed between columns by matrix_write. */
static const char *COLUMN_GAP = "  ";

/*
 * A growable double array, used to collect values before their shape is known.
 * The shape rules need the total count, so the values cannot go straight into
 * a Matrix.
 */
typedef struct {
  double *data;
  size_t len;
  size_t cap;
} ValueList;

static void values_free(ValueList *v) {
  free(v->data);
  v->data = NULL;
  v->len = 0;
  v->cap = 0;
}

static MatrixResult values_push(ValueList *v, double value) {
  if (v->len == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 16;
    /* The doubling itself cannot wrap before this does, so one check covers
     * both the new capacity and the allocation size. */
    if (cap > SIZE_MAX / sizeof(double))
      return MATRIX_ERR_OVERFLOW;
    double *data = realloc(v->data, cap * sizeof(double));
    if (!data)
      return MATRIX_ERR_NOMEM;
    v->data = data;
    v->cap = cap;
  }
  v->data[v->len++] = value;
  return MATRIX_OK;
}

/* True for the space characters that separate values within a line. */
static int is_inline_space(char c) {
  return c != '\n' && isspace((unsigned char)c);
}

/*
 * Parses one whitespace-delimited token as a finite double.
 *
 * strtod is the parser, but not the whole rule: it also accepts "nan", "inf",
 * and "infinity", and reports ERANGE for a value too large to represent. All of
 * those are rejected here so the accepted set is written down in one place
 * rather than inherited from the platform — the later C++ and Rust ports have
 * to match this, not their own standard libraries.
 *
 * ERANGE for an underflowing value is *not* an error: strtod sets it when a
 * result is subnormal or flushes to zero, which is a fine answer for input like
 * 1e-400. Only the overflow case, which yields an infinity, is refused.
 */
static MatrixResult parse_number(const char *token, size_t len, double *out) {
  /* strtod needs a NUL-terminated string, and the token points into a buffer
   * that must not be modified, so it is copied. A stack buffer covers every
   * realistic number; anything longer is not one. */
  char buf[512];
  if (len == 0 || len >= sizeof(buf))
    return MATRIX_ERR_BAD_NUMBER;
  memcpy(buf, token, len);
  buf[len] = '\0';

  errno = 0;
  char *endp;
  double value = strtod(buf, &endp);

  /* endp == buf means nothing was consumed; a non-NUL *endp means trailing
   * junk, which catches "1.2.3" and "5x". */
  if (endp == buf || *endp != '\0')
    return MATRIX_ERR_BAD_NUMBER;
  if (!isfinite(value))
    return MATRIX_ERR_BAD_NUMBER;

  *out = value;
  return MATRIX_OK;
}

/*
 * Scans text into values, and reports the layout the text itself implies.
 *
 * *rows_out is the number of non-blank lines and *cols_out the number of values
 * on each of them; a ragged input is rejected here, before any shape override
 * is considered.
 */
static MatrixResult scan_values(const char *text, ValueList *values,
                                size_t *rows_out, size_t *cols_out) {
  size_t rows = 0;
  size_t cols = 0;
  const char *p = text;

  while (*p != '\0') {
    /* Collect one line's worth of values. */
    size_t line_cols = 0;
    while (*p != '\0' && *p != '\n') {
      if (is_inline_space(*p)) {
        p++;
        continue;
      }

      const char *start = p;
      while (*p != '\0' && *p != '\n' && !is_inline_space(*p))
        p++;

      double value;
      MatrixResult rc = parse_number(start, (size_t)(p - start), &value);
      if (rc != MATRIX_OK)
        return rc;
      rc = values_push(values, value);
      if (rc != MATRIX_OK)
        return rc;
      line_cols++;
    }

    if (*p == '\n')
      p++;

    /* A blank line carries no values and is not a row; that is what lets input
     * be padded with blank lines or end in a newline. */
    if (line_cols == 0)
      continue;

    if (rows == 0)
      cols = line_cols;
    else if (line_cols != cols)
      return MATRIX_ERR_RAGGED;
    rows++;
  }

  *rows_out = rows;
  *cols_out = cols;
  return MATRIX_OK;
}

/*
 * Turns the layout's shape and the caller's requested dimensions into the final
 * one, per the rules documented on matrix_parse_text.
 */
static MatrixResult resolve_shape(size_t count, size_t natural_rows,
                                  size_t natural_cols, size_t want_rows,
                                  size_t want_cols, size_t *rows_out,
                                  size_t *cols_out) {
  if (want_rows == MATRIX_DIM_UNSPECIFIED &&
      want_cols == MATRIX_DIM_UNSPECIFIED) {
    *rows_out = natural_rows;
    *cols_out = natural_cols;
    return MATRIX_OK;
  }

  if (want_rows != MATRIX_DIM_UNSPECIFIED &&
      want_cols != MATRIX_DIM_UNSPECIFIED) {
    /* Guard the product before comparing it, or a wrapped want_rows * want_cols
     * could equal count and produce a matrix of the wrong shape. */
    if (want_rows > SIZE_MAX / want_cols)
      return MATRIX_ERR_OVERFLOW;
    if (want_rows * want_cols != count)
      return MATRIX_ERR_BAD_SHAPE;
    *rows_out = want_rows;
    *cols_out = want_cols;
    return MATRIX_OK;
  }

  /* Exactly one dimension is given here, since the two cases above returned for
   * neither and for both. The branches are spelled out rather than sharing a
   * "given" variable so that each divisor is visibly the one the enclosing
   * condition just proved nonzero. */
  if (want_rows != MATRIX_DIM_UNSPECIFIED) {
    if (count % want_rows != 0)
      return MATRIX_ERR_BAD_SHAPE;
    *rows_out = want_rows;
    *cols_out = count / want_rows;
    return MATRIX_OK;
  }

  if (count % want_cols != 0)
    return MATRIX_ERR_BAD_SHAPE;
  *rows_out = count / want_cols;
  *cols_out = want_cols;
  return MATRIX_OK;
}

MatrixFormat matrix_format_default(void) {
  MatrixFormat fmt;
  fmt.precision = MATRIX_DEFAULT_PRECISION;
  return fmt;
}

MatrixResult matrix_parse_text(const char *text, size_t want_rows,
                               size_t want_cols, Matrix *out) {
  ValueList values = {NULL, 0, 0};
  size_t natural_rows = 0;
  size_t natural_cols = 0;

  MatrixResult rc = scan_values(text, &values, &natural_rows, &natural_cols);
  if (rc != MATRIX_OK)
    goto done;

  if (values.len == 0) {
    rc = MATRIX_ERR_EMPTY;
    goto done;
  }

  size_t rows;
  size_t cols;
  rc = resolve_shape(values.len, natural_rows, natural_cols, want_rows,
                     want_cols, &rows, &cols);
  if (rc != MATRIX_OK)
    goto done;

  Matrix m;
  rc = matrix_create(rows, cols, &m);
  if (rc != MATRIX_OK)
    goto done;

  /* The values are already in row-major order, and resolve_shape has
   * established that there are exactly rows * cols of them. */
  memcpy(m.data, values.data, values.len * sizeof(double));
  *out = m;

done:
  values_free(&values);
  return rc;
}

MatrixResult matrix_read_stream(FILE *in, size_t want_rows, size_t want_cols,
                                Matrix *out) {
  char *text = NULL;
  size_t len = 0;
  size_t cap = 0;
  MatrixResult rc = MATRIX_OK;

  for (;;) {
    /* Keep room for READ_CHUNK bytes plus the terminating NUL. */
    if (cap - len < READ_CHUNK + 1) {
      size_t new_cap = cap ? cap * 2 : READ_CHUNK + 1;
      char *grown = realloc(text, new_cap);
      if (!grown) {
        rc = MATRIX_ERR_NOMEM;
        goto done;
      }
      text = grown;
      cap = new_cap;
    }

    size_t n = fread(text + len, 1, READ_CHUNK, in);
    len += n;
    if (n < READ_CHUNK) {
      if (ferror(in))
        rc = MATRIX_ERR_READ;
      break;
    }
  }

  if (rc == MATRIX_OK) {
    /* A NUL in the input would end the text early. Rejecting it as a bad
     * number is more honest than silently parsing the prefix. */
    text[len] = '\0';
    if (strlen(text) != len)
      rc = MATRIX_ERR_BAD_NUMBER;
    else
      rc = matrix_parse_text(text, want_rows, want_cols, out);
  }

done:;
  int saved = errno;
  free(text);
  errno = saved;
  return rc;
}

/*
 * Renders one element into a newly malloc'd string the caller must free,
 * trimming trailing zeros so an integral value prints as an integer. Stores
 * the length in *len_out. Returns NULL on allocation failure.
 *
 * The buffer is sized by asking snprintf how much room the rendering needs
 * rather than by guessing. A fixed buffer has to cover the worst case, which
 * is not 512 bytes: "%.*f" of a value near DBL_MAX needs 309 digits before the
 * point plus whatever --precision asks for after it, so `--scalar 1e300
 * --precision 300` wants ~611. The fixed buffer used to make that legitimate
 * render fail, and fail as MATRIX_ERR_NOMEM with an unrelated errno, printing
 * "out of memory: Success".
 */
static char *format_element(double value, int precision, int *len_out) {
  int n = snprintf(NULL, 0, "%.*f", precision, value);
  if (n < 0)
    return NULL;

  char *buf = malloc((size_t)n + 1);
  if (!buf)
    return NULL;
  snprintf(buf, (size_t)n + 1, "%.*f", precision, value);

  /* Only a rendering that actually has a fraction can be trimmed; with
   * precision 0 there is no '.' and the digits are all significant. */
  if (memchr(buf, '.', (size_t)n) != NULL) {
    while (n > 0 && buf[n - 1] == '0')
      n--;
    if (n > 0 && buf[n - 1] == '.')
      n--;
    buf[n] = '\0';
  }

  /* Normalize a negative zero. It arrives two ways: as the double -0.0, and as
   * a small negative value the precision rounded away (-0.0001 at precision 2).
   * Checking the rendering rather than the value catches both, and "-0" in a
   * result reads as a bug rather than as arithmetic. */
  if (strcmp(buf, "-0") == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    n = 1;
  }

  *len_out = n;
  return buf;
}

MatrixResult matrix_write(FILE *out, const Matrix *m, const MatrixFormat *fmt) {
  size_t count = m->rows * m->cols;

  /* Every element is rendered before anything is printed, because the widest
   * one sets the column width that all of them are justified into. */
  char **cells = calloc(count, sizeof(char *));
  if (!cells)
    return MATRIX_ERR_NOMEM;

  MatrixResult rc = MATRIX_OK;
  size_t width = 0;

  for (size_t i = 0; i < count; i++) {
    int n = 0;
    cells[i] = format_element(m->data[i], fmt->precision, &n);
    if (!cells[i]) {
      rc = MATRIX_ERR_NOMEM;
      goto done;
    }
    if ((size_t)n > width)
      width = (size_t)n;
  }

  for (size_t r = 0; r < m->rows; r++) {
    for (size_t c = 0; c < m->cols; c++) {
      if (c > 0 && fputs(COLUMN_GAP, out) == EOF) {
        rc = MATRIX_ERR_WRITE;
        goto done;
      }
      /* Right-justified into the common width, so decimal points line up down
       * each column. */
      if (fprintf(out, "%*s", (int)width, cells[r * m->cols + c]) < 0) {
        rc = MATRIX_ERR_WRITE;
        goto done;
      }
    }
    if (fputc('\n', out) == EOF) {
      rc = MATRIX_ERR_WRITE;
      goto done;
    }
  }

done:;
  int saved = errno;
  for (size_t i = 0; i < count; i++)
    free(cells[i]);
  free(cells);
  errno = saved;
  return rc;
}
