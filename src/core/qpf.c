#include "dlx4sop/qpf.h"
#include "dlx4sop/residue.h"
#include "qsop_internal.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef qsop_qpf_i128_t qpf_i128_t;

static atomic_uint cyclotomic_cache_state = 0;
static uint32_t cyclotomic_degrees[QSOP_QPF_CYCLOTOMIC_MAX_ORDER + 1U];
static int64_t cyclotomic_polynomials[QSOP_QPF_CYCLOTOMIC_MAX_ORDER + 1U]
                                      [QSOP_QPF_CYCLOTOMIC_MAX_ORDER + 1U];

static void cyclotomic_cache_build(void) {
  for (uint32_t n = 1; n <= QSOP_QPF_CYCLOTOMIC_MAX_ORDER; n++) {
    int64_t polynomial[QSOP_QPF_CYCLOTOMIC_MAX_ORDER + 1U] = {0};
    polynomial[0] = -1;
    polynomial[n] = 1;
    uint32_t degree = n;
    for (uint32_t divisor = 1; divisor < n; divisor++) {
      if (n % divisor != 0) {
        continue;
      }
      const uint32_t divisor_degree = cyclotomic_degrees[divisor];
      int64_t quotient[QSOP_QPF_CYCLOTOMIC_MAX_ORDER + 1U] = {0};
      for (uint32_t k = degree + 1U; k-- > divisor_degree;) {
        const int64_t leading = polynomial[k];
        quotient[k - divisor_degree] = leading;
        for (uint32_t j = 0; j <= divisor_degree; j++) {
          polynomial[k - divisor_degree + j] -=
              leading * cyclotomic_polynomials[divisor][j];
        }
      }
      degree -= divisor_degree;
      memcpy(polynomial, quotient, sizeof(polynomial));
    }
    cyclotomic_degrees[n] = degree;
    memcpy(cyclotomic_polynomials[n], polynomial, sizeof(polynomial));
  }
}

static void cyclotomic_cache_ensure(void) {
  unsigned expected = 0;
  if (atomic_compare_exchange_strong(&cyclotomic_cache_state, &expected, 1U)) {
    cyclotomic_cache_build();
    atomic_store(&cyclotomic_cache_state, 2U);
    return;
  }
  while (atomic_load(&cyclotomic_cache_state) != 2U) {
  }
}

static bool i128_add(qpf_i128_t left, qpf_i128_t right, qpf_i128_t *out,
                     qsop_error_t *error) {
  if (__builtin_add_overflow(left, right, out)) {
    qsop_set_error(error, "exact QPF cyclotomic coefficient overflow");
    return false;
  }
  return true;
}

static bool i128_mul(qpf_i128_t left, qpf_i128_t right, qpf_i128_t *out,
                     qsop_error_t *error) {
  if (__builtin_mul_overflow(left, right, out)) {
    qsop_set_error(error, "exact QPF cyclotomic coefficient overflow");
    return false;
  }
  return true;
}

static bool i128_shift(qpf_i128_t value, uint32_t shift, qpf_i128_t *out,
                       qsop_error_t *error) {
  if (value == 0) {
    *out = 0;
    return true;
  }
  if (shift >= 127U) {
    qsop_set_error(error, "exact QPF cyclotomic denominator alignment overflow");
    return false;
  }
  return i128_mul(value, (qpf_i128_t)1 << shift, out, error);
}

static void cyclotomic_normalize(qsop_qpf_scalar_t *scalar) {
  while (scalar->denominator_exp2 != 0U) {
    bool all_even = true;
    for (uint32_t i = 0; i < scalar->cyclotomic_degree; i++) {
      all_even &= (scalar->coefficients[i] & 1) == 0;
    }
    if (!all_even) {
      break;
    }
    for (uint32_t i = 0; i < scalar->cyclotomic_degree; i++) {
      scalar->coefficients[i] /= 2;
    }
    scalar->denominator_exp2--;
  }
}

static bool cyclotomic_reduce(uint32_t order, qpf_i128_t *polynomial, uint32_t max_degree,
                              qsop_error_t *error) {
  cyclotomic_cache_ensure();
  const uint32_t degree = cyclotomic_degrees[order];
  for (uint32_t k = max_degree + 1U; k-- > degree;) {
    const qpf_i128_t leading = polynomial[k];
    polynomial[k] = 0;
    if (leading == 0) {
      continue;
    }
    for (uint32_t j = 0; j < degree; j++) {
      qpf_i128_t product = 0;
      qpf_i128_t updated = 0;
      if (!i128_mul(leading, (qpf_i128_t)cyclotomic_polynomials[order][j], &product,
                    error) ||
          !i128_add(polynomial[k - degree + j], -product, &updated, error)) {
        return false;
      }
      polynomial[k - degree + j] = updated;
    }
  }
  return true;
}

static size_t qpf_words(uint32_t m) {
  return ((size_t)m + 63U) / 64U;
}

