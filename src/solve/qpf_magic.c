#include "qpf_magic.h"

#include "dlx4sop/qsop_stats.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/residue.h"
#include "../core/qsop_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define QPF_DEFAULT_MAX_TERMS UINT64_C(4096)

static uint64_t gcd_u64(uint64_t left, uint64_t right) {
  while (right != 0) {
    const uint64_t remainder = left % right;
    left = right;
    right = remainder;
  }
  return left;
}

bool qsop_qpf_phase_order_supported(uint64_t r, uint32_t max_order) {
  if (r == 0U || r > UINT32_MAX) {
    return false;
  }
  const uint64_t multiplier = 8U / gcd_u64(r, 8U);
  return r <= UINT32_MAX / multiplier && r * multiplier <= max_order;
}

static bool qpf_phase_order(uint64_t r, uint32_t *out, qsop_error_t *error) {
  if (r == 0 || r > UINT32_MAX) {
    qsop_set_error(error, "QPF path requires a modulus in [1, 2^32-1]");
    return false;
  }
  const uint64_t gcd = gcd_u64(r, 8U);
  if (r > UINT32_MAX / (8U / gcd)) {
    qsop_set_error(error, "QPF phase order lcm(r,8) exceeds 32 bits");
    return false;
  }
  *out = (uint32_t)(r * (8U / gcd));
  return true;
}

uint64_t qsop_qpf_naive_term_bound(uint32_t magic_vertices) {
  return magic_vertices >= 64U ? UINT64_MAX : UINT64_C(1) << magic_vertices;
}

static uint64_t qpf_saturating_pow(uint64_t base, uint32_t exponent) {
  uint64_t result = 1U;
  for (uint32_t i = 0; i < exponent; i++) {
    if (base != 0U && result > UINT64_MAX / base) {
      return UINT64_MAX;
    }
    result *= base;
  }
  return result;
}

uint64_t qsop_qpf_stabilizer_term_bound(const qsop_instance_t *qsop, uint64_t target_mode) {
  const uint32_t magic = qsop_magic_vertex_count(qsop, target_mode);
  if (qsop->r != 8U || magic < 6U) {
    return qsop_qpf_naive_term_bound(magic);
  }
  const uint64_t blocks = qpf_saturating_pow(6U, magic / 6U);
  const uint64_t remainder = qsop_qpf_naive_term_bound(magic % 6U);
  return blocks == UINT64_MAX || remainder > UINT64_MAX / blocks ? UINT64_MAX
                                                                 : blocks * remainder;
}

static qsop_qpf_scalar_t scalar_like(const qsop_qpf_scalar_t *unit) {
  qsop_qpf_scalar_t out = *unit;
  if (out.backend == QSOP_QPF_SCALAR_MODULAR) {
    out.value = 1U;
  } else {
    out.re = 1.0L;
    out.im = 0.0L;
    out.numeric_error_bound = 0.0L;
    out.scale_exp2 = 0;
  }
  return out;
}

