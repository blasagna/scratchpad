#ifndef MATRIX_OPS_MATRIX_H
#define MATRIX_OPS_MATRIX_H

#include <stddef.h>

/*
 * Outcome of a matrix operation. A nonzero value names what went wrong. Most of
 * these carry no errno: the failure is about the values or their shapes rather
 * than about a libc call. The exceptions are MATRIX_ERR_READ and
 * MATRIX_ERR_WRITE, raised by the stream functions in matrix_io.h, which leave
 * the failing call's errno in place so the caller may pair them with strerror.
 */
typedef enum {
  MATRIX_OK = 0,
  MATRIX_ERR_NOMEM,        /* out of memory */
  MATRIX_ERR_DIM_MISMATCH, /* add/sub shapes differ, or mul inner dims differ */
  MATRIX_ERR_BAD_NUMBER,   /* a token was not a finite number */
  MATRIX_ERR_RAGGED,       /* input rows have differing column counts */
  MATRIX_ERR_EMPTY,        /* the input held no values */
  MATRIX_ERR_BAD_SHAPE,    /* requested dimensions do not fit the value count */
  MATRIX_ERR_READ,         /* a read error occurred on the input stream */
  MATRIX_ERR_WRITE,        /* a write error occurred on the output stream */
  MATRIX_ERR_OVERFLOW,     /* rows * cols does not fit in size_t */
} MatrixResult;

/*
 * A dense 2D matrix of doubles.
 *
 * Elements are stored row-major, so element (r, c) lives at data[r * cols + c];
 * matrix_at and matrix_set spell that out rather than leaving it to callers.
 * A matrix is always non-empty in practice — matrix_create rejects a zero
 * dimension — so data is never NULL on a matrix that matrix_create returned.
 *
 * Ownership: data is owned by the matrix and released by matrix_free.
 */
typedef struct {
  size_t rows;
  size_t cols;
  double *data;
} Matrix;

/* Returns a short human-readable label for a MatrixResult. */
const char *matrix_result_str(MatrixResult r);

/*
 * matrix_create - allocates a rows x cols matrix with every element zero.
 *
 * Input:  rows, cols - both must be nonzero; a zero dimension is
 *         MATRIX_ERR_BAD_SHAPE, since no operation here has a meaningful
 *         answer for a matrix with no elements.
 *
 * Output: Returns MATRIX_OK with the new matrix in *out, MATRIX_ERR_OVERFLOW
 *         if rows * cols would wrap, MATRIX_ERR_BAD_SHAPE for a zero
 *         dimension, or MATRIX_ERR_NOMEM. On any failure *out is left
 *         untouched, so a caller may zero-initialize its Matrix once and call
 *         matrix_free unconditionally on the way out.
 */
MatrixResult matrix_create(size_t rows, size_t cols, Matrix *out);

/*
 * matrix_free - releases a matrix's storage and resets it to empty.
 *
 * Safe on a NULL pointer and on an already-freed or zero-initialized matrix,
 * which is what makes a single unconditional cleanup path possible.
 */
void matrix_free(Matrix *m);

/*
 * matrix_at / matrix_set - read or write element (row, col).
 *
 * The indices are a caller precondition: they must satisfy row < m->rows and
 * col < m->cols. They are not checked, because every caller here derives them
 * from the matrix's own dimensions.
 */
double matrix_at(const Matrix *m, size_t row, size_t col);
void matrix_set(Matrix *m, size_t row, size_t col, double value);

/*
 * matrix_add / matrix_sub - element-wise sum and difference.
 *
 * Input:  a, b - must have identical dimensions.
 *
 * Output: Returns MATRIX_OK with a newly allocated result in *out,
 *         MATRIX_ERR_DIM_MISMATCH if the shapes differ, or MATRIX_ERR_NOMEM.
 *         *out is written only on success.
 */
MatrixResult matrix_add(const Matrix *a, const Matrix *b, Matrix *out);
MatrixResult matrix_sub(const Matrix *a, const Matrix *b, Matrix *out);

/*
 * matrix_mul - matrix product a * b.
 *
 * Input:  a is m x n and b must be n x p; the shared inner dimension is what
 *         the two have to agree on, not their full shapes. The product is
 *         m x p. The operation is not commutative, so the argument order is
 *         the CLI's operand order.
 *
 * Output: Returns MATRIX_OK with the m x p result in *out,
 *         MATRIX_ERR_DIM_MISMATCH if a->cols != b->rows, or MATRIX_ERR_NOMEM.
 */
MatrixResult matrix_mul(const Matrix *a, const Matrix *b, Matrix *out);

/*
 * matrix_scale - multiplies every element of a by scalar.
 *
 * Output: Returns MATRIX_OK with the result in *out, or MATRIX_ERR_NOMEM.
 */
MatrixResult matrix_scale(const Matrix *a, double scalar, Matrix *out);

#endif