static bool bit_get(const uint64_t *bits, uint32_t bit) {
  return bits != NULL && ((bits[bit / 64U] >> (bit % 64U)) & 1U) != 0;
}

static void bit_toggle(uint64_t *bits, uint32_t bit) {
  bits[bit / 64U] ^= UINT64_C(1) << (bit % 64U);
}

static void bit_set(uint64_t *bits, uint32_t bit) {
  bits[bit / 64U] |= UINT64_C(1) << (bit % 64U);
}

static uint32_t parity_and(const uint64_t *left, const uint64_t *right, size_t words) {
  uint32_t parity = 0;
  for (size_t i = 0; i < words; i++) {
#if defined(__GNUC__) || defined(__clang__)
    parity ^= (uint32_t)__builtin_parityll((unsigned long long)(left[i] & right[i]));
#else
    uint64_t value = left[i] & right[i];
    while (value != 0) {
      parity ^= 1U;
      value &= value - 1U;
    }
#endif
  }
  return parity & 1U;
}

static void scalar_renormalize(qsop_qpf_scalar_t *scalar) {
  if (scalar->backend != QSOP_QPF_SCALAR_COMPLEX) {
    return;
  }
  const long double magnitude = fmaxl(fabsl(scalar->re), fabsl(scalar->im));
  if (magnitude == 0.0L) {
    scalar->scale_exp2 = 0;
    return;
  }
  int exponent = 0;
  (void)frexpl(magnitude, &exponent);
  scalar->re = ldexpl(scalar->re, -exponent);
  scalar->im = ldexpl(scalar->im, -exponent);
  scalar->numeric_error_bound = ldexpl(scalar->numeric_error_bound, -exponent);
  scalar->scale_exp2 += exponent;
}

void qsop_qpf_scalar_modular(qsop_qpf_scalar_t *out, uint32_t order, uint64_t modulus,
                             uint64_t root, uint64_t value) {
  *out = (qsop_qpf_scalar_t){
      .backend = QSOP_QPF_SCALAR_MODULAR,
      .order = order,
      .modulus = modulus,
      .root = root,
      .value = modulus == 0 ? value : value % modulus,
  };
}

void qsop_qpf_scalar_complex(qsop_qpf_scalar_t *out, uint32_t order, uint32_t exponent) {
  const long double angle = 2.0L * acosl(-1.0L) * (long double)(exponent % order) /
                            (long double)order;
  *out = (qsop_qpf_scalar_t){
      .backend = QSOP_QPF_SCALAR_COMPLEX,
      .order = order,
      .re = cosl(angle),
      .im = sinl(angle),
      .numeric_error_bound = 4.0L * LDBL_EPSILON,
  };
}

bool qsop_qpf_scalar_cyclotomic(qsop_qpf_scalar_t *out, uint32_t order, uint32_t exponent,
                                qsop_error_t *error) {
  if (out == NULL || order == 0U || order > QSOP_QPF_CYCLOTOMIC_MAX_ORDER || order % 4U != 0U) {
    qsop_set_error(error, "exact QPF cyclotomic order must be a multiple of four at most %u",
                   QSOP_QPF_CYCLOTOMIC_MAX_ORDER);
    return false;
  }
  cyclotomic_cache_ensure();
  *out = (qsop_qpf_scalar_t){
      .backend = QSOP_QPF_SCALAR_CYCLOTOMIC,
      .order = order,
      .cyclotomic_degree = cyclotomic_degrees[order],
  };
  qpf_i128_t polynomial[QSOP_QPF_CYCLOTOMIC_MAX_ORDER + 1U] = {0};
  polynomial[exponent % order] = 1;
  if (!cyclotomic_reduce(order, polynomial, order - 1U, error)) {
    return false;
  }
  memcpy(out->coefficients, polynomial,
         (size_t)out->cyclotomic_degree * sizeof(*out->coefficients));
  return true;
}

static bool scalar_compatible(const qsop_qpf_scalar_t *left,
                              const qsop_qpf_scalar_t *right, qsop_error_t *error) {
  if (left->backend != right->backend || left->order != right->order ||
      (left->backend == QSOP_QPF_SCALAR_MODULAR &&
       (left->modulus != right->modulus || left->root != right->root)) ||
      (left->backend == QSOP_QPF_SCALAR_CYCLOTOMIC &&
       left->cyclotomic_degree != right->cyclotomic_degree)) {
    qsop_set_error(error, "incompatible QPF scalar backends");
    return false;
  }
  return true;
}