static bool scalar_magic_coefficients(const qsop_qpf_scalar_t *unit, uint64_t root_exponent,
                                      qsop_qpf_scalar_t *alpha, qsop_qpf_scalar_t *beta,
                                      qsop_error_t *error) {
  *alpha = scalar_like(unit);
  *beta = scalar_like(unit);
  if (unit->backend == QSOP_QPF_SCALAR_MODULAR) {
    const uint64_t w = qsop_mod_pow_u64(unit->root, root_exponent % unit->order, unit->modulus);
    const uint64_t i_value =
        qsop_mod_pow_u64(unit->root, unit->order / 4U, unit->modulus);
    const uint64_t denominator = i_value == 0 ? 0 : i_value - 1U;
    if (denominator == 0) {
      qsop_set_error(error, "QPF modular scalar has no i-1 inverse");
      return false;
    }
    const uint64_t inverse = qsop_mod_pow_u64(denominator, unit->modulus - 2U, unit->modulus);
    const uint64_t numerator = w == 0 ? 0 : w - 1U;
    beta->value = qsop_mod_mul_u64(numerator, inverse, unit->modulus);
    alpha->value = beta->value == 0 ? 1U : unit->modulus + 1U - beta->value;
    if (alpha->value >= unit->modulus) {
      alpha->value -= unit->modulus;
    }
    return true;
  }
  if (unit->backend == QSOP_QPF_SCALAR_CYCLOTOMIC) {
    qsop_qpf_scalar_t one = {0};
    qsop_qpf_scalar_t w = {0};
    qsop_qpf_scalar_t minus_one_minus_i = {0};
    qsop_qpf_scalar_t minus_i = {0};
    if (!qsop_qpf_scalar_cyclotomic(&one, unit->order, 0U, error) ||
        !qsop_qpf_scalar_cyclotomic(&w, unit->order,
                                    (uint32_t)(root_exponent % unit->order), error) ||
        !qsop_qpf_scalar_cyclotomic(&minus_one_minus_i, unit->order, 0U, error) ||
        !qsop_qpf_scalar_cyclotomic(&minus_i, unit->order, unit->order / 4U, error) ||
        !qsop_qpf_scalar_negate(&one, error) || !qsop_qpf_scalar_add(&w, &one, error) ||
        !qsop_qpf_scalar_negate(&minus_one_minus_i, error) ||
        !qsop_qpf_scalar_negate(&minus_i, error) ||
        !qsop_qpf_scalar_add(&minus_one_minus_i, &minus_i, error) ||
        !qsop_qpf_scalar_mul(&w, &minus_one_minus_i, error) ||
        !qsop_qpf_scalar_divide_pow2(&w, 1U, error)) {
      return false;
    }
    *beta = w;
    if (!qsop_qpf_scalar_cyclotomic(alpha, unit->order, 0U, error)) {
      return false;
    }
    qsop_qpf_scalar_t minus_beta = *beta;
    return qsop_qpf_scalar_negate(&minus_beta, error) &&
           qsop_qpf_scalar_add(alpha, &minus_beta, error);
  }

  const long double angle = 2.0L * acosl(-1.0L) *
                            (long double)(root_exponent % unit->order) /
                            (long double)unit->order;
  const long double w_re = cosl(angle);
  const long double w_im = sinl(angle);
  /* (w-1)/(i-1) = (w-1)(-1-i)/2. */
  beta->re = ((w_re - 1.0L) * -1.0L + w_im) / 2.0L;
  beta->im = (-(w_re - 1.0L) - w_im) / 2.0L;
  beta->numeric_error_bound = 12.0L * LDBL_EPSILON;
  alpha->re = 1.0L - beta->re;
  alpha->im = -beta->im;
  alpha->numeric_error_bound = beta->numeric_error_bound + 2.0L * LDBL_EPSILON;
  return true;
}

static qsop_qpf_scalar_t scalar_integer_like(const qsop_qpf_scalar_t *unit, uint64_t value) {
  qsop_qpf_scalar_t out = scalar_like(unit);
  if (out.backend == QSOP_QPF_SCALAR_MODULAR) {
    out.value = value % out.modulus;
  } else if (out.backend == QSOP_QPF_SCALAR_COMPLEX) {
    out.re = (long double)value;
    out.im = 0.0L;
    out.numeric_error_bound = 0.0L;
    out.scale_exp2 = 0;
  } else {
    memset(out.coefficients, 0, sizeof(out.coefficients));
    out.coefficients[0] = (qsop_qpf_i128_t)value;
    out.denominator_exp2 = 0U;
  }
  return out;
}

static bool scalar_inverse_sqrt_two(const qsop_qpf_scalar_t *unit, qsop_qpf_scalar_t *out,
                                    qsop_error_t *error) {
  *out = scalar_like(unit);
  qsop_qpf_scalar_t conjugate = scalar_like(unit);
  if (!qsop_qpf_scalar_mul_root(out, 1U, error) ||
      !qsop_qpf_scalar_mul_root(&conjugate, 7U, error) ||
      !qsop_qpf_scalar_add(out, &conjugate, error)) {
    return false;
  }
  if (out->backend == QSOP_QPF_SCALAR_MODULAR) {
    const uint64_t inverse_two = qsop_mod_pow_u64(2U, out->modulus - 2U, out->modulus);
    qsop_qpf_scalar_t half = scalar_integer_like(unit, inverse_two);
    return qsop_qpf_scalar_mul(out, &half, error);
  }
  if (out->backend == QSOP_QPF_SCALAR_COMPLEX) {
    out->re /= 2.0L;
    out->im /= 2.0L;
    out->numeric_error_bound += 2.0L * LDBL_EPSILON;
    return true;
  }
  return qsop_qpf_scalar_divide_pow2(out, 1U, error);
}

