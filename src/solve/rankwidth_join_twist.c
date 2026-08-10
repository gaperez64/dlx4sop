/* Precision-independent machinery for the twist-diagonalized (WHT) single-Fourier-mode join
 * (paper thm:fast-join-wht / cor:crossing-join): the per-join plan (parent-coordinate basis,
 * crossing-form rank factorization), the in-place Walsh--Hadamard butterflies used by the
 * long-double and f64 kernels in rankwidth_dp_complex.c, and the exact modular count-WHT
 * used for reachability when direct pair marking exceeds its budget.
 *
 * The crossing form B(a, b) = a^T A[X_L, X_R] b factors through its rank c as <L a, R b>:
 * the cross basis spans the rows adj[v] & X_R for v in X_L, recording the source vertex of
 * each independent row. Because a child table's signatures are masked to
 * X_sibling | outside(parent), both twist coordinates are functions of the signature alone:
 * the left coordinate is the cross-basis coordinate of sig_L & X_R, and bit j of the right
 * coordinate is bit gen_vertex[j] of sig_R. */
#include "../core/qsop_internal.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop_solve.h"
#include "rankwidth_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

void rw_twist_plan_free(rw_twist_plan_t *plan) {
  if (plan == NULL) {
    return;
  }
  rw_dense_basis_free(&plan->parent_basis);
  rw_dense_basis_free(&plan->cross_basis);
  free(plan->gen_vertex);
  *plan = (rw_twist_plan_t){0};
}

rw_twist_feasibility_t rw_twist_plan_build(uint32_t nvars, const uint64_t *adj,
                                           const rw_signature_pool_t *pool,
                                           const uint32_t *left_signatures, size_t left_len,
                                           const uint32_t *right_signatures, size_t right_len,
                                           const uint64_t *left_vars, const uint64_t *right_vars,
                                           const uint64_t *outside, size_t words, bool want_twist,
                                           uint32_t max_dim, rw_twist_plan_t *plan,
                                           qsop_error_t *error) {
  *plan = (rw_twist_plan_t){0};
  if (max_dim > RW_TWIST_MAX_DIM) {
    max_dim = RW_TWIST_MAX_DIM;
  }

  const size_t w = words == 0 ? 1U : words;
  rw_twist_feasibility_t status = RW_TWIST_FEASIBLE;
  uint64_t *scratch = calloc(w, sizeof(*scratch));
  uint64_t *masked = calloc(w, sizeof(*masked));
  if (scratch == NULL || masked == NULL) {
    qsop_set_error(error, "out of memory while planning rankwidth twist join");
    status = RW_TWIST_ERROR;
    goto cleanup;
  }
  if (!rw_dense_basis_init(&plan->parent_basis, nvars, words, error)) {
    status = RW_TWIST_ERROR;
    goto cleanup;
  }

  if (want_twist) {
    if (!rw_dense_basis_init(&plan->cross_basis, nvars, words, error)) {
      status = RW_TWIST_ERROR;
      goto cleanup;
    }
    plan->gen_vertex = malloc((max_dim == 0 ? 1U : (size_t)max_dim) * sizeof(*plan->gen_vertex));
    if (plan->gen_vertex == NULL) {
      qsop_set_error(error, "out of memory while planning rankwidth twist join");
      status = RW_TWIST_ERROR;
      goto cleanup;
    }
    for (uint32_t v = 0; v < nvars; v++) {
      if (!qsop_bitset_get(left_vars, v)) {
        continue;
      }
      qsop_bitset_copy(masked, qsop_bitset_const_row(adj, words, v), words);
      qsop_bitset_and(masked, right_vars, words);
      const uint32_t before = plan->cross_basis.dim;
      if (!rw_dense_basis_add(&plan->cross_basis, masked, scratch, NULL)) {
        status = RW_TWIST_TOO_LARGE;
        goto cleanup;
      }
      if (plan->cross_basis.dim > before) {
        if (plan->cross_basis.dim > max_dim) {
          status = RW_TWIST_TOO_LARGE;
          goto cleanup;
        }
        plan->gen_vertex[before] = v;
      }
    }
  }
  plan->c = plan->cross_basis.dim;

  for (size_t i = 0; i < left_len + right_len; i++) {
    const uint32_t signature =
        i < left_len ? left_signatures[i] : right_signatures[i - left_len];
    qsop_bitset_copy(masked, rw_signature_bits(pool, signature), words);
    qsop_bitset_and(masked, outside, words);
    if (!rw_dense_basis_add(&plan->parent_basis, masked, scratch, NULL)) {
      status = RW_TWIST_TOO_LARGE;
      goto cleanup;
    }
    if (plan->parent_basis.dim + plan->c > max_dim) {
      status = RW_TWIST_TOO_LARGE;
      goto cleanup;
    }
  }
  plan->p = plan->parent_basis.dim;

  {
    const uint32_t pc = plan->p + plan->c;
    const uint64_t transform_ops = ((uint64_t)1 << pc) * ((uint64_t)pc + 1U) +
                                   ((uint64_t)1 << plan->p) * ((uint64_t)plan->p + 1U);
    plan->forecast_ops = qsop_saturating_add_u64(
        transform_ops, qsop_saturating_add_u64((uint64_t)left_len, (uint64_t)right_len));
  }

cleanup:
  free(scratch);
  free(masked);
  if (status != RW_TWIST_FEASIBLE) {
    rw_twist_plan_free(plan);
  }
  return status;
}