bool qsop_qpf_scalar_add(qsop_qpf_scalar_t *dst, const qsop_qpf_scalar_t *src,
                         qsop_error_t *error) {
  if (dst == NULL || src == NULL || !scalar_compatible(dst, src, error)) {
    return false;
  }
  if (dst->backend == QSOP_QPF_SCALAR_MODULAR) {
    dst->value = qsop_mod_add_u64(dst->value, src->value, dst->modulus);
    return true;
  }
  if (dst->backend == QSOP_QPF_SCALAR_CYCLOTOMIC) {
    const uint32_t denominator = dst->denominator_exp2 > src->denominator_exp2
                                     ? dst->denominator_exp2
                                     : src->denominator_exp2;
    for (uint32_t i = 0; i < dst->cyclotomic_degree; i++) {
      qpf_i128_t left = 0;
      qpf_i128_t right = 0;
      if (!i128_shift(dst->coefficients[i], denominator - dst->denominator_exp2, &left, error) ||
          !i128_shift(src->coefficients[i], denominator - src->denominator_exp2, &right, error) ||
          !i128_add(left, right, &dst->coefficients[i], error)) {
        return false;
      }
    }
    dst->denominator_exp2 = denominator;
    cyclotomic_normalize(dst);
    return true;
  }
  const int32_t exponent = dst->scale_exp2 > src->scale_exp2 ? dst->scale_exp2 : src->scale_exp2;
  dst->re = ldexpl(dst->re, dst->scale_exp2 - exponent) +
            ldexpl(src->re, src->scale_exp2 - exponent);
  dst->im = ldexpl(dst->im, dst->scale_exp2 - exponent) +
            ldexpl(src->im, src->scale_exp2 - exponent);
  dst->numeric_error_bound =
      ldexpl(dst->numeric_error_bound, dst->scale_exp2 - exponent) +
      ldexpl(src->numeric_error_bound, src->scale_exp2 - exponent) +
      4.0L * LDBL_EPSILON * (fabsl(dst->re) + fabsl(dst->im));
  dst->scale_exp2 = exponent;
  scalar_renormalize(dst);
  return true;
}

bool qsop_qpf_scalar_mul(qsop_qpf_scalar_t *dst, const qsop_qpf_scalar_t *src,
                         qsop_error_t *error) {
  if (dst == NULL || src == NULL || !scalar_compatible(dst, src, error)) {
    return false;
  }
  if (dst->backend == QSOP_QPF_SCALAR_MODULAR) {
    dst->value = qsop_mod_mul_u64(dst->value, src->value, dst->modulus);
    return true;
  }
  if (dst->backend == QSOP_QPF_SCALAR_CYCLOTOMIC) {
    const uint32_t degree = dst->cyclotomic_degree;
    qpf_i128_t product[2U * QSOP_QPF_CYCLOTOMIC_MAX_ORDER] = {0};
    for (uint32_t i = 0; i < degree; i++) {
      for (uint32_t j = 0; j < degree; j++) {
        qpf_i128_t term = 0;
        qpf_i128_t updated = 0;
        if (!i128_mul(dst->coefficients[i], src->coefficients[j], &term, error) ||
            !i128_add(product[i + j], term, &updated, error)) {
          return false;
        }
        product[i + j] = updated;
      }
    }
    if (!cyclotomic_reduce(dst->order, product, degree == 0 ? 0U : 2U * degree - 2U, error)) {
      return false;
    }
    if (src->denominator_exp2 > UINT32_MAX - dst->denominator_exp2) {
      qsop_set_error(error, "exact QPF cyclotomic denominator overflow");
      return false;
    }
    dst->denominator_exp2 += src->denominator_exp2;
    memcpy(dst->coefficients, product, (size_t)degree * sizeof(*dst->coefficients));
    cyclotomic_normalize(dst);
    return true;
  }
  const long double old_re = dst->re;
  const long double old_im = dst->im;
  const long double old_error = dst->numeric_error_bound;
  dst->re = old_re * src->re - old_im * src->im;
  dst->im = old_re * src->im + old_im * src->re;
  dst->numeric_error_bound =
      old_error * (fabsl(src->re) + fabsl(src->im)) +
      src->numeric_error_bound * (fabsl(old_re) + fabsl(old_im)) +
      8.0L * LDBL_EPSILON * (fabsl(dst->re) + fabsl(dst->im));
  if ((src->scale_exp2 > 0 && dst->scale_exp2 > INT32_MAX - src->scale_exp2) ||
      (src->scale_exp2 < 0 && dst->scale_exp2 < INT32_MIN - src->scale_exp2)) {
    qsop_set_error(error, "QPF complex scale exponent overflow");
    return false;
  }
  dst->scale_exp2 += src->scale_exp2;
  scalar_renormalize(dst);
  return true;
}

bool qsop_qpf_scalar_mul_root(qsop_qpf_scalar_t *scalar, uint64_t exponent,
                              qsop_error_t *error) {
  if (scalar == NULL || scalar->order == 0) {
    qsop_set_error(error, "invalid QPF root-of-unity scalar");
    return false;
  }
  const uint32_t reduced = (uint32_t)(exponent % scalar->order);
  qsop_qpf_scalar_t factor = {0};
  if (scalar->backend == QSOP_QPF_SCALAR_MODULAR) {
    qsop_qpf_scalar_modular(&factor, scalar->order, scalar->modulus, scalar->root,
                            qsop_mod_pow_u64(scalar->root, reduced, scalar->modulus));
  } else if (scalar->backend == QSOP_QPF_SCALAR_COMPLEX) {
    qsop_qpf_scalar_complex(&factor, scalar->order, reduced);
  } else if (!qsop_qpf_scalar_cyclotomic(&factor, scalar->order, reduced, error)) {
    return false;
  }
  return qsop_qpf_scalar_mul(scalar, &factor, error);
}