static bool qpf_t6_apply_a(const qsop_qpf_t *source, qsop_qpf_t *out, qsop_error_t *error) {
  if (!qsop_qpf_clone(source, out, error)) {
    return false;
  }
  const uint8_t old_linear = out->ell[0] & 3U;
  if (!qsop_qpf_scalar_mul_root(&out->eps,
                                ((uint64_t)old_linear * (out->eps.order / 4U) +
                                 out->eps.order - 1U) %
                                    out->eps.order,
                                error)) {
    qsop_qpf_free(out);
    return false;
  }
  out->ell[0] = (uint8_t)((5U - old_linear) & 3U);
  for (uint32_t j = 1; j < out->m; j++) {
    if (((out->quad[j / 64U] >> (j % 64U)) & 1U) != 0U) {
      out->ell[j] = (uint8_t)((out->ell[j] + 2U) & 3U);
    }
  }
  for (uint32_t row = 0; row < out->aff_len; row++) {
    if (((out->aff_rows[(size_t)row * out->words] & 1U) != 0U)) {
      out->aff_rhs[row] ^= 1U;
    }
  }
  return true;
}

/* Qassim--Pashayan--Gosset, Eq. (5), followed by (I+A)/sqrt(2): an exact six-term
 * decomposition of the unnormalised function z -> zeta_8^{|z|}. */
static bool qpf_t6_block(const qsop_qpf_scalar_t *unit, qsop_qpf_t block[6],
                         qsop_error_t *error) {
  uint64_t row[1] = {0};
  qsop_qpf_scalar_t c_eps = scalar_integer_like(unit, 2U);
  if (!qsop_qpf_init(&block[0], 6U, &c_eps, error)) {
    return false;
  }
  qsop_qpf_set_linear(&block[0], 0U, 3U);
  for (uint32_t j = 1; j < 6U; j++) {
    row[0] = UINT64_C(1) | (UINT64_C(1) << j);
    if (!qsop_qpf_add_constraint(&block[0], row, 0U, error)) {
      goto fail;
    }
  }

  qsop_qpf_scalar_t inverse_sqrt_two = {0};
  if (!scalar_inverse_sqrt_two(unit, &inverse_sqrt_two, error)) {
    goto fail;
  }
  qsop_qpf_scalar_t e_eps = inverse_sqrt_two;
  qsop_qpf_scalar_t k_eps = inverse_sqrt_two;
  if (!qsop_qpf_scalar_mul_root(&e_eps, 3U, error) ||
      !qsop_qpf_scalar_mul_root(&k_eps, 5U, error) ||
      !qsop_qpf_init(&block[1], 6U, &e_eps, error) ||
      !qsop_qpf_init(&block[2], 6U, &k_eps, error)) {
    goto fail;
  }
  row[0] = UINT64_C(0x3f);
  if (!qsop_qpf_add_constraint(&block[1], row, 0U, error) ||
      !qsop_qpf_add_constraint(&block[2], row, 0U, error)) {
    goto fail;
  }
  for (uint32_t i = 0; i < 6U; i++) {
    for (uint32_t j = i + 1U; j < 6U; j++) {
      qsop_qpf_toggle_quadratic(&block[2], i, j);
    }
  }
  if (!qpf_t6_apply_a(&block[0], &block[3], error) ||
      !qpf_t6_apply_a(&block[1], &block[4], error) ||
      !qpf_t6_apply_a(&block[2], &block[5], error)) {
    goto fail;
  }
  return true;

fail:
  for (uint32_t i = 0; i < 6U; i++) {
    qsop_qpf_free(&block[i]);
  }
  return false;
}