/* In-place WHT over the c-axis (contiguous rows of length 2^c) of a flat [2^p][2^c] table. */
void rw_twist_wht_cols_l(long double *re, long double *im, uint32_t p, uint32_t c) {
  const size_t rows = (size_t)1 << p;
  const size_t cols = (size_t)1 << c;
  for (size_t row = 0; row < rows; row++) {
    long double *row_re = re + row * cols;
    long double *row_im = im + row * cols;
    for (size_t half = 1; half < cols; half <<= 1) {
      for (size_t base = 0; base < cols; base += half << 1) {
        for (size_t j = base; j < base + half; j++) {
          const long double are = row_re[j];
          const long double aim = row_im[j];
          const long double bre = row_re[j + half];
          const long double bim = row_im[j + half];
          row_re[j] = are + bre;
          row_im[j] = aim + bim;
          row_re[j + half] = are - bre;
          row_im[j + half] = aim - bim;
        }
      }
    }
  }
}

/* In-place WHT over the p-axis (stride 2^c) of a flat [2^p][2^c] table. */
void rw_twist_wht_rows_l(long double *re, long double *im, uint32_t p, uint32_t c) {
  const size_t rows = (size_t)1 << p;
  const size_t cols = (size_t)1 << c;
  for (size_t half = 1; half < rows; half <<= 1) {
    for (size_t base = 0; base < rows; base += half << 1) {
      for (size_t j = base; j < base + half; j++) {
        long double *are = re + j * cols;
        long double *aim = im + j * cols;
        long double *bre = re + (j + half) * cols;
        long double *bim = im + (j + half) * cols;
        for (size_t s = 0; s < cols; s++) {
          const long double xre = are[s];
          const long double xim = aim[s];
          const long double yre = bre[s];
          const long double yim = bim[s];
          are[s] = xre + yre;
          aim[s] = xim + yim;
          bre[s] = xre - yre;
          bim[s] = xim - yim;
        }
      }
    }
  }
}

void rw_twist_wht_cols_f64(double *re, double *im, uint32_t p, uint32_t c) {
  const size_t rows = (size_t)1 << p;
  const size_t cols = (size_t)1 << c;
  for (size_t row = 0; row < rows; row++) {
    double *row_re = re + row * cols;
    double *row_im = im + row * cols;
    for (size_t half = 1; half < cols; half <<= 1) {
      for (size_t base = 0; base < cols; base += half << 1) {
        for (size_t j = base; j < base + half; j++) {
          const double are = row_re[j];
          const double aim = row_im[j];
          const double bre = row_re[j + half];
          const double bim = row_im[j + half];
          row_re[j] = are + bre;
          row_im[j] = aim + bim;
          row_re[j + half] = are - bre;
          row_im[j + half] = aim - bim;
        }
      }
    }
  }
}

