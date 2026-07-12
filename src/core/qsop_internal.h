#ifndef DLX4SOP_QSOP_INTERNAL_H
#define DLX4SOP_QSOP_INTERNAL_H

/* Shared internal helpers duplicated verbatim (or near-verbatim) across the solver backends and
 * core translation units. Everything here is static inline: each including TU gets its own copy
 * of the code, same as before, but from one source location instead of N hand-kept ones. */

#include "dlx4sop/qsop.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static inline void qsop_set_error(qsop_error_t *error, const char *fmt, ...) {
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

static inline void qsop_add_saturating_u64(uint64_t *dst, uint64_t value) {
  if (dst == NULL) {
    return;
  }
  if (UINT64_MAX - *dst < value) {
    *dst = UINT64_MAX;
  } else {
    *dst += value;
  }
}

static inline uint64_t qsop_saturating_add_u64(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static inline uint64_t qsop_saturating_mul_u64(uint64_t left, uint64_t right) {
  if (left != 0 && right > UINT64_MAX / left) {
    return UINT64_MAX;
  }
  return left * right;
}

static const long double QSOP_TWO_PI_L =
    6.283185307179586476925286766559005768394338798750211641949889L;

/* omega_r^{target_mode * k}, computed directly from a scalar angle: r never sizes an array in
 * this path, only a divisor here, so table cost is fully independent of r. */
static inline void qsop_root_of_unity_l(uint64_t r, uint32_t target_mode, uint64_t k,
                                        long double *re, long double *im) {
  const long double angle =
      QSOP_TWO_PI_L * (long double)target_mode * (long double)k / (long double)r;
  *re = cosl(angle);
  *im = sinl(angle);
}

static inline void qsop_root_of_unity_f64(uint64_t r, uint32_t target_mode, uint64_t k, double *re,
                                          double *im) {
  const double angle = (double)QSOP_TWO_PI_L * (double)target_mode * (double)k / (double)r;
  *re = cos(angle);
  *im = sin(angle);
}

/* Conservative worst-case bound on floating-point error accumulated by the single-mode complex
 * DP, following standard backward-error analysis: each complex multiply-accumulate or complex
 * add counted in complex_ops contributes at most a small constant multiple of the type's unit
 * roundoff to the (unnormalized) result's error. Not tight by design -- validated empirically
 * against exact histogram reconstruction in the differential tests. Shared between treewidth's
 * and rankwidth's single-mode solvers, which use the identical derivation. */
static inline long double qsop_single_mode_error_bound_l(uint64_t complex_ops) {
  static const long double ops_per_step =
      8.0L; /* complex multiply: 4 mul + 2 add/sub, plus margin */
  return (long double)complex_ops * ops_per_step * LDBL_EPSILON;
}

static inline long double qsop_single_mode_error_bound_f64(uint64_t complex_ops) {
  static const long double ops_per_step = 8.0L;
  return (long double)complex_ops * ops_per_step * DBL_EPSILON;
}

#endif