void qsop_qpf_term_list_free(qsop_qpf_term_list_t *list) {
  if (list == NULL) {
    return;
  }
  for (uint64_t i = 0; i < list->len; i++) {
    qsop_qpf_free(&list->terms[i]);
  }
  free(list->terms);
  *list = (qsop_qpf_term_list_t){0};
}

static bool qpf_magic_decompose_t6(const qsop_instance_t *qsop, uint64_t target_mode,
                                   uint64_t max_terms, const qsop_qpf_scalar_t *unit,
                                   qsop_qpf_term_list_t *out, qsop_error_t *error) {
  const uint32_t magic_count = qsop_magic_vertex_count(qsop, target_mode);
  const uint32_t block_count = magic_count / 6U;
  const uint32_t remainder_count = magic_count % 6U;
  const uint64_t term_count = qsop_qpf_stabilizer_term_bound(qsop, target_mode);
  const uint64_t budget = max_terms == 0U ? QPF_DEFAULT_MAX_TERMS : max_terms;
  if (term_count == UINT64_MAX || term_count > budget ||
      term_count > SIZE_MAX / sizeof(*out->terms)) {
    qsop_set_error(error, "QPF stabilizer decomposition needs %" PRIu64
                          " terms (budget=%" PRIu64 ")",
                   term_count, budget);
    return false;
  }
  uint32_t *magic_vars = malloc((size_t)magic_count * sizeof(*magic_vars));
  qsop_qpf_scalar_t *alpha = calloc(remainder_count == 0 ? 1U : remainder_count, sizeof(*alpha));
  qsop_qpf_scalar_t *beta = calloc(remainder_count == 0 ? 1U : remainder_count, sizeof(*beta));
  qsop_qpf_t block[6] = {0};
  out->terms = calloc((size_t)term_count, sizeof(*out->terms));
  if (magic_vars == NULL || alpha == NULL || beta == NULL || out->terms == NULL ||
      !qpf_t6_block(unit, block, error)) {
    free(magic_vars);
    free(alpha);
    free(beta);
    qsop_qpf_term_list_free(out);
    return false;
  }
  uint32_t next_magic = 0;
  const uint64_t order_per_r = unit->order / qsop->r;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    const uint64_t phase =
        (uint64_t)(((qsop_qpf_u128_t)(target_mode % qsop->r) * qsop->unary[v]) % qsop->r);
    if (((qsop_qpf_u128_t)4U * phase) % qsop->r != 0U) {
      magic_vars[next_magic++] = v;
    }
  }
  for (uint32_t j = 0; j < remainder_count; j++) {
    const uint32_t v = magic_vars[6U * block_count + j];
    const uint64_t phase =
        (uint64_t)(((qsop_qpf_u128_t)(target_mode % qsop->r) * qsop->unary[v]) % qsop->r);
    if (!scalar_magic_coefficients(unit, phase * order_per_r, &alpha[j], &beta[j], error)) {
      goto fail;
    }
  }

  out->len = term_count;
  for (uint64_t index = 0; index < term_count; index++) {
    uint64_t selector = index;
    qsop_qpf_scalar_t eps = scalar_like(unit);
    const uint64_t constant_phase =
        (uint64_t)(((qsop_qpf_u128_t)(target_mode % qsop->r) * qsop->constant) % qsop->r);
    if (!qsop_qpf_scalar_mul_root(&eps, constant_phase * order_per_r, error) ||
        !qsop_qpf_init(&out->terms[index], qsop->nvars, &eps, error)) {
      goto fail;
    }
    uint64_t *constraint = calloc(out->terms[index].words == 0 ? 1U : out->terms[index].words,
                                  sizeof(*constraint));
    if (constraint == NULL) {
      qsop_set_error(error, "out of memory while embedding a T6 QPF block");
      goto fail;
    }
    for (uint32_t group = 0; group < block_count; group++) {
      const uint32_t selected = (uint32_t)(selector % 6U);
      selector /= 6U;
      if (!qsop_qpf_scalar_mul(&out->terms[index].eps, &block[selected].eps, error)) {
        free(constraint);
        goto fail;
      }
      for (uint32_t local = 0; local < 6U; local++) {
        const uint32_t global = magic_vars[6U * group + local];
        const uint64_t phase = (uint64_t)(((qsop_qpf_u128_t)(target_mode % qsop->r) *
                                           qsop->unary[global]) %
                                          qsop->r);
        const uint8_t clifford_adjustment = (uint8_t)(((phase + 7U) % 8U) / 2U);
        qsop_qpf_set_linear(&out->terms[index], global,
                            (uint8_t)(block[selected].ell[local] + clifford_adjustment));
        for (uint32_t other = local + 1U; other < 6U; other++) {
          if (((block[selected].quad[(size_t)local * block[selected].words + other / 64U] >>
                (other % 64U)) &
               1U) != 0U) {
            qsop_qpf_toggle_quadratic(&out->terms[index], global,
                                      magic_vars[6U * group + other]);
          }
        }
      }
      for (uint32_t row = 0; row < block[selected].aff_len; row++) {
        qsop_bitset_zero(constraint, out->terms[index].words);
        for (uint32_t local = 0; local < 6U; local++) {
          if (((block[selected].aff_rows[(size_t)row * block[selected].words] >> local) & 1U) !=
              0U) {
            qsop_bitset_set(constraint, magic_vars[6U * group + local]);
          }
        }
        if (!qsop_qpf_add_constraint(&out->terms[index], constraint,
                                     block[selected].aff_rhs[row], error)) {
          free(constraint);
          goto fail;
        }
      }
    }
    for (uint32_t j = 0; j < remainder_count; j++) {
      const bool selected = (selector & 1U) != 0U;
      selector >>= 1U;
      if (!qsop_qpf_scalar_mul(&out->terms[index].eps, selected ? &beta[j] : &alpha[j], error)) {
        free(constraint);
        goto fail;
      }
      qsop_qpf_set_linear(&out->terms[index], magic_vars[6U * block_count + j],
                          selected ? 1U : 0U);
    }
    free(constraint);
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      const uint64_t phase =
          (uint64_t)(((qsop_qpf_u128_t)(target_mode % qsop->r) * qsop->unary[v]) % qsop->r);
      if (((qsop_qpf_u128_t)4U * phase) % qsop->r == 0U) {
        qsop_qpf_set_linear(&out->terms[index], v,
                            (uint8_t)((4U * phase / qsop->r) & 3U));
      }
    }
    if ((target_mode & 1U) != 0U) {
      for (uint32_t edge = 0; edge < qsop->nedges; edge++) {
        qsop_qpf_toggle_quadratic(&out->terms[index], qsop->edge_u[edge], qsop->edge_v[edge]);
      }
    }
  }
  for (uint32_t i = 0; i < 6U; i++) {
    qsop_qpf_free(&block[i]);
  }
  free(magic_vars);
  free(alpha);
  free(beta);
  return true;