void rw_twist_wht_rows_f64(double *re, double *im, uint32_t p, uint32_t c) {
  const size_t rows = (size_t)1 << p;
  const size_t cols = (size_t)1 << c;
  for (size_t half = 1; half < rows; half <<= 1) {
    for (size_t base = 0; base < rows; base += half << 1) {
      for (size_t j = base; j < base + half; j++) {
        double *are = re + j * cols;
        double *aim = im + j * cols;
        double *bre = re + (j + half) * cols;
        double *bim = im + (j + half) * cols;
        for (size_t s = 0; s < cols; s++) {
          const double xre = are[s];
          const double xim = aim[s];
          const double yre = bre[s];
          const double yim = bim[s];
          are[s] = xre + yre;
          aim[s] = xim + yim;
          bre[s] = xre - yre;
          bim[s] = xim - yim;
        }
      }
    }
  }
}

/* Exact XOR-convolution support counts over F_2^p via a WHT modulo the Mersenne prime
 * M = 2^61 - 1. The true pair counts are at most 2^{2p} <= 2^44 < M, so the residues are
 * the exact counts; the inverse transform's 2^{-p} is the multiplication by 2^{61-p}
 * (2^61 = 1 mod M). Unsigned __int128 is available on every supported compiler/target
 * (GCC/Clang on x86_64/aarch64, same assumption as the rest of the SIMD plumbing). */
#define RW_TWIST_REACH_PRIME ((UINT64_C(1) << 61) - 1U)

static inline uint64_t twist_reach_addmod(uint64_t left, uint64_t right) {
  const uint64_t sum = left + right;
  return sum >= RW_TWIST_REACH_PRIME ? sum - RW_TWIST_REACH_PRIME : sum;
}

static inline uint64_t twist_reach_submod(uint64_t left, uint64_t right) {
  return left >= right ? left - right : left + RW_TWIST_REACH_PRIME - right;
}

static inline uint64_t twist_reach_mulmod(uint64_t left, uint64_t right) {
  return (uint64_t)(((unsigned __int128)left * right) % RW_TWIST_REACH_PRIME);
}

static void twist_reach_wht(uint64_t *values, size_t len) {
  for (size_t half = 1; half < len; half <<= 1) {
    for (size_t base = 0; base < len; base += half << 1) {
      for (size_t j = base; j < base + half; j++) {
        const uint64_t a = values[j];
        const uint64_t b = values[j + half];
        values[j] = twist_reach_addmod(a, b);
        values[j + half] = twist_reach_submod(a, b);
      }
    }
  }
}

bool rw_twist_reach_counts(const uint64_t *occ_left, const uint64_t *occ_right, uint32_t p,
                           uint64_t *work, uint64_t *counts_out, qsop_error_t *error) {
  if (p > RW_TWIST_MAX_DIM) {
    qsop_set_error(error, "rankwidth twist reachability dimension exceeds %" PRIu32,
                   (uint32_t)RW_TWIST_MAX_DIM);
    return false;
  }
  const size_t len = (size_t)1 << p;
  uint64_t *left_values = work;
  uint64_t *right_values = work + len;
  for (size_t i = 0; i < len; i++) {
    left_values[i] = qsop_bitset_get(occ_left, (uint32_t)i) ? 1U : 0U;
    right_values[i] = qsop_bitset_get(occ_right, (uint32_t)i) ? 1U : 0U;
  }
  twist_reach_wht(left_values, len);
  twist_reach_wht(right_values, len);
  for (size_t i = 0; i < len; i++) {
    left_values[i] = twist_reach_mulmod(left_values[i], right_values[i]);
  }
  twist_reach_wht(left_values, len);
  const uint64_t inverse_pow2 = p == 0 ? 1U : (UINT64_C(1) << (61U - p));
  for (size_t i = 0; i < len; i++) {
    counts_out[i] = twist_reach_mulmod(left_values[i], inverse_pow2);
  }
  return true;
}