bool qsop_qpf_scalar_negate(qsop_qpf_scalar_t *scalar, qsop_error_t *error) {
  if (scalar == NULL) {
    qsop_set_error(error, "invalid QPF scalar negation");
    return false;
  }
  if (scalar->backend == QSOP_QPF_SCALAR_MODULAR) {
    scalar->value = scalar->value == 0 ? 0 : scalar->modulus - scalar->value;
  } else if (scalar->backend == QSOP_QPF_SCALAR_COMPLEX) {
    scalar->re = -scalar->re;
    scalar->im = -scalar->im;
  } else {
    for (uint32_t i = 0; i < scalar->cyclotomic_degree; i++) {
      qpf_i128_t value = 0;
      if (__builtin_sub_overflow((qpf_i128_t)0, scalar->coefficients[i], &value)) {
        qsop_set_error(error, "exact QPF cyclotomic coefficient overflow");
        return false;
      }
      scalar->coefficients[i] = value;
    }
  }
  return true;
}

bool qsop_qpf_scalar_divide_pow2(qsop_qpf_scalar_t *scalar, uint32_t exponent,
                                 qsop_error_t *error) {
  if (scalar == NULL || scalar->backend != QSOP_QPF_SCALAR_CYCLOTOMIC ||
      exponent > UINT32_MAX - scalar->denominator_exp2) {
    qsop_set_error(error, "invalid exact QPF power-of-two division");
    return false;
  }
  scalar->denominator_exp2 += exponent;
  cyclotomic_normalize(scalar);
  return true;
}

bool qsop_qpf_scalar_divide_u64_exact(qsop_qpf_scalar_t *scalar, uint64_t divisor,
                                      qsop_error_t *error) {
  if (scalar == NULL || scalar->backend != QSOP_QPF_SCALAR_CYCLOTOMIC || divisor == 0U) {
    qsop_set_error(error, "invalid exact QPF integer division");
    return false;
  }
  uint32_t twos = 0;
  while ((divisor & 1U) == 0U) {
    divisor >>= 1U;
    twos++;
  }
  if (twos > UINT32_MAX - scalar->denominator_exp2) {
    qsop_set_error(error, "exact QPF cyclotomic denominator overflow");
    return false;
  }
  for (uint32_t i = 0; i < scalar->cyclotomic_degree; i++) {
    if (scalar->coefficients[i] % (qpf_i128_t)divisor != 0) {
      qsop_set_error(error, "exact QPF cyclotomic division is not integral");
      return false;
    }
    scalar->coefficients[i] /= (qpf_i128_t)divisor;
  }
  scalar->denominator_exp2 += twos;
  cyclotomic_normalize(scalar);
  return true;
}

bool qsop_qpf_scalar_is_zero(const qsop_qpf_scalar_t *scalar) {
  if (scalar == NULL) {
    return true;
  }
  if (scalar->backend == QSOP_QPF_SCALAR_MODULAR) {
    return scalar->value == 0;
  }
  if (scalar->backend == QSOP_QPF_SCALAR_COMPLEX) {
    return scalar->re == 0.0L && scalar->im == 0.0L;
  }
  for (uint32_t i = 0; i < scalar->cyclotomic_degree; i++) {
    if (scalar->coefficients[i] != 0) {
      return false;
    }
  }
  return true;
}

bool qsop_qpf_init(qsop_qpf_t *qpf, uint32_t m, const qsop_qpf_scalar_t *eps,
                   qsop_error_t *error) {
  if (qpf == NULL || eps == NULL || eps->order == 0 || eps->order % 4U != 0) {
    qsop_set_error(error, "invalid QPF initialization argument");
    return false;
  }
  *qpf = (qsop_qpf_t){.m = m, .words = qpf_words(m), .eps = *eps};
  const size_t ell_len = m == 0 ? 1U : m;
  const size_t quad_len = m == 0 || qpf->words == 0 ? 1U : (size_t)m * qpf->words;
  qpf->ell = calloc(ell_len, sizeof(*qpf->ell));
  qpf->quad = calloc(quad_len, sizeof(*qpf->quad));
  if (qpf->ell == NULL || qpf->quad == NULL) {
    qsop_qpf_free(qpf);
    qsop_set_error(error, "out of memory while allocating a QPF");
    return false;
  }
  return true;
}

void qsop_qpf_free(qsop_qpf_t *qpf) {
  if (qpf == NULL) {
    return;
  }
  free(qpf->ell);
  free(qpf->quad);
  free(qpf->aff_rows);
  free(qpf->aff_rhs);
  *qpf = (qsop_qpf_t){0};
}

