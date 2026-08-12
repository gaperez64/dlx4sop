#ifndef DLX4SOP_QPF_H
#define DLX4SOP_QPF_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum qsop_qpf_scalar_backend {
  QSOP_QPF_SCALAR_MODULAR,
  QSOP_QPF_SCALAR_COMPLEX,
  QSOP_QPF_SCALAR_CYCLOTOMIC,
} qsop_qpf_scalar_backend_t;

#define QSOP_QPF_CYCLOTOMIC_MAX_ORDER 64U

#if defined(__GNUC__) || defined(__clang__)
__extension__ typedef signed __int128 qsop_qpf_i128_t;
__extension__ typedef unsigned __int128 qsop_qpf_u128_t;
#endif

/* Exact modular scalars are used under enough CRT primes to reconstruct counts. Complex scalars
 * are the direct single-mode amplitude path. In either backend root is a primitive order-th root
 * of unity and order is divisible by four. */
typedef struct qsop_qpf_scalar {
  qsop_qpf_scalar_backend_t backend;
  uint32_t order;
  uint64_t modulus;
  uint64_t root;
  uint64_t value;
  long double re;
  long double im;
  long double numeric_error_bound;
  int32_t scale_exp2;
  /* Exact element of Z[zeta_order][1/2], reduced in the power basis modulo Phi_order. */
  uint32_t cyclotomic_degree;
  uint32_t denominator_exp2;
  qsop_qpf_i128_t coefficients[QSOP_QPF_CYCLOTOMIC_MAX_ORDER];
} qsop_qpf_scalar_t;

typedef struct qsop_qpf {
  uint32_t m;
  size_t words;
  qsop_qpf_scalar_t eps;
  uint8_t *ell;       /* m coefficients modulo 4 */
  uint64_t *quad;     /* m strictly-upper bitset rows */
  uint64_t *aff_rows; /* aff_len bitset rows */
  uint64_t *aff_rhs;  /* aff_len entries in F2 */
  uint32_t aff_len;
  uint32_t aff_cap;
} qsop_qpf_t;

void qsop_qpf_scalar_modular(qsop_qpf_scalar_t *out, uint32_t order, uint64_t modulus,
                             uint64_t root, uint64_t value);
void qsop_qpf_scalar_complex(qsop_qpf_scalar_t *out, uint32_t order, uint32_t exponent);
bool qsop_qpf_scalar_cyclotomic(qsop_qpf_scalar_t *out, uint32_t order, uint32_t exponent,
                                qsop_error_t *error);
bool qsop_qpf_scalar_add(qsop_qpf_scalar_t *dst, const qsop_qpf_scalar_t *src,
                         qsop_error_t *error);
bool qsop_qpf_scalar_mul(qsop_qpf_scalar_t *dst, const qsop_qpf_scalar_t *src,
                         qsop_error_t *error);
bool qsop_qpf_scalar_mul_root(qsop_qpf_scalar_t *scalar, uint64_t exponent,
                              qsop_error_t *error);
bool qsop_qpf_scalar_negate(qsop_qpf_scalar_t *scalar, qsop_error_t *error);
bool qsop_qpf_scalar_divide_pow2(qsop_qpf_scalar_t *scalar, uint32_t exponent,
                                 qsop_error_t *error);
bool qsop_qpf_scalar_divide_u64_exact(qsop_qpf_scalar_t *scalar, uint64_t divisor,
                                      qsop_error_t *error);
bool qsop_qpf_scalar_is_zero(const qsop_qpf_scalar_t *scalar);

bool qsop_qpf_init(qsop_qpf_t *qpf, uint32_t m, const qsop_qpf_scalar_t *eps,
                   qsop_error_t *error);
void qsop_qpf_free(qsop_qpf_t *qpf);
bool qsop_qpf_clone(const qsop_qpf_t *source, qsop_qpf_t *out, qsop_error_t *error);
void qsop_qpf_set_linear(qsop_qpf_t *qpf, uint32_t variable, uint8_t coefficient);
void qsop_qpf_toggle_quadratic(qsop_qpf_t *qpf, uint32_t left, uint32_t right);
bool qsop_qpf_add_constraint(qsop_qpf_t *qpf, const uint64_t *row, uint64_t rhs,
                             qsop_error_t *error);
bool qsop_qpf_eval(const qsop_qpf_t *qpf, const uint64_t *assignment,
                   qsop_qpf_scalar_t *out, qsop_error_t *error);
bool qsop_qpf_total_sum(const qsop_qpf_t *qpf, qsop_qpf_scalar_t *out,
                        qsop_error_t *error);

#endif
