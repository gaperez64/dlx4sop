#include "dlx4sop/residue.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void set_error(qsop_error_t *error, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }

  error->path = NULL;
  error->line = 0;
  error->column = 0;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
}

bool qsop_counts_alloc(uint32_t r, uint64_t **out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residue-count allocation output");
    return false;
  }
  *out = NULL;

  if (r == 0) {
    set_error(error, "cannot allocate residue vector for modulus 0");
    return false;
  }

  uint64_t *counts = calloc(r, sizeof(*counts));
  if (counts == NULL) {
    set_error(error, "out of memory while allocating residue counts");
    return false;
  }

  *out = counts;
  return true;
}

void qsop_counts_clear(uint32_t r, uint64_t *counts) {
  if (counts == NULL) {
    return;
  }
  memset(counts, 0, (size_t)r * sizeof(*counts));
}

void qsop_counts_shift_add(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift) {
  if (r == 0 || dst == NULL || src == NULL) {
    return;
  }

  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t target = residue + delta;
    if (target >= r) {
      target -= r;
    }
    dst[target] += src[residue];
  }
}

bool qsop_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                          const uint64_t *right, qsop_error_t *error) {
  if (r == 0 || dst == NULL || left == NULL || right == NULL) {
    set_error(error, "internal error: invalid residue convolution argument");
    return false;
  }

  qsop_counts_clear(r, dst);
  for (uint32_t a = 0; a < r; a++) {
    if (left[a] == 0) {
      continue;
    }
    for (uint32_t b = 0; b < r; b++) {
      if (right[b] == 0) {
        continue;
      }
      uint32_t target = a + b;
      if (target >= r) {
        target -= r;
      }
      dst[target] += left[a] * right[b];
    }
  }
  return true;
}