bool qsop_qpf_clone(const qsop_qpf_t *source, qsop_qpf_t *out, qsop_error_t *error) {
  if (source == NULL || out == NULL || !qsop_qpf_init(out, source->m, &source->eps, error)) {
    return false;
  }
  memcpy(out->ell, source->ell, (size_t)source->m * sizeof(*out->ell));
  memcpy(out->quad, source->quad, (size_t)source->m * source->words * sizeof(*out->quad));
  for (uint32_t row = 0; row < source->aff_len; row++) {
    if (!qsop_qpf_add_constraint(out, source->aff_rows + (size_t)row * source->words,
                                 source->aff_rhs[row], error)) {
      qsop_qpf_free(out);
      return false;
    }
  }
  return true;
}

void qsop_qpf_set_linear(qsop_qpf_t *qpf, uint32_t variable, uint8_t coefficient) {
  if (qpf != NULL && variable < qpf->m) {
    qpf->ell[variable] = coefficient & 3U;
  }
}

void qsop_qpf_toggle_quadratic(qsop_qpf_t *qpf, uint32_t left, uint32_t right) {
  if (qpf == NULL || left == right || left >= qpf->m || right >= qpf->m) {
    return;
  }
  if (left > right) {
    const uint32_t swap = left;
    left = right;
    right = swap;
  }
  bit_toggle(qpf->quad + (size_t)left * qpf->words, right);
}

bool qsop_qpf_add_constraint(qsop_qpf_t *qpf, const uint64_t *row, uint64_t rhs,
                             qsop_error_t *error) {
  if (qpf == NULL || (qpf->words != 0 && row == NULL)) {
    qsop_set_error(error, "invalid QPF affine constraint");
    return false;
  }
  if (qpf->aff_len == qpf->aff_cap) {
    uint32_t cap = qpf->aff_cap == 0 ? 4U : qpf->aff_cap * 2U;
    if (cap < qpf->aff_cap) {
      qsop_set_error(error, "too many QPF affine constraints");
      return false;
    }
    uint64_t *rows = realloc(qpf->aff_rows, (size_t)cap * qpf->words * sizeof(*rows));
    uint64_t *values = realloc(qpf->aff_rhs, (size_t)cap * sizeof(*values));
    if (rows == NULL || values == NULL) {
      if (rows != NULL) {
        qpf->aff_rows = rows;
      }
      if (values != NULL) {
        qpf->aff_rhs = values;
      }
      qsop_set_error(error, "out of memory while growing QPF constraints");
      return false;
    }
    qpf->aff_rows = rows;
    qpf->aff_rhs = values;
    qpf->aff_cap = cap;
  }
  if (qpf->words != 0) {
    memcpy(qpf->aff_rows + (size_t)qpf->aff_len * qpf->words, row,
           qpf->words * sizeof(*row));
  }
  qpf->aff_rhs[qpf->aff_len++] = rhs & 1U;
  return true;
}

bool qsop_qpf_eval(const qsop_qpf_t *qpf, const uint64_t *assignment,
                   qsop_qpf_scalar_t *out, qsop_error_t *error) {
  if (qpf == NULL || out == NULL || (qpf->words != 0 && assignment == NULL)) {
    qsop_set_error(error, "invalid QPF evaluation argument");
    return false;
  }
  *out = qpf->eps;
  for (uint32_t row = 0; row < qpf->aff_len; row++) {
    if (parity_and(qpf->aff_rows + (size_t)row * qpf->words, assignment, qpf->words) !=
        (qpf->aff_rhs[row] & 1U)) {
      if (out->backend == QSOP_QPF_SCALAR_MODULAR) {
        out->value = 0;
      } else {
        out->re = 0.0L;
        out->im = 0.0L;
        out->numeric_error_bound = 0.0L;
        out->scale_exp2 = 0;
      }
      return true;
    }
  }
  uint32_t phase = 0;
  for (uint32_t i = 0; i < qpf->m; i++) {
    if (!bit_get(assignment, i)) {
      continue;
    }
    phase = (phase + qpf->ell[i]) & 3U;
    const uint64_t *row = qpf->quad + (size_t)i * qpf->words;
    for (uint32_t j = i + 1U; j < qpf->m; j++) {
      if (bit_get(row, j) && bit_get(assignment, j)) {
        phase ^= 2U;
      }
    }
  }
  return qsop_qpf_scalar_mul_root(out, (uint64_t)phase * (qpf->eps.order / 4U), error);
}

