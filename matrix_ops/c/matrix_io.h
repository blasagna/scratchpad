#ifndef MATRIX_OPS_MATRIX_IO_H
#define MATRIX_OPS_MATRIX_IO_H

#include <stdio.h>

#include "matrix.h"

/* Decimal places used when the caller does not pick one. */
#define MATRIX_DEFAULT_PRECISION 4

/*
 * Sentinel for "the caller did not specify this dimension". Zero is free for
 * the job because matrix_create rejects a zero dimension anyway, so no real
 * matrix can have one.
 */
#define MATRIX_DIM_UNSPECIFIED ((size_t)0)

/*
 * How a matrix is rendered by matrix_write.
 */
typedef struct {
  /* Maximum decimal places. Trailing zeros are trimmed afterwards, so this is
   * an upper bound on the fraction shown, not a fixed width: with the default
   * of 4, 1.0 prints as "1" and 1/3 as "0.3333". */
  int precision;
} MatrixFormat;

/* Returns a MatrixFormat populated with defaults. */
MatrixFormat matrix_format_default(void);

/*
 * matrix_parse_text - builds a matrix from whitespace-separated numbers.
 *
 * Values are read in row-major order. The shape comes from the text's own
 * layout unless the caller overrides it:
 *
 *   1. A single non-blank line is a 1 x N row vector; several non-blank lines
 *      are rows, one per line.
 *   2. Ragged input — lines with differing value counts — is always
 *      MATRIX_ERR_RAGGED, even when want_rows and want_cols would make the
 *      layout irrelevant. A ragged file is far more often a typo than an
 *      intent, so it is not silently reshaped.
 *   3. The requested dimensions are then applied:
 *        - neither given: the layout's own shape
 *        - both given: requires want_rows * want_cols == the value count, and
 *          reshapes row-major, which is what turns a flat list of six values
 *          into a 2 x 3
 *        - only one given: the other is derived by division, and a count that
 *          does not divide evenly is MATRIX_ERR_BAD_SHAPE
 *
 * A number is anything strtod accepts *except* the non-finite spellings (nan,
 * inf, infinity) and anything that overflows to infinity, all of which are
 * MATRIX_ERR_BAD_NUMBER. Pinning that set here rather than inheriting whatever
 * the platform's strtod allows is what will keep the later C++ and Rust ports
 * agreeing with this one.
 *
 * Input:  text - NUL-terminated; blank lines and surrounding whitespace are
 *         ignored.
 *         want_rows, want_cols - MATRIX_DIM_UNSPECIFIED when not requested.
 *
 * Output: Returns MATRIX_OK with a newly allocated matrix in *out, which the
 *         caller frees with matrix_free. On failure returns
 *         MATRIX_ERR_EMPTY, MATRIX_ERR_RAGGED, MATRIX_ERR_BAD_NUMBER,
 *         MATRIX_ERR_BAD_SHAPE, MATRIX_ERR_OVERFLOW, or MATRIX_ERR_NOMEM, and
 *         leaves *out untouched.
 */
MatrixResult matrix_parse_text(const char *text, size_t want_rows,
                               size_t want_cols, Matrix *out);

/*
 * matrix_read_stream - reads all of in, then parses it as matrix_parse_text.
 *
 * The whole stream is buffered rather than streamed. Unlike text_analyzer,
 * where streaming was a requirement, a matrix has to be held in memory in its
 * entirety anyway, so reading the text twice its size buys nothing.
 *
 * Output: As matrix_parse_text, plus MATRIX_ERR_READ if in reported an error,
 *         with errno as the failing call left it.
 */
MatrixResult matrix_read_stream(FILE *in, size_t want_rows, size_t want_cols,
                                Matrix *out);

/*
 * matrix_write - prints a matrix to out, one row per line.
 *
 * Every element is rendered first, then the widest rendering sets a common
 * column width and all of them are right-justified into it and separated by two
 * spaces, so decimal points line up down each column. Each line, including the
 * last, ends in a newline.
 *
 * Elements are formatted with "%.*f" and then stripped of trailing zeros and a
 * bare trailing '.', so integral values print as integers. "%g" is deliberately
 * not used: it counts significant digits rather than decimals and switches to
 * scientific notation for large values, which reads badly in a column. A
 * negative zero is normalized to "0".
 *
 * Because the width follows the data, a matrix holding a value like 1e300
 * prints a column some 300 characters wide. That is left uncapped rather than
 * truncating a value the user asked to see.
 *
 * Output: Returns MATRIX_OK, MATRIX_ERR_NOMEM, or MATRIX_ERR_WRITE if a write
 *         to out failed, with errno as the failing call left it.
 */
MatrixResult matrix_write(FILE *out, const Matrix *m, const MatrixFormat *fmt);

#endif