fail:
  for (uint32_t i = 0; i < 6U; i++) {
    qsop_qpf_free(&block[i]);
  }
  free(magic_vars);
  free(alpha);
  free(beta);
  qsop_qpf_term_list_free(out);
  return false;
}

bool qsop_qpf_magic_decompose(const qsop_instance_t *qsop, uint64_t target_mode,
                              uint64_t max_terms, const qsop_qpf_scalar_t *unit,
                              qsop_qpf_term_list_t *out, qsop_error_t *error) {
  if (qsop == NULL || unit == NULL || out == NULL || qsop->r == 0) {
    qsop_set_error(error, "invalid QPF magic decomposition argument");
    return false;
  }
  *out = (qsop_qpf_term_list_t){0};
  target_mode %= qsop->r;
  const uint32_t magic_count = qsop_magic_vertex_count(qsop, target_mode);
  if (qsop->r == 8U && magic_count >= 6U && unit->order == 8U) {
    return qpf_magic_decompose_t6(qsop, target_mode, max_terms, unit, out, error);
  }
  const uint64_t term_count = qsop_qpf_naive_term_bound(magic_count);
  const uint64_t budget = max_terms == 0 ? QPF_DEFAULT_MAX_TERMS : max_terms;
  if (term_count == UINT64_MAX || term_count > budget || term_count > SIZE_MAX / sizeof(*out->terms)) {
    qsop_set_error(error,
                   "QPF decomposition needs 2^%" PRIu32 " terms (budget=%" PRIu64 ")",
                   magic_count, budget);
    return false;
  }
  uint32_t *magic_index = malloc((size_t)(qsop->nvars == 0 ? 1U : qsop->nvars) *
                                 sizeof(*magic_index));
  qsop_qpf_scalar_t *alpha = calloc(magic_count == 0 ? 1U : magic_count, sizeof(*alpha));
  qsop_qpf_scalar_t *beta = calloc(magic_count == 0 ? 1U : magic_count, sizeof(*beta));
  out->terms = calloc((size_t)term_count, sizeof(*out->terms));
  if (magic_index == NULL || alpha == NULL || beta == NULL || out->terms == NULL) {
    free(magic_index);
    free(alpha);
    free(beta);
    qsop_qpf_term_list_free(out);
    qsop_set_error(error, "out of memory while allocating a QPF decomposition");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    magic_index[v] = UINT32_MAX;
  }
  uint32_t next_magic = 0;
  const uint64_t order_per_r = unit->order / qsop->r;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    const uint64_t phase = (uint64_t)(((__uint128_t)target_mode * qsop->unary[v]) % qsop->r);
    if (((__uint128_t)4U * phase) % qsop->r != 0) {
      magic_index[v] = next_magic;
      if (!scalar_magic_coefficients(unit, phase * order_per_r, &alpha[next_magic],
                                     &beta[next_magic], error)) {
        free(magic_index);
        free(alpha);
        free(beta);
        qsop_qpf_term_list_free(out);
        return false;
      }
      next_magic++;
    }
  }

  out->len = term_count;
  for (uint64_t mask = 0; mask < term_count; mask++) {
    qsop_qpf_scalar_t eps = scalar_like(unit);
    const uint64_t constant_phase =
        (uint64_t)(((__uint128_t)target_mode * qsop->constant) % qsop->r);
    if (!qsop_qpf_scalar_mul_root(&eps, constant_phase * order_per_r, error)) {
      free(magic_index);
      free(alpha);
      free(beta);
      qsop_qpf_term_list_free(out);
      return false;
    }
    for (uint32_t j = 0; j < magic_count; j++) {
      const qsop_qpf_scalar_t *coefficient = ((mask >> j) & 1U) != 0 ? &beta[j] : &alpha[j];
      if (!qsop_qpf_scalar_mul(&eps, coefficient, error)) {
        free(magic_index);
        free(alpha);
        free(beta);
        qsop_qpf_term_list_free(out);
        return false;
      }
    }
    if (!qsop_qpf_init(&out->terms[mask], qsop->nvars, &eps, error)) {
      free(magic_index);
      free(alpha);
      free(beta);
      qsop_qpf_term_list_free(out);
      return false;
    }
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if (magic_index[v] != UINT32_MAX) {
        qsop_qpf_set_linear(&out->terms[mask], v,
                            (uint8_t)((mask >> magic_index[v]) & 1U));
      } else {
        const uint64_t phase =
            (uint64_t)(((__uint128_t)target_mode * qsop->unary[v]) % qsop->r);
        qsop_qpf_set_linear(&out->terms[mask], v,
                            (uint8_t)((4U * phase / qsop->r) & 3U));
      }
    }
    if ((target_mode & 1U) != 0) {
      for (uint32_t edge = 0; edge < qsop->nedges; edge++) {
        qsop_qpf_toggle_quadratic(&out->terms[mask], qsop->edge_u[edge], qsop->edge_v[edge]);
      }
    }
  }
  free(magic_index);
  free(alpha);
  free(beta);
  return true;
}