static void add_affine_phase(uint8_t *ell, uint64_t *quad, size_t words, uint32_t m,
                             uint8_t coefficient, bool constant, const uint64_t *row,
                             uint32_t *constant_phase) {
  coefficient &= 3U;
  if (coefficient == 0) {
    return;
  }
  if (constant) {
    *constant_phase = (*constant_phase + coefficient) & 3U;
  }
  const uint8_t linear = constant ? (uint8_t)((4U - coefficient) & 3U) : coefficient;
  for (uint32_t i = 0; i < m; i++) {
    if (!bit_get(row, i)) {
      continue;
    }
    ell[i] = (uint8_t)((ell[i] + linear) & 3U);
    if ((coefficient & 1U) == 0) {
      continue;
    }
    for (uint32_t j = i + 1U; j < m; j++) {
      if (bit_get(row, j)) {
        bit_toggle(quad + (size_t)i * words, j);
      }
    }
  }
}

static void add_product_phase(uint8_t *ell, uint64_t *quad, size_t words, uint32_t m,
                              bool left_constant, const uint64_t *left, bool right_constant,
                              const uint64_t *right, uint32_t *constant_phase) {
  if (left_constant && right_constant) {
    *constant_phase ^= 2U;
  }
  for (uint32_t i = 0; i < m; i++) {
    bool coefficient = (left_constant && bit_get(right, i)) ^
                       (right_constant && bit_get(left, i)) ^
                       (bit_get(left, i) && bit_get(right, i));
    if (coefficient) {
      ell[i] ^= 2U;
    }
    for (uint32_t j = i + 1U; j < m; j++) {
      coefficient = (bit_get(left, i) && bit_get(right, j)) ^
                    (bit_get(left, j) && bit_get(right, i));
      if (coefficient) {
        bit_toggle(quad + (size_t)i * words, j);
      }
    }
  }
}

static bool qpf_set_zero(qsop_qpf_t *qpf) {
  if (qpf->eps.backend == QSOP_QPF_SCALAR_MODULAR) {
    qpf->eps.value = 0;
  } else if (qpf->eps.backend == QSOP_QPF_SCALAR_COMPLEX) {
    qpf->eps.re = 0.0L;
    qpf->eps.im = 0.0L;
    qpf->eps.numeric_error_bound = 0.0L;
    qpf->eps.scale_exp2 = 0;
  } else {
    memset(qpf->eps.coefficients, 0, sizeof(qpf->eps.coefficients));
    qpf->eps.denominator_exp2 = 0;
  }
  return true;
}

