#include "matrix.h"

#include <stdint.h>
#include <stdlib.h>

const char *matrix_result_str(MatrixResult r) {
  switch (r) {
  case MATRIX_OK:
    return "success";
  case MATRIX_ERR_NOMEM:
    return "out of memory";
  case MATRIX_ERR_DIM_MISMATCH:
    return "incompatible matrix dimensions";
  case MATRIX_ERR_BAD_NUMBER:
    return "value is not a finite number";
  case MATRIX_ERR_RAGGED:
    return "rows do not all have the same number of values";
  case MATRIX_ERR_EMPTY:
    return "no values given";
  case MATRIX_ERR_BAD_SHAPE:
    return "requested dimensions do not fit the values";
  case MATRIX_ERR_READ:
    return "error reading input";
  case MATRIX_ERR_WRITE:
    return "error writing output";
  case MATRIX_ERR_OVERFLOW:
    return "matrix is too large";
  }
  return "unknown error";
}

MatrixResult matrix_create(size_t rows, size_t cols, Matrix *out) {
  /* Every operation here is defined in terms of elements, so a matrix with no
   * elements has no useful answer to give. Refusing it up front also means the
   * rest of this file never has to reason about a NULL data pointer. */
  if (rows == 0 || cols == 0)
    return MATRIX_ERR_BAD_SHAPE;

  /* Checked before multiplying, not after: the wrapped product would be a
   * plausible-looking small number and calloc would happily satisfy it. */
  if (rows > SIZE_MAX / cols)
    return MATRIX_ERR_OVERFLOW;

  size_t count = rows * cols;
  double *data = calloc(count, sizeof(double));
  if (!data)
    return MATRIX_ERR_NOMEM;

  out->rows = rows;
  out->cols = cols;
  out->data = data;
  return MATRIX_OK;
}

void matrix_free(Matrix *m) {
  if (!m)
    return;
  free(m->data);
  m->data = NULL;
  m->rows = 0;
  m->cols = 0;
}

double matrix_at(const Matrix *m, size_t row, size_t col) {
  return m->data[row * m->cols + col];
}

void matrix_set(Matrix *m, size_t row, size_t col, double value) {
  m->data[row * m->cols + col] = value;
}

/*
 * Shared body of matrix_add and matrix_sub: both walk the elements in step and
 * differ only in the sign applied to b. Passing the sign rather than a function
 * pointer keeps the inner loop a plain multiply-add.
 */
static MatrixResult add_scaled(const Matrix *a, const Matrix *b, double b_sign,
                               Matrix *out) {
  if (a->rows != b->rows || a->cols != b->cols)
    return MATRIX_ERR_DIM_MISMATCH;

  Matrix result;
  MatrixResult rc = matrix_create(a->rows, a->cols, &result);
  if (rc != MATRIX_OK)
    return rc;

  size_t count = a->rows * a->cols;
  for (size_t i = 0; i < count; i++)
    result.data[i] = a->data[i] + b_sign * b->data[i];

  *out = result;
  return MATRIX_OK;
}

MatrixResult matrix_add(const Matrix *a, const Matrix *b, Matrix *out) {
  return add_scaled(a, b, 1.0, out);
}

MatrixResult matrix_sub(const Matrix *a, const Matrix *b, Matrix *out) {
  return add_scaled(a, b, -1.0, out);
}

MatrixResult matrix_mul(const Matrix *a, const Matrix *b, Matrix *out) {
  /* Only the inner dimension has to agree; the outer two become the result's
   * shape. */
  if (a->cols != b->rows)
    return MATRIX_ERR_DIM_MISMATCH;

  Matrix result;
  MatrixResult rc = matrix_create(a->rows, b->cols, &result);
  if (rc != MATRIX_OK)
    return rc;

  for (size_t i = 0; i < a->rows; i++) {
    for (size_t j = 0; j < b->cols; j++) {
      double sum = 0.0;
      for (size_t k = 0; k < a->cols; k++)
        sum += matrix_at(a, i, k) * matrix_at(b, k, j);
      matrix_set(&result, i, j, sum);
    }
  }

  *out = result;
  return MATRIX_OK;
}

MatrixResult matrix_scale(const Matrix *a, double scalar, Matrix *out) {
  Matrix result;
  MatrixResult rc = matrix_create(a->rows, a->cols, &result);
  if (rc != MATRIX_OK)
    return rc;

  size_t count = a->rows * a->cols;
  for (size_t i = 0; i < count; i++)
    result.data[i] = a->data[i] * scalar;

  *out = result;
  return MATRIX_OK;
}