bool qsop_qpf_term_list_total(const qsop_qpf_term_list_t *list, qsop_qpf_scalar_t *out,
                              qsop_error_t *error) {
  if (list == NULL || out == NULL || list->len == 0) {
    qsop_set_error(error, "invalid QPF term-list sum argument");
    return false;
  }
  if (!qsop_qpf_total_sum(&list->terms[0], out, error)) {
    return false;
  }
  for (uint64_t i = 1; i < list->len; i++) {
    qsop_qpf_scalar_t term = {0};
    if (!qsop_qpf_total_sum(&list->terms[i], &term, error) ||
        !qsop_qpf_scalar_add(out, &term, error)) {
      return false;
    }
  }
  return true;
}

static bool solve_mode_exact(const qsop_instance_t *qsop, uint32_t order, uint32_t mode,
                             uint64_t max_terms, qsop_qpf_scalar_t *out,
                             qsop_solve_stats_t *stats, qsop_error_t *error) {
  qsop_qpf_scalar_t unit = {0};
  if (!qsop_qpf_scalar_cyclotomic(&unit, order, 0U, error)) {
    return false;
  }
  qsop_qpf_term_list_t terms = {0};
  if (!qsop_qpf_magic_decompose(qsop, mode, max_terms, &unit, &terms, error)) {
    return false;
  }
  qsop_qpf_scalar_t total = {0};
  const bool ok = qsop_qpf_term_list_total(&terms, &total, error);
  if (ok && stats != NULL) {
    stats->qpf_decompositions++;
    stats->qpf_terms += terms.len;
    if (terms.len > stats->qpf_max_terms) {
      stats->qpf_max_terms = terms.len;
    }
    const uint32_t magic = qsop_magic_vertex_count(qsop, mode);
    if (magic > stats->qpf_magic_vertices) {
      stats->qpf_magic_vertices = magic;
    }
  }
  qsop_qpf_term_list_free(&terms);
  if (ok) {
    *out = total;
  }
  return ok;
}