static bool qpf_parameterize(qsop_qpf_t *qpf, qsop_error_t *error) {
  if (qpf->aff_len == 0 || qsop_qpf_scalar_is_zero(&qpf->eps)) {
    return true;
  }
  const uint32_t old_m = qpf->m;
  const size_t old_words = qpf->words;
  uint64_t *matrix = calloc((size_t)qpf->aff_len * old_words, sizeof(*matrix));
  uint64_t *rhs = calloc(qpf->aff_len, sizeof(*rhs));
  int32_t *pivot_row = malloc((size_t)(old_m == 0 ? 1U : old_m) * sizeof(*pivot_row));
  if (matrix == NULL || rhs == NULL || pivot_row == NULL) {
    free(matrix);
    free(rhs);
    free(pivot_row);
    qsop_set_error(error, "out of memory while reducing QPF constraints");
    return false;
  }
  memcpy(matrix, qpf->aff_rows, (size_t)qpf->aff_len * old_words * sizeof(*matrix));
  memcpy(rhs, qpf->aff_rhs, (size_t)qpf->aff_len * sizeof(*rhs));
  for (uint32_t column = 0; column < old_m; column++) {
    pivot_row[column] = -1;
  }

  uint32_t rank = 0;
  for (uint32_t column = 0; column < old_m && rank < qpf->aff_len; column++) {
    uint32_t pivot = rank;
    while (pivot < qpf->aff_len &&
           !bit_get(matrix + (size_t)pivot * old_words, column)) {
      pivot++;
    }
    if (pivot == qpf->aff_len) {
      continue;
    }
    if (pivot != rank) {
      for (size_t word = 0; word < old_words; word++) {
        const size_t a = (size_t)rank * old_words + word;
        const size_t b = (size_t)pivot * old_words + word;
        const uint64_t swap = matrix[a];
        matrix[a] = matrix[b];
        matrix[b] = swap;
      }
      const uint64_t swap_rhs = rhs[rank];
      rhs[rank] = rhs[pivot];
      rhs[pivot] = swap_rhs;
    }
    for (uint32_t row = 0; row < qpf->aff_len; row++) {
      if (row == rank || !bit_get(matrix + (size_t)row * old_words, column)) {
        continue;
      }
      for (size_t word = 0; word < old_words; word++) {
        matrix[(size_t)row * old_words + word] ^=
            matrix[(size_t)rank * old_words + word];
      }
      rhs[row] ^= rhs[rank];
    }
    pivot_row[column] = (int32_t)rank;
    rank++;
  }
  for (uint32_t row = rank; row < qpf->aff_len; row++) {
    bool any = false;
    for (size_t word = 0; word < old_words; word++) {
      any |= matrix[(size_t)row * old_words + word] != 0;
    }
    if (!any && (rhs[row] & 1U) != 0) {
      free(matrix);
      free(rhs);
      free(pivot_row);
      free(qpf->aff_rows);
      free(qpf->aff_rhs);
      qpf->aff_rows = NULL;
      qpf->aff_rhs = NULL;
      qpf->aff_len = 0;
      qpf->aff_cap = 0;
      return qpf_set_zero(qpf);
    }
  }

  const uint32_t free_m = old_m - rank;
  const size_t free_words = qpf_words(free_m);
  uint32_t *free_columns = malloc((size_t)(free_m == 0 ? 1U : free_m) * sizeof(*free_columns));
  uint64_t *preimages = calloc((size_t)(old_m == 0 ? 1U : old_m) *
                                   (free_words == 0 ? 1U : free_words),
                               sizeof(*preimages));
  bool *constants = calloc(old_m == 0 ? 1U : old_m, sizeof(*constants));
  if (free_columns == NULL || preimages == NULL || constants == NULL) {
    free(matrix);
    free(rhs);
    free(pivot_row);
    free(free_columns);
    free(preimages);
    free(constants);
    qsop_set_error(error, "out of memory while parameterizing a QPF");
    return false;
  }
  uint32_t free_index = 0;
  for (uint32_t column = 0; column < old_m; column++) {
    if (pivot_row[column] < 0) {
      free_columns[free_index++] = column;
    }
  }
  free_index = 0;
  for (uint32_t column = 0; column < old_m; column++) {
    if (pivot_row[column] < 0) {
      if (free_words != 0) {
        bit_set(preimages + (size_t)column * free_words, free_index);
      }
      free_index++;
    } else {
      const uint32_t row = (uint32_t)pivot_row[column];
      constants[column] = (rhs[row] & 1U) != 0;
      for (uint32_t j = 0; j < free_m; j++) {
        if (bit_get(matrix + (size_t)row * old_words, free_columns[j])) {
          bit_set(preimages + (size_t)column * free_words, j);
        }
      }
    }
  }

  uint8_t *new_ell = calloc(free_m == 0 ? 1U : free_m, sizeof(*new_ell));
  uint64_t *new_quad = calloc(free_m == 0 || free_words == 0
                                  ? 1U
                                  : (size_t)free_m * free_words,
                              sizeof(*new_quad));
  if (new_ell == NULL || new_quad == NULL) {
    free(matrix);
    free(rhs);
    free(pivot_row);
    free(free_columns);
    free(preimages);
    free(constants);
    free(new_ell);
    free(new_quad);
    qsop_set_error(error, "out of memory while substituting QPF coordinates");
    return false;
  }
  uint32_t constant_phase = 0;
  for (uint32_t i = 0; i < old_m; i++) {
    add_affine_phase(new_ell, new_quad, free_words, free_m, qpf->ell[i], constants[i],
                     preimages + (size_t)i * free_words, &constant_phase);
    const uint64_t *quad_row = qpf->quad + (size_t)i * old_words;
    for (uint32_t j = i + 1U; j < old_m; j++) {
      if (bit_get(quad_row, j)) {
        add_product_phase(new_ell, new_quad, free_words, free_m, constants[i],
                          preimages + (size_t)i * free_words, constants[j],
                          preimages + (size_t)j * free_words, &constant_phase);
      }
    }
  }
  const bool phase_ok = qsop_qpf_scalar_mul_root(
      &qpf->eps, (uint64_t)constant_phase * (qpf->eps.order / 4U), error);
  if (phase_ok) {
    free(qpf->ell);
    free(qpf->quad);
    free(qpf->aff_rows);
    free(qpf->aff_rhs);
    qpf->m = free_m;
    qpf->words = free_words;
    qpf->ell = new_ell;
    qpf->quad = new_quad;
    qpf->aff_rows = NULL;
    qpf->aff_rhs = NULL;
    qpf->aff_len = 0;
    qpf->aff_cap = 0;
  } else {
    free(new_ell);
    free(new_quad);
  }
  free(matrix);
  free(rhs);
  free(pivot_row);
  free(free_columns);
  free(preimages);
  free(constants);
  return phase_ok;
}

static bool qpf_shrink_last(qsop_qpf_t *qpf, qsop_error_t *error) {
  const uint32_t new_m = qpf->m - 1U;
  const size_t new_words = qpf_words(new_m);
  uint8_t *ell = calloc(new_m == 0 ? 1U : new_m, sizeof(*ell));
  uint64_t *quad = calloc(new_m == 0 || new_words == 0 ? 1U : (size_t)new_m * new_words,
                          sizeof(*quad));
  if (ell == NULL || quad == NULL) {
    free(ell);
    free(quad);
    qsop_set_error(error, "out of memory while eliminating a QPF variable");
    return false;
  }
  memcpy(ell, qpf->ell, (size_t)new_m * sizeof(*ell));
  for (uint32_t i = 0; i < new_m; i++) {
    for (uint32_t j = i + 1U; j < new_m; j++) {
      if (bit_get(qpf->quad + (size_t)i * qpf->words, j)) {
        bit_set(quad + (size_t)i * new_words, j);
      }
    }
  }
  free(qpf->ell);
  free(qpf->quad);
  qpf->m = new_m;
  qpf->words = new_words;
  qpf->ell = ell;
  qpf->quad = quad;
  return true;
}

static bool scalar_mul_elimination_factor(qsop_qpf_scalar_t *scalar, uint8_t coefficient,
                                          qsop_error_t *error) {
  qsop_qpf_scalar_t factor = {0};
  if ((coefficient & 1U) == 0) {
    if (scalar->backend == QSOP_QPF_SCALAR_MODULAR) {
      qsop_qpf_scalar_modular(&factor, scalar->order, scalar->modulus, scalar->root, 2U);
    } else if (scalar->backend == QSOP_QPF_SCALAR_COMPLEX) {
      factor = *scalar;
      factor.re = 1.0L;
      factor.im = 0.0L;
      factor.numeric_error_bound = 0.0L;
      factor.scale_exp2 = 1;
    } else if (!qsop_qpf_scalar_cyclotomic(&factor, scalar->order, 0U, error)) {
      return false;
    } else {
      factor.coefficients[0] = 2;
    }
  } else {
    factor = *scalar;
    if (factor.backend == QSOP_QPF_SCALAR_MODULAR) {
      const uint64_t i_value = qsop_mod_pow_u64(factor.root, factor.order / 4U, factor.modulus);
      factor.value = coefficient == 1U ? qsop_mod_add_u64(1U, i_value, factor.modulus)
                                       : qsop_mod_add_u64(1U, factor.modulus - i_value,
                                                          factor.modulus);
    } else if (factor.backend == QSOP_QPF_SCALAR_COMPLEX) {
      factor.re = 1.0L;
      factor.im = coefficient == 1U ? 1.0L : -1.0L;
      factor.numeric_error_bound = 0.0L;
      factor.scale_exp2 = 0;
    } else {
      qsop_qpf_scalar_t i_factor = {0};
      if (!qsop_qpf_scalar_cyclotomic(&factor, scalar->order, 0U, error) ||
          !qsop_qpf_scalar_cyclotomic(&i_factor, scalar->order, scalar->order / 4U, error)) {
        return false;
      }
      if (coefficient == 3U && !qsop_qpf_scalar_negate(&i_factor, error)) {
        return false;
      }
      if (!qsop_qpf_scalar_add(&factor, &i_factor, error)) {
        return false;
      }
    }
  }
  return qsop_qpf_scalar_mul(scalar, &factor, error);
}

bool qsop_qpf_total_sum(const qsop_qpf_t *qpf, qsop_qpf_scalar_t *out,
                        qsop_error_t *error) {
  if (qpf == NULL || out == NULL) {
    qsop_set_error(error, "invalid QPF total-sum argument");
    return false;
  }
  qsop_qpf_t work = {0};
  if (!qsop_qpf_clone(qpf, &work, error)) {
    return false;
  }
  while (work.m != 0 && !qsop_qpf_scalar_is_zero(&work.eps)) {
    if (!qpf_parameterize(&work, error)) {
      qsop_qpf_free(&work);
      return false;
    }
    if (work.m == 0 || qsop_qpf_scalar_is_zero(&work.eps)) {
      break;
    }
    const uint32_t last = work.m - 1U;
    const uint8_t coefficient = work.ell[last] & 3U;
    const size_t lambda_words = qpf_words(last);
    uint64_t *lambda = calloc(lambda_words == 0 ? 1U : lambda_words, sizeof(*lambda));
    if (lambda == NULL) {
      qsop_qpf_free(&work);
      qsop_set_error(error, "out of memory while summing a QPF variable");
      return false;
    }
    for (uint32_t i = 0; i < last; i++) {
      if (bit_get(work.quad + (size_t)i * work.words, last)) {
        bit_set(lambda, i);
      }
    }
    if (!scalar_mul_elimination_factor(&work.eps, coefficient, error) ||
        !qpf_shrink_last(&work, error)) {
      free(lambda);
      qsop_qpf_free(&work);
      return false;
    }
    if ((coefficient & 1U) != 0) {
      const uint8_t mu = coefficient == 1U ? 3U : 1U;
      uint32_t constant_phase = 0;
      add_affine_phase(work.ell, work.quad, work.words, work.m, mu, false, lambda,
                       &constant_phase);
    } else {
      bool nonzero = false;
      for (size_t word = 0; word < lambda_words; word++) {
        nonzero |= lambda[word] != 0;
      }
      const uint64_t rhs = coefficient == 2U ? 1U : 0U;
      if (!nonzero && rhs != 0) {
        qpf_set_zero(&work);
      } else if (nonzero && !qsop_qpf_add_constraint(&work, lambda, rhs, error)) {
        free(lambda);
        qsop_qpf_free(&work);
        return false;
      }
    }
    free(lambda);
  }
  *out = work.eps;
  qsop_qpf_free(&work);
  return true;
}