static char *qpf_i128_decimal(qsop_qpf_i128_t value, qsop_error_t *error) {
  if (value < 0) {
    qsop_set_error(error, "exact QPF count is negative");
    return NULL;
  }
  char reverse[48] = {0};
  size_t length = 0;
  do {
    reverse[length++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  char *text = malloc(length + 1U);
  if (text == NULL) {
    qsop_set_error(error, "out of memory while formatting an exact QPF count");
    return NULL;
  }
  for (size_t i = 0; i < length; i++) {
    text[i] = reverse[length - 1U - i];
  }
  text[length] = '\0';
  return text;
}

static bool exact_scalar_count(const qsop_qpf_scalar_t *scalar, qsop_qpf_i128_t *out,
                               qsop_error_t *error) {
  if (scalar->backend != QSOP_QPF_SCALAR_CYCLOTOMIC || scalar->denominator_exp2 != 0U) {
    qsop_set_error(error, "inverse Fourier QPF count is not an integer");
    return false;
  }
  for (uint32_t i = 1; i < scalar->cyclotomic_degree; i++) {
    if (scalar->coefficients[i] != 0) {
      qsop_set_error(error, "inverse Fourier QPF count has a non-rational cyclotomic part");
      return false;
    }
  }
  if (scalar->coefficients[0] < 0) {
    qsop_set_error(error, "inverse Fourier QPF count is negative");
    return false;
  }
  *out = scalar->coefficients[0];
  return true;
}

bool qsop_solve_qpf(const qsop_instance_t *qsop, uint64_t max_terms, qsop_result_t **out,
                    qsop_solve_stats_t *stats, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || out == NULL) {
    qsop_set_error(error, "invalid QPF solve argument");
    return false;
  }
  *out = NULL;
  uint32_t order = 0;
  if (!qpf_phase_order(qsop->r, &order, error)) {
    return false;
  }
  if (order > QSOP_QPF_CYCLOTOMIC_MAX_ORDER) {
    qsop_set_error(error,
                   "exact QPF count path supports lcm(R,8) <= %u (got %" PRIu32 ")",
                   QSOP_QPF_CYCLOTOMIC_MAX_ORDER, order);
    return false;
  }
  /* Fixed signed-128 coefficients deliberately impose a lower exact threshold than the
   * uint32 modulus ceiling. With the default 2^12 term cap, n<=96 leaves headroom for magic
   * coefficient growth and makes overflow a checked refusal rather than silent wraparound. */
  if (qsop->nvars > 96U) {
    qsop_set_error(error, "exact QPF count path supports at most 96 variables");
    return false;
  }
  const uint32_t r = (uint32_t)qsop->r;
  qsop_qpf_scalar_t *modes = calloc(r == 0 ? 1U : r, sizeof(*modes));
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (modes == NULL || result == NULL) {
    free(modes);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating an exact QPF solve");
    return false;
  }
  result->r = r;
  result->norm_h = qsop->norm_h;
  bool ok = true;
  for (uint32_t mode = 0; mode < r && ok; mode++) {
    ok = solve_mode_exact(qsop, order, mode, max_terms, &modes[mode], stats, error);
  }
  if (ok && qsop->nvars < 64U) {
    ok = qsop_counts_alloc(r, &result->counts, error);
  } else if (ok) {
    result->count_strings = calloc(r, sizeof(*result->count_strings));
    if (result->count_strings == NULL) {
      qsop_set_error(error, "out of memory while allocating exact QPF result strings");
      ok = false;
    }
  }
  for (uint32_t residue = 0; residue < r && ok; residue++) {
    qsop_qpf_scalar_t inverse = {0};
    if (!qsop_qpf_scalar_cyclotomic(&inverse, order, 0U, error)) {
      ok = false;
      break;
    }
    memset(inverse.coefficients, 0, sizeof(inverse.coefficients));
    for (uint32_t mode = 0; mode < r && ok; mode++) {
      qsop_qpf_scalar_t term = modes[mode];
      const uint64_t phase =
          ((uint64_t)(r - ((uint64_t)mode * residue) % r) % r) * (order / r);
      ok = qsop_qpf_scalar_mul_root(&term, phase, error) &&
           qsop_qpf_scalar_add(&inverse, &term, error);
    }
    if (ok) {
      ok = qsop_qpf_scalar_divide_u64_exact(&inverse, r, error);
    }
    qsop_qpf_i128_t count = 0;
    if (ok) {
      ok = exact_scalar_count(&inverse, &count, error);
    }
    if (ok && result->counts != NULL) {
      if ((qsop_qpf_u128_t)count > UINT64_MAX) {
        qsop_set_error(error, "exact QPF count exceeds uint64 storage");
        ok = false;
      } else {
        result->counts[residue] = (uint64_t)count;
      }
    } else if (ok) {
      result->count_strings[residue] = qpf_i128_decimal(count, error);
      ok = result->count_strings[residue] != NULL;
    }
  }
  free(modes);
  if (!ok) {
    qsop_result_free(result);
    return false;
  }
  *out = result;
  return true;
}

bool qsop_solve_qpf_single_mode(const qsop_instance_t *qsop, uint32_t target_mode,
                                uint64_t max_terms, qsop_amplitude_t *out,
                                qsop_solve_stats_t *stats, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || out == NULL) {
    qsop_set_error(error, "invalid QPF single-mode solve argument");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  uint32_t order = 0;
  if (!qpf_phase_order(qsop->r, &order, error)) {
    return false;
  }
  qsop_qpf_scalar_t unit = {0};
  qsop_qpf_scalar_complex(&unit, order, 0);
  qsop_qpf_term_list_t terms = {0};
  if (!qsop_qpf_magic_decompose(qsop, target_mode, max_terms, &unit, &terms, error)) {
    return false;
  }
  qsop_qpf_scalar_t total = {0};
  const bool ok = qsop_qpf_term_list_total(&terms, &total, error);
  if (ok) {
    out->re = total.re;
    out->im = total.im;
    out->scale_exp2 = total.scale_exp2;
    out->numeric_error_bound = total.numeric_error_bound;
    if (stats != NULL) {
      stats->qpf_decompositions = 1;
      stats->qpf_terms = terms.len;
      stats->qpf_max_terms = terms.len;
      stats->qpf_magic_vertices = qsop_magic_vertex_count(qsop, target_mode);
    }
  }
  qsop_qpf_term_list_free(&terms);
  return ok;
}
