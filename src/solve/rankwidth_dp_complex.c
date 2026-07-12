/* The single-Fourier-mode complex dynamic program (long-double and f64/complex64), including the
 * CSR/streaming/dense join machinery and the SIMD row-batching helper.
 *
 * Part of the rankwidth.c file split (pure movement, no logic changes) -- see
 * rankwidth_internal.h for the shared types and cross-TU declarations. */
#include "../core/qsop_internal.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "dlx4sop/simd.h"
#include "rankwidth_internal.h"
#include "trace.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum rw_dense_join_feasibility {
  RW_DENSE_JOIN_FEASIBLE,
  RW_DENSE_JOIN_TOO_LARGE,
  RW_DENSE_JOIN_ERROR,
} rw_dense_join_feasibility_t;
typedef struct rw_complex_transition32 {
  uint32_t right_index;
  uint32_t parent_index;
  uint32_t flags; /* bit 0 = sign flip */
  uint32_t reserved;
} rw_complex_transition32_t;
typedef struct rw_complex_transition_csr {
  uint32_t left_count;
  uint32_t *offsets;
  rw_complex_transition32_t *items;
  uint64_t transition_count;
} rw_complex_transition_csr_t;
typedef struct rw_complex_context {
  uint64_t r;
  uint32_t target_mode;
  long double sign_re;
  int scale_exp2;
  uint64_t complex_ops;
  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
} rw_complex_context_t;
static void complex_table_renormalize(rw_complex_table_t *table, rw_complex_context_t *ctx) {
  long double peak = 0.0L;
  for (size_t i = 0; i < table->len; i++) {
    const long double re = fabsl(table->re[i]);
    const long double im = fabsl(table->im[i]);
    if (re > peak) {
      peak = re;
    }
    if (im > peak) {
      peak = im;
    }
  }
  if (peak == 0.0L || !isfinite(peak)) {
    return;
  }
  const int exponent = ilogbl(peak);
  if (exponent == 0) {
    return;
  }
  const long double scale = ldexpl(1.0L, -exponent);
  for (size_t i = 0; i < table->len; i++) {
    table->re[i] *= scale;
    table->im[i] *= scale;
  }
  ctx->scale_exp2 += exponent;
}
typedef struct rw_complex64_context {
  uint64_t r;
  uint32_t target_mode;
  double sign_re;
  int scale_exp2;
  const qsop_simd_vtable_t *simd;
  uint64_t complex_ops;
  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
} rw_complex64_context_t;
static void complex64_table_renormalize(rw_complex64_table_t *table, rw_complex64_context_t *ctx) {
  double peak = 0.0;
  for (size_t i = 0; i < table->len; i++) {
    const double re = fabs(table->re[i]);
    const double im = fabs(table->im[i]);
    if (re > peak) {
      peak = re;
    }
    if (im > peak) {
      peak = im;
    }
  }
  if (peak == 0.0 || !isfinite(peak)) {
    return;
  }

  const int exponent = ilogb(peak);
  if (exponent == 0) {
    return;
  }
  const double scale = ldexp(1.0, -exponent);
  for (size_t i = 0; i < table->len; i++) {
    table->re[i] *= scale;
    table->im[i] *= scale;
  }
  ctx->scale_exp2 += exponent;
}
static void record_rankwidth_f64_simd_kernel(qsop_solve_stats_t *stats,
                                             const qsop_simd_vtable_t *simd) {
  if (stats == NULL || simd == NULL) {
    return;
  }
  const char *name = qsop_simd_kernel_name(simd);
  if (strcmp(name, "avx512") == 0) {
    stats->simd_kernel = QSOP_SIMD_KERNEL_AVX512;
  } else if (strcmp(name, "avx2") == 0) {
    stats->simd_kernel = QSOP_SIMD_KERNEL_AVX2;
  } else if (strcmp(name, "neon") == 0) {
    stats->simd_kernel = QSOP_SIMD_KERNEL_NEON;
  } else {
    stats->simd_kernel = QSOP_SIMD_KERNEL_SCALAR;
  }
}
static void note_rankwidth_f64_scalar_fallback(const rw_complex64_context_t *ctx, uint64_t ops) {
  if (ctx != NULL && ctx->stats != NULL) {
    qsop_add_saturating_u64(&ctx->stats->simd_scalar_fallback_ops, ops);
  }
}
static void rw_complex_transition_csr_free(rw_complex_transition_csr_t *csr) {
  if (csr == NULL) {
    return;
  }
  free(csr->offsets);
  free(csr->items);
  *csr = (rw_complex_transition_csr_t){0};
}
static uint64_t rw_complex_transition_csr_bytes(const rw_complex_transition_csr_t *csr) {
  if (csr == NULL || csr->left_count == 0) {
    return 0;
  }
  return (uint64_t)(csr->left_count + 1U) * sizeof(uint32_t) +
         csr->transition_count * sizeof(rw_complex_transition32_t);
}
static bool complex_table_signature_index(rw_complex_table_t *table, uint32_t signature,
                                          const uint64_t *assignment, size_t words, size_t *out,
                                          qsop_error_t *error) {
  if (table->signature_slots == NULL ||
      (table->len + 1U) * 2U > (table->signature_slots_mask + 1U)) {
    if (!rw_complex_slots_rehash(table, error)) {
      return false;
    }
  }

  const size_t mask = table->signature_slots_mask;
  size_t slot = rw_rep_hash(signature) & mask;
  while (table->signature_slots[slot] != UINT32_MAX) {
    const uint32_t index = table->signature_slots[slot];
    if (table->signatures[index] == signature) {
      *out = index;
      return true;
    }
    slot = (slot + 1U) & mask;
  }
  if (!rw_reserve_complex_table(table, table->len + 1U, words, error)) {
    return false;
  }
  const size_t index = table->len++;
  table->signatures[index] = signature;
  qsop_bitset_copy(rw_complex_assignment(table, index, words), assignment, words);
  table->assignment_weights[index] = qsop_bitset_popcount(assignment, words);
  table->re[index] = 0.0L;
  table->im[index] = 0.0L;
  table->signature_slots[slot] = (uint32_t)index;
  *out = index;
  return true;
}
static bool complex_table_find_signature(const rw_complex_table_t *table, uint32_t signature,
                                         size_t *out) {
  if (table->signature_slots != NULL && table->signature_slots_mask != 0) {
    const size_t mask = table->signature_slots_mask;
    size_t slot = rw_rep_hash(signature) & mask;
    while (table->signature_slots[slot] != UINT32_MAX) {
      const uint32_t index = table->signature_slots[slot];
      if (table->signatures[index] == signature) {
        *out = index;
        return true;
      }
      slot = (slot + 1U) & mask;
    }
    return false;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->signatures[i] == signature) {
      *out = i;
      return true;
    }
  }
  return false;
}
static bool complex64_table_signature_index(rw_complex64_table_t *table, uint32_t signature,
                                            const uint64_t *assignment, size_t words, size_t *out,
                                            qsop_error_t *error) {
  if (table->signature_slots == NULL ||
      (table->len + 1U) * 2U > (table->signature_slots_mask + 1U)) {
    if (!rw_complex64_slots_rehash(table, error)) {
      return false;
    }
  }

  const size_t mask = table->signature_slots_mask;
  size_t slot = rw_rep_hash(signature) & mask;
  while (table->signature_slots[slot] != UINT32_MAX) {
    const uint32_t index = table->signature_slots[slot];
    if (table->signatures[index] == signature) {
      *out = index;
      return true;
    }
    slot = (slot + 1U) & mask;
  }
  if (!rw_reserve_complex64_table(table, table->len + 1U, words, error)) {
    return false;
  }
  const size_t index = table->len++;
  table->signatures[index] = signature;
  qsop_bitset_copy(rw_complex64_assignment(table, index, words), assignment, words);
  table->assignment_weights[index] = qsop_bitset_popcount(assignment, words);
  table->re[index] = 0.0;
  table->im[index] = 0.0;
  table->signature_slots[slot] = (uint32_t)index;
  *out = index;
  return true;
}
static bool complex64_table_find_signature(const rw_complex64_table_t *table, uint32_t signature,
                                           size_t *out) {
  if (table->signature_slots != NULL && table->signature_slots_mask != 0) {
    const size_t mask = table->signature_slots_mask;
    size_t slot = rw_rep_hash(signature) & mask;
    while (table->signature_slots[slot] != UINT32_MAX) {
      const uint32_t index = table->signature_slots[slot];
      if (table->signatures[index] == signature) {
        *out = index;
        return true;
      }
      slot = (slot + 1U) & mask;
    }
    return false;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->signatures[i] == signature) {
      *out = i;
      return true;
    }
  }
  return false;
}
static bool solve_leaf_complex(const qsop_instance_t *qsop, const uint64_t *adj,
                               const rw_node_t *node, uint32_t target_mode, size_t words,
                               rw_signature_pool_t *pool, rw_complex_table_t *table,
                               uint64_t *zero_bits, uint64_t *one_bits,
                               uint64_t *signature_bits_buffer, qsop_error_t *error) {
  const size_t w = words == 0 ? 1U : words;
  memset(zero_bits, 0, w * sizeof(*zero_bits));
  memset(one_bits, 0, w * sizeof(*one_bits));
  memset(signature_bits_buffer, 0, w * sizeof(*signature_bits_buffer));

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature_bits_buffer, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature_bits_buffer, node->var);
  qsop_bitset_set(one_bits, node->var);
  size_t zero = 0;
  size_t one = 0;
  if (!rw_signature_pool_intern(pool, zero_bits, &zero_signature, error) ||
      !rw_signature_pool_intern(pool, signature_bits_buffer, &one_signature, error) ||
      !complex_table_signature_index(table, zero_signature, zero_bits, words, &zero, error) ||
      !complex_table_signature_index(table, one_signature, one_bits, words, &one, error)) {
    return false;
  }
  table->re[zero] += 1.0L;
  long double re = 0.0L;
  long double im = 0.0L;
  qsop_root_of_unity_l(qsop->r, target_mode, qsop->unary[node->var] % qsop->r, &re, &im);
  table->re[one] += re;
  table->im[one] += im;
  return true;
}
static bool solve_leaf_complex64(const qsop_instance_t *qsop, const uint64_t *adj,
                                 const rw_node_t *node, uint32_t target_mode, size_t words,
                                 rw_signature_pool_t *pool, rw_complex64_table_t *table,
                                 uint64_t *zero_bits, uint64_t *one_bits,
                                 uint64_t *signature_bits_buffer, qsop_error_t *error) {
  const size_t w = words == 0 ? 1U : words;
  memset(zero_bits, 0, w * sizeof(*zero_bits));
  memset(one_bits, 0, w * sizeof(*one_bits));
  memset(signature_bits_buffer, 0, w * sizeof(*signature_bits_buffer));

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature_bits_buffer, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature_bits_buffer, node->var);
  qsop_bitset_set(one_bits, node->var);
  size_t zero = 0;
  size_t one = 0;
  if (!rw_signature_pool_intern(pool, zero_bits, &zero_signature, error) ||
      !rw_signature_pool_intern(pool, signature_bits_buffer, &one_signature, error) ||
      !complex64_table_signature_index(table, zero_signature, zero_bits, words, &zero, error) ||
      !complex64_table_signature_index(table, one_signature, one_bits, words, &one, error)) {
    return false;
  }
  table->re[zero] += 1.0;
  double re = 0.0;
  double im = 0.0;
  qsop_root_of_unity_f64(qsop->r, target_mode, qsop->unary[node->var] % qsop->r, &re, &im);
  table->re[one] += re;
  table->im[one] += im;
  return true;
}
static bool dense_basis_from_complex_table(const rw_complex_table_t *table,
                                           const rw_signature_pool_t *pool, uint32_t nbits,
                                           rw_dense_basis_t *basis, uint64_t *scratch,
                                           qsop_error_t *error) {
  if (!rw_dense_basis_init(basis, nbits, pool->words, error)) {
    return false;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (!rw_dense_basis_add(basis, rw_signature_bits(pool, table->signatures[i]), scratch, error)) {
      rw_dense_basis_free(basis);
      return false;
    }
  }
  return true;
}
static bool dense_basis_from_complex64_table(const rw_complex64_table_t *table,
                                             const rw_signature_pool_t *pool, uint32_t nbits,
                                             rw_dense_basis_t *basis, uint64_t *scratch,
                                             qsop_error_t *error) {
  if (!rw_dense_basis_init(basis, nbits, pool->words, error)) {
    return false;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (!rw_dense_basis_add(basis, rw_signature_bits(pool, table->signatures[i]), scratch, error)) {
      rw_dense_basis_free(basis);
      return false;
    }
  }
  return true;
}
static rw_dense_join_feasibility_t
dense_single_join_feasibility(const uint32_t *left_signatures, size_t left_len,
                              const uint32_t *right_signatures, size_t right_len,
                              const rw_signature_pool_t *pool, uint32_t nbits, size_t words,
                              uint64_t *dense_pair_count_out, qsop_error_t *error) {
  const size_t w = words == 0 ? 1U : words;
  uint64_t *scratch = calloc(w, sizeof(*scratch));
  if (scratch == NULL) {
    qsop_set_error(error, "out of memory while checking dense rankwidth single-mode feasibility");
    return RW_DENSE_JOIN_ERROR;
  }

  rw_dense_basis_t left_basis = {0};
  rw_dense_basis_t right_basis = {0};
  rw_dense_join_feasibility_t status = RW_DENSE_JOIN_FEASIBLE;
  if (!rw_dense_basis_init(&left_basis, nbits, pool->words, error) ||
      !rw_dense_basis_init(&right_basis, nbits, pool->words, error)) {
    status = RW_DENSE_JOIN_ERROR;
    goto cleanup;
  }
  for (size_t i = 0; i < left_len; i++) {
    if (!rw_dense_basis_add(&left_basis, rw_signature_bits(pool, left_signatures[i]), scratch,
                            NULL)) {
      status = RW_DENSE_JOIN_TOO_LARGE;
      goto cleanup;
    }
  }
  for (size_t i = 0; i < right_len; i++) {
    if (!rw_dense_basis_add(&right_basis, rw_signature_bits(pool, right_signatures[i]), scratch,
                            NULL)) {
      status = RW_DENSE_JOIN_TOO_LARGE;
      goto cleanup;
    }
  }

  size_t left_value_count = 0;
  size_t right_value_count = 0;
  if (!rw_dense_reference_value_count(left_basis.dim, 1U, &left_value_count, NULL) ||
      !rw_dense_reference_value_count(right_basis.dim, 1U, &right_value_count, NULL) ||
      !rw_dense_single_pair_count(left_value_count, right_value_count, dense_pair_count_out,
                                  NULL)) {
    status = RW_DENSE_JOIN_TOO_LARGE;
  }

cleanup:
  rw_dense_basis_free(&left_basis);
  rw_dense_basis_free(&right_basis);
  free(scratch);
  return status;
}
static bool solve_join_complex_streaming(const qsop_instance_t *qsop, const uint64_t *adj,
                                         rw_signature_pool_t *pool, const rw_complex_table_t *left,
                                         const rw_complex_table_t *right, rw_complex_context_t *ctx,
                                         rw_complex_table_t *out, const uint64_t *outside,
                                         uint64_t *scratch_sig, uint64_t *parent_assignment,
                                         size_t words, qsop_error_t *error) {
  /* r is only forwarded to rw_compute_join_transition_sign's residue_shift computation, which
   * this call site does not consume (it uses eval.sign_flip instead, computed from parity
   * alone with no mod-r reduction) -- so truncating it here cannot affect correctness even
   * when ctx->r exceeds UINT32_MAX. */
  const uint32_t r = (uint32_t)ctx->r;
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  for (size_t i = 0; i < left->len; i++) {
    const uint64_t *left_rep = rw_complex_assignment(left, i, words);
    const long double left_re = left->re[i];
    const long double left_im = left->im[i];
    for (size_t j = 0; j < right->len; j++) {
      const uint64_t *right_rep = rw_complex_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[i], left_rep,
              left->assignment_weights[i], right->signatures[j], right_rep,
              right->assignment_weights[j], scratch_sig, &eval, error)) {
        return false;
      }
      if (!eval.valid) {
        continue;
      }

      size_t out_index = 0;
      if (!complex_table_find_signature(out, eval.parent_signature, &out_index)) {
        qsop_bitset_copy(parent_assignment, left_rep, words);
        qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
        if (!complex_table_signature_index(out, eval.parent_signature, parent_assignment, words,
                                           &out_index, error)) {
          return false;
        }
      }
      const long double right_re = right->re[j];
      const long double right_im = right->im[j];
      long double product_re = left_re * right_re - left_im * right_im;
      long double product_im = left_re * right_im + left_im * right_re;
      if (eval.sign_flip) {
        product_re *= ctx->sign_re;
        product_im *= ctx->sign_re;
      }
      out->re[out_index] += product_re;
      out->im[out_index] += product_im;
      ctx->complex_ops++;
    }
  }
  return true;
}
static bool solve_join_complex64_streaming(
    const qsop_instance_t *qsop, const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_complex64_table_t *left, const rw_complex64_table_t *right,
    rw_complex64_context_t *ctx, rw_complex64_table_t *out, const uint64_t *outside,
    uint64_t *scratch_sig, uint64_t *parent_assignment, size_t words, qsop_error_t *error) {
  const uint32_t r = (uint32_t)ctx->r;
  const qsop_simd_vtable_t *simd = ctx->simd;
  uint64_t scalar_ops = 0;
  for (size_t i = 0; i < left->len; i++) {
    const uint64_t *left_rep = rw_complex64_assignment(left, i, words);
    const double left_re = left->re[i];
    const double left_im = left->im[i];
    for (size_t j = 0; j < right->len; j++) {
      const uint64_t *right_rep = rw_complex64_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[i], left_rep,
              left->assignment_weights[i], right->signatures[j], right_rep,
              right->assignment_weights[j], scratch_sig, &eval, error)) {
        return false;
      }
      if (!eval.valid) {
        continue;
      }

      size_t out_index = 0;
      if (!complex64_table_find_signature(out, eval.parent_signature, &out_index)) {
        qsop_bitset_copy(parent_assignment, left_rep, words);
        qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
        if (!complex64_table_signature_index(out, eval.parent_signature, parent_assignment, words,
                                             &out_index, error)) {
          return false;
        }
      }
      const double right_re = right->re[j];
      const double right_im = right->im[j];
      double product_re = left_re * right_re - left_im * right_im;
      double product_im = left_re * right_im + left_im * right_re;
      if (eval.sign_flip) {
        product_re *= ctx->sign_re;
        product_im *= ctx->sign_re;
      }
      out->re[out_index] += product_re;
      out->im[out_index] += product_im;
      ctx->complex_ops++;
      scalar_ops++;
    }
  }
  note_rankwidth_f64_scalar_fallback(ctx, scalar_ops);
  return true;
}
static bool solve_join_complex_dense_reference(
    const qsop_instance_t *qsop, const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_complex_table_t *left, const rw_complex_table_t *right, rw_complex_context_t *ctx,
    rw_complex_table_t *out, const uint64_t *outside, uint64_t *scratch_sig,
    uint64_t *parent_assignment, size_t words, qsop_error_t *error) {
  if (left->len > UINT32_MAX || right->len > UINT32_MAX) {
    qsop_set_error(error, "rankwidth dense single-mode table is too large");
    return false;
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();

  const size_t w = words == 0 ? 1U : words;
  uint64_t *basis_scratch = calloc(w, sizeof(*basis_scratch));
  if (basis_scratch == NULL) {
    qsop_set_error(error, "out of memory while allocating dense rankwidth single-mode scratch");
    return false;
  }

  bool ok = false;
  rw_dense_basis_t left_basis = {0};
  rw_dense_basis_t right_basis = {0};
  long double *left_dense_re = NULL;
  long double *left_dense_im = NULL;
  long double *right_dense_re = NULL;
  long double *right_dense_im = NULL;
  uint32_t *left_index = NULL;
  uint32_t *right_index = NULL;

  if (!dense_basis_from_complex_table(left, pool, qsop->nvars, &left_basis, basis_scratch, error) ||
      !dense_basis_from_complex_table(right, pool, qsop->nvars, &right_basis, basis_scratch,
                                      error)) {
    goto cleanup;
  }

  size_t left_value_count = 0;
  size_t right_value_count = 0;
  if (!rw_dense_reference_value_count(left_basis.dim, 1U, &left_value_count, error) ||
      !rw_dense_reference_value_count(right_basis.dim, 1U, &right_value_count, error)) {
    goto cleanup;
  }
  const size_t left_signatures = (size_t)1U << left_basis.dim;
  const size_t right_signatures = (size_t)1U << right_basis.dim;
  uint64_t dense_pairs = 0;
  if (!rw_dense_single_pair_count(left_signatures, right_signatures, &dense_pairs, error)) {
    goto cleanup;
  }
  (void)dense_pairs;

  left_dense_re = calloc(left_value_count == 0 ? 1U : left_value_count, sizeof(*left_dense_re));
  left_dense_im = calloc(left_value_count == 0 ? 1U : left_value_count, sizeof(*left_dense_im));
  right_dense_re = calloc(right_value_count == 0 ? 1U : right_value_count, sizeof(*right_dense_re));
  right_dense_im = calloc(right_value_count == 0 ? 1U : right_value_count, sizeof(*right_dense_im));
  left_index = malloc(left_signatures * sizeof(*left_index));
  right_index = malloc(right_signatures * sizeof(*right_index));
  if (left_dense_re == NULL || left_dense_im == NULL || right_dense_re == NULL ||
      right_dense_im == NULL || left_index == NULL || right_index == NULL) {
    qsop_set_error(error, "out of memory while allocating dense rankwidth single-mode tables");
    goto cleanup;
  }
  memset(left_index, 0xFF, left_signatures * sizeof(*left_index));
  memset(right_index, 0xFF, right_signatures * sizeof(*right_index));

  for (size_t i = 0; i < left->len; i++) {
    uint64_t coord = 0;
    if (!rw_dense_basis_coord(&left_basis, rw_signature_bits(pool, left->signatures[i]),
                              basis_scratch, &coord, error)) {
      goto cleanup;
    }
    left_index[(size_t)coord] = (uint32_t)i;
    left_dense_re[(size_t)coord] = left->re[i];
    left_dense_im[(size_t)coord] = left->im[i];
  }
  for (size_t i = 0; i < right->len; i++) {
    uint64_t coord = 0;
    if (!rw_dense_basis_coord(&right_basis, rw_signature_bits(pool, right->signatures[i]),
                              basis_scratch, &coord, error)) {
      goto cleanup;
    }
    right_index[(size_t)coord] = (uint32_t)i;
    right_dense_re[(size_t)coord] = right->re[i];
    right_dense_im[(size_t)coord] = right->im[i];
  }

  const uint32_t r = (uint32_t)ctx->r;
  for (size_t lc = 0; lc < left_signatures; lc++) {
    const uint32_t li = left_index[lc];
    if (li == UINT32_MAX) {
      continue;
    }
    const long double left_re = left_dense_re[lc];
    const long double left_im = left_dense_im[lc];
    if (left_re == 0.0L && left_im == 0.0L) {
      continue;
    }
    const uint64_t *left_rep = rw_complex_assignment(left, li, words);
    for (size_t rc = 0; rc < right_signatures; rc++) {
      const uint32_t ri = right_index[rc];
      if (ri == UINT32_MAX) {
        continue;
      }
      const long double right_re = right_dense_re[rc];
      const long double right_im = right_dense_im[rc];
      if (right_re == 0.0L && right_im == 0.0L) {
        continue;
      }
      const uint64_t *right_rep = rw_complex_assignment(right, ri, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[li], left_rep,
              left->assignment_weights[li], right->signatures[ri], right_rep,
              right->assignment_weights[ri], scratch_sig, &eval, error)) {
        goto cleanup;
      }
      if (!eval.valid) {
        continue;
      }

      size_t out_index = 0;
      if (!complex_table_find_signature(out, eval.parent_signature, &out_index)) {
        qsop_bitset_copy(parent_assignment, left_rep, words);
        qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
        if (!complex_table_signature_index(out, eval.parent_signature, parent_assignment, words,
                                           &out_index, error)) {
          goto cleanup;
        }
      }

      long double product_re = left_re * right_re - left_im * right_im;
      long double product_im = left_re * right_im + left_im * right_re;
      if (eval.sign_flip) {
        product_re *= ctx->sign_re;
        product_im *= ctx->sign_re;
      }
      out->re[out_index] += product_re;
      out->im[out_index] += product_im;
      ctx->complex_ops++;
    }
  }

  ok = true;

cleanup:
  free(left_dense_re);
  free(left_dense_im);
  free(right_dense_re);
  free(right_dense_im);
  free(left_index);
  free(right_index);
  rw_dense_basis_free(&left_basis);
  rw_dense_basis_free(&right_basis);
  free(basis_scratch);
  return ok;
}
static bool solve_join_complex64_dense_reference(
    const qsop_instance_t *qsop, const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_complex64_table_t *left, const rw_complex64_table_t *right,
    rw_complex64_context_t *ctx, rw_complex64_table_t *out, const uint64_t *outside,
    uint64_t *scratch_sig, uint64_t *parent_assignment, size_t words, qsop_error_t *error) {
  if (left->len > UINT32_MAX || right->len > UINT32_MAX) {
    qsop_set_error(error, "rankwidth dense double single-mode table is too large");
    return false;
  }
  const qsop_simd_vtable_t *simd = ctx->simd;

  const size_t w = words == 0 ? 1U : words;
  uint64_t *basis_scratch = calloc(w, sizeof(*basis_scratch));
  if (basis_scratch == NULL) {
    qsop_set_error(error,
                   "out of memory while allocating dense rankwidth double single-mode scratch");
    return false;
  }

  bool ok = false;
  rw_dense_basis_t left_basis = {0};
  rw_dense_basis_t right_basis = {0};
  double *left_dense_re = NULL;
  double *left_dense_im = NULL;
  double *right_dense_re = NULL;
  double *right_dense_im = NULL;
  uint32_t *left_index = NULL;
  uint32_t *right_index = NULL;

  if (!dense_basis_from_complex64_table(left, pool, qsop->nvars, &left_basis, basis_scratch,
                                        error) ||
      !dense_basis_from_complex64_table(right, pool, qsop->nvars, &right_basis, basis_scratch,
                                        error)) {
    goto cleanup;
  }

  size_t left_value_count = 0;
  size_t right_value_count = 0;
  if (!rw_dense_reference_value_count(left_basis.dim, 1U, &left_value_count, error) ||
      !rw_dense_reference_value_count(right_basis.dim, 1U, &right_value_count, error)) {
    goto cleanup;
  }
  const size_t left_signatures = (size_t)1U << left_basis.dim;
  const size_t right_signatures = (size_t)1U << right_basis.dim;
  uint64_t dense_pairs = 0;
  if (!rw_dense_single_pair_count(left_signatures, right_signatures, &dense_pairs, error)) {
    goto cleanup;
  }
  (void)dense_pairs;

  left_dense_re = calloc(left_value_count == 0 ? 1U : left_value_count, sizeof(*left_dense_re));
  left_dense_im = calloc(left_value_count == 0 ? 1U : left_value_count, sizeof(*left_dense_im));
  right_dense_re = calloc(right_value_count == 0 ? 1U : right_value_count, sizeof(*right_dense_re));
  right_dense_im = calloc(right_value_count == 0 ? 1U : right_value_count, sizeof(*right_dense_im));
  left_index = malloc(left_signatures * sizeof(*left_index));
  right_index = malloc(right_signatures * sizeof(*right_index));
  if (left_dense_re == NULL || left_dense_im == NULL || right_dense_re == NULL ||
      right_dense_im == NULL || left_index == NULL || right_index == NULL) {
    qsop_set_error(error,
                   "out of memory while allocating dense rankwidth double single-mode tables");
    goto cleanup;
  }
  memset(left_index, 0xFF, left_signatures * sizeof(*left_index));
  memset(right_index, 0xFF, right_signatures * sizeof(*right_index));

  for (size_t i = 0; i < left->len; i++) {
    uint64_t coord = 0;
    if (!rw_dense_basis_coord(&left_basis, rw_signature_bits(pool, left->signatures[i]),
                              basis_scratch, &coord, error)) {
      goto cleanup;
    }
    left_index[(size_t)coord] = (uint32_t)i;
    left_dense_re[(size_t)coord] = left->re[i];
    left_dense_im[(size_t)coord] = left->im[i];
  }
  for (size_t i = 0; i < right->len; i++) {
    uint64_t coord = 0;
    if (!rw_dense_basis_coord(&right_basis, rw_signature_bits(pool, right->signatures[i]),
                              basis_scratch, &coord, error)) {
      goto cleanup;
    }
    right_index[(size_t)coord] = (uint32_t)i;
    right_dense_re[(size_t)coord] = right->re[i];
    right_dense_im[(size_t)coord] = right->im[i];
  }

  const uint32_t r = (uint32_t)ctx->r;
  uint64_t scalar_ops = 0;
  for (size_t lc = 0; lc < left_signatures; lc++) {
    const uint32_t li = left_index[lc];
    if (li == UINT32_MAX) {
      continue;
    }
    const double left_re = left_dense_re[lc];
    const double left_im = left_dense_im[lc];
    if (left_re == 0.0 && left_im == 0.0) {
      continue;
    }
    const uint64_t *left_rep = rw_complex64_assignment(left, li, words);
    for (size_t rc = 0; rc < right_signatures; rc++) {
      const uint32_t ri = right_index[rc];
      if (ri == UINT32_MAX) {
        continue;
      }
      const double right_re = right_dense_re[rc];
      const double right_im = right_dense_im[rc];
      if (right_re == 0.0 && right_im == 0.0) {
        continue;
      }
      const uint64_t *right_rep = rw_complex64_assignment(right, ri, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[li], left_rep,
              left->assignment_weights[li], right->signatures[ri], right_rep,
              right->assignment_weights[ri], scratch_sig, &eval, error)) {
        goto cleanup;
      }
      if (!eval.valid) {
        continue;
      }

      size_t out_index = 0;
      if (!complex64_table_find_signature(out, eval.parent_signature, &out_index)) {
        qsop_bitset_copy(parent_assignment, left_rep, words);
        qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
        if (!complex64_table_signature_index(out, eval.parent_signature, parent_assignment, words,
                                             &out_index, error)) {
          goto cleanup;
        }
      }

      double product_re = left_re * right_re - left_im * right_im;
      double product_im = left_re * right_im + left_im * right_re;
      if (eval.sign_flip) {
        product_re *= ctx->sign_re;
        product_im *= ctx->sign_re;
      }
      out->re[out_index] += product_re;
      out->im[out_index] += product_im;
      ctx->complex_ops++;
      scalar_ops++;
    }
  }

  /* Dense coordinates make the right-side inputs contiguous, but parent signatures
   * are still scattered, so the current contiguous f64 SIMD vtable cannot accumulate
   * this join without a separate scatter/gather kernel. */
  note_rankwidth_f64_scalar_fallback(ctx, scalar_ops);
  ok = true;

cleanup:
  free(left_dense_re);
  free(left_dense_im);
  free(right_dense_re);
  free(right_dense_im);
  free(left_index);
  free(right_index);
  rw_dense_basis_free(&left_basis);
  rw_dense_basis_free(&right_basis);
  free(basis_scratch);
  return ok;
}
static bool rw_complex_transition_csr_build(
    const qsop_instance_t *qsop, const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_complex_table_t *left, const rw_complex_table_t *right, rw_complex_table_t *out,
    const uint64_t *outside, uint64_t *scratch_sig, uint64_t *parent_assignment, size_t words,
    rw_complex_transition_csr_t *csr, qsop_error_t *error) {
  if (left->len > UINT32_MAX || right->len > UINT32_MAX) {
    qsop_set_error(error, "rankwidth single-mode materialized join table is too large");
    return false;
  }
  const uint32_t r = (uint32_t)qsop->r;
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  uint32_t *counts = calloc(left->len == 0 ? 1U : left->len, sizeof(*counts));
  if (counts == NULL) {
    qsop_set_error(error, "out of memory while building rankwidth single-mode transition counts");
    return false;
  }

  uint64_t total = 0;
  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    const uint64_t *left_rep = rw_complex_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)right->len; j++) {
      const uint64_t *right_rep = rw_complex_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[i], left_rep,
              left->assignment_weights[i], right->signatures[j], right_rep,
              right->assignment_weights[j], scratch_sig, &eval, error)) {
        free(counts);
        return false;
      }
      if (!eval.valid) {
        continue;
      }
      qsop_bitset_copy(parent_assignment, left_rep, words);
      qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
      size_t out_index = 0;
      if (!complex_table_signature_index(out, eval.parent_signature, parent_assignment, words,
                                         &out_index, error)) {
        free(counts);
        return false;
      }
      (void)out_index;
      counts[i]++;
      total++;
      if (total > UINT32_MAX) {
        free(counts);
        qsop_set_error(error, "rankwidth single-mode materialized join has too many transitions");
        return false;
      }
    }
  }

  uint32_t *offsets = malloc((left->len + 1U) * sizeof(*offsets));
  rw_complex_transition32_t *items = total == 0 ? NULL : malloc((size_t)total * sizeof(*items));
  if (offsets == NULL || (total != 0 && items == NULL)) {
    free(counts);
    free(offsets);
    free(items);
    qsop_set_error(error, "out of memory while building rankwidth single-mode transitions");
    return false;
  }
  offsets[0] = 0;
  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    offsets[i + 1U] = offsets[i] + counts[i];
  }
  memcpy(counts, offsets, left->len * sizeof(*counts));

  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    const uint64_t *left_rep = rw_complex_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)right->len; j++) {
      const uint64_t *right_rep = rw_complex_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[i], left_rep,
              left->assignment_weights[i], right->signatures[j], right_rep,
              right->assignment_weights[j], scratch_sig, &eval, error)) {
        free(counts);
        free(offsets);
        free(items);
        return false;
      }
      if (!eval.valid) {
        continue;
      }
      size_t parent_index = 0;
      if (!complex_table_find_signature(out, eval.parent_signature, &parent_index)) {
        free(counts);
        free(offsets);
        free(items);
        qsop_set_error(error, "internal error: missing rankwidth single-mode parent signature");
        return false;
      }
      const uint32_t pos = counts[i]++;
      items[pos] = (rw_complex_transition32_t){
          .right_index = j,
          .parent_index = (uint32_t)parent_index,
          .flags = eval.sign_flip ? 1U : 0U,
      };
    }
  }

  free(counts);
  *csr = (rw_complex_transition_csr_t){
      .left_count = (uint32_t)left->len,
      .offsets = offsets,
      .items = items,
      .transition_count = total,
  };
  return true;
}
static void rw_execute_complex_transition_csr(const rw_complex_transition_csr_t *csr,
                                              const rw_complex_table_t *left,
                                              const rw_complex_table_t *right,
                                              rw_complex_context_t *ctx, rw_complex_table_t *out) {
  for (uint32_t i = 0; i < csr->left_count; i++) {
    const long double left_re = left->re[i];
    const long double left_im = left->im[i];
    for (uint32_t p = csr->offsets[i]; p < csr->offsets[i + 1U]; p++) {
      const rw_complex_transition32_t *t = &csr->items[p];
      const uint32_t j = t->right_index;
      long double product_re = left_re * right->re[j] - left_im * right->im[j];
      long double product_im = left_re * right->im[j] + left_im * right->re[j];
      if ((t->flags & 1U) != 0) {
        product_re *= ctx->sign_re;
        product_im *= ctx->sign_re;
      }
      out->re[t->parent_index] += product_re;
      out->im[t->parent_index] += product_im;
      ctx->complex_ops++;
    }
  }
}
static bool rw_complex64_transition_csr_build(
    const qsop_instance_t *qsop, const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_complex64_table_t *left, const rw_complex64_table_t *right, rw_complex64_table_t *out,
    const uint64_t *outside, uint64_t *scratch_sig, uint64_t *parent_assignment, size_t words,
    rw_complex_transition_csr_t *csr, qsop_error_t *error) {
  if (left->len > UINT32_MAX || right->len > UINT32_MAX) {
    qsop_set_error(error, "rankwidth double single-mode materialized join table is too large");
    return false;
  }
  const uint32_t r = (uint32_t)qsop->r;
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  uint32_t *counts = calloc(left->len == 0 ? 1U : left->len, sizeof(*counts));
  if (counts == NULL) {
    qsop_set_error(error,
                   "out of memory while building rankwidth double single-mode transition counts");
    return false;
  }

  uint64_t total = 0;
  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    const uint64_t *left_rep = rw_complex64_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)right->len; j++) {
      const uint64_t *right_rep = rw_complex64_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[i], left_rep,
              left->assignment_weights[i], right->signatures[j], right_rep,
              right->assignment_weights[j], scratch_sig, &eval, error)) {
        free(counts);
        return false;
      }
      if (!eval.valid) {
        continue;
      }
      qsop_bitset_copy(parent_assignment, left_rep, words);
      qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
      size_t out_index = 0;
      if (!complex64_table_signature_index(out, eval.parent_signature, parent_assignment, words,
                                           &out_index, error)) {
        free(counts);
        return false;
      }
      (void)out_index;
      counts[i]++;
      total++;
      if (total > UINT32_MAX) {
        free(counts);
        qsop_set_error(error,
                       "rankwidth double single-mode materialized join has too many transitions");
        return false;
      }
    }
  }

  uint32_t *offsets = malloc((left->len + 1U) * sizeof(*offsets));
  rw_complex_transition32_t *items = total == 0 ? NULL : malloc((size_t)total * sizeof(*items));
  if (offsets == NULL || (total != 0 && items == NULL)) {
    free(counts);
    free(offsets);
    free(items);
    qsop_set_error(error, "out of memory while building rankwidth double single-mode transitions");
    return false;
  }
  offsets[0] = 0;
  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    offsets[i + 1U] = offsets[i] + counts[i];
  }
  /* Two cursors per row, growing toward each other: flag=0 items fill from the front, flag=1
   * items fill from the back. Each row ends up stably partitioned (all flag=0 before all
   * flag=1, though the flag=1 sub-run lands in reverse right_index order) without needing to
   * know each row's flag=1 count ahead of time -- rw_complex64_execute_row below relies on this
   * partition to scale each sign class with one complex_scale_f64 call instead of a branch per
   * item. */
  uint32_t *back = malloc((left->len == 0 ? 1U : left->len) * sizeof(*back));
  if (back == NULL) {
    free(counts);
    free(offsets);
    free(items);
    qsop_set_error(error, "out of memory while building rankwidth double single-mode transitions");
    return false;
  }
  memcpy(counts, offsets, left->len * sizeof(*counts));
  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    back[i] = offsets[i + 1U] - 1U;
  }

  for (uint32_t i = 0; i < (uint32_t)left->len; i++) {
    const uint64_t *left_rep = rw_complex64_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)right->len; j++) {
      const uint64_t *right_rep = rw_complex64_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, r, left->signatures[i], left_rep,
              left->assignment_weights[i], right->signatures[j], right_rep,
              right->assignment_weights[j], scratch_sig, &eval, error)) {
        free(counts);
        free(offsets);
        free(items);
        free(back);
        return false;
      }
      if (!eval.valid) {
        continue;
      }
      size_t parent_index = 0;
      if (!complex64_table_find_signature(out, eval.parent_signature, &parent_index)) {
        free(counts);
        free(offsets);
        free(items);
        free(back);
        qsop_set_error(error,
                       "internal error: missing rankwidth double single-mode parent signature");
        return false;
      }
      const uint32_t pos = eval.sign_flip ? back[i]-- : counts[i]++;
      items[pos] = (rw_complex_transition32_t){
          .right_index = j,
          .parent_index = (uint32_t)parent_index,
          .flags = eval.sign_flip ? 1U : 0U,
      };
    }
  }

  free(counts);
  free(back);
  *csr = (rw_complex_transition_csr_t){
      .left_count = (uint32_t)left->len,
      .offsets = offsets,
      .items = items,
      .transition_count = total,
  };
  return true;
}
static void rw_complex64_execute_row(const rw_complex_transition32_t *items, uint32_t start,
                                     uint32_t end, double left_re, double left_im,
                                     const rw_complex64_table_t *right, rw_complex64_context_t *ctx,
                                     rw_complex64_table_t *out, double *gather_re,
                                     double *gather_im, double *product_re, double *product_im) {
  const qsop_simd_vtable_t *simd = ctx->simd;
  const uint32_t len = end - start;
  const bool have_scratch =
      gather_re != NULL && gather_im != NULL && product_re != NULL && product_im != NULL;
  if (!have_scratch || simd == NULL || simd->complex_scale_f64 == NULL || len < simd->min_lanes) {
    for (uint32_t p = start; p < end; p++) {
      const rw_complex_transition32_t *t = &items[p];
      const uint32_t j = t->right_index;
      double item_re = left_re * right->re[j] - left_im * right->im[j];
      double item_im = left_re * right->im[j] + left_im * right->re[j];
      if ((t->flags & 1U) != 0) {
        item_re *= ctx->sign_re;
        item_im *= ctx->sign_re;
      }
      out->re[t->parent_index] += item_re;
      out->im[t->parent_index] += item_im;
      ctx->complex_ops++;
    }
    note_rankwidth_f64_scalar_fallback(ctx, len);
    return;
  }

  uint32_t mid = start;
  while (mid < end && (items[mid].flags & 1U) == 0) {
    mid++;
  }
  const uint32_t bounds[3] = {start, mid, end};
  for (uint32_t part = 0; part < 2; part++) {
    const uint32_t pstart = bounds[part];
    const uint32_t pend = bounds[part + 1U];
    const uint32_t plen = pend - pstart;
    if (plen == 0) {
      continue;
    }
    for (uint32_t k = 0; k < plen; k++) {
      const uint32_t j = items[pstart + k].right_index;
      gather_re[k] = right->re[j];
      gather_im[k] = right->im[j];
    }
    const double scale_re = part == 0 ? left_re : left_re * ctx->sign_re;
    const double scale_im = part == 0 ? left_im : left_im * ctx->sign_re;
    simd->complex_scale_f64(product_re, product_im, gather_re, gather_im, scale_re, scale_im, plen);
    for (uint32_t k = 0; k < plen; k++) {
      const uint32_t parent_index = items[pstart + k].parent_index;
      out->re[parent_index] += product_re[k];
      out->im[parent_index] += product_im[k];
    }
    ctx->complex_ops += plen;
    /* Matches note_rankwidth_bitset_ops's convention: the scalar vtable's own complex_scale_f64
     * is a plain loop, not actually vectorized, even though it takes this same batched path. */
    if (ctx->stats != NULL && simd != qsop_simd_scalar_vtable()) {
      qsop_add_saturating_u64(&ctx->stats->simd_vectorized_ops, plen);
    }
  }
}
static void rw_execute_complex64_transition_csr(const rw_complex_transition_csr_t *csr,
                                                const rw_complex64_table_t *left,
                                                const rw_complex64_table_t *right,
                                                rw_complex64_context_t *ctx,
                                                rw_complex64_table_t *out) {
  const size_t max_row = right->len == 0 ? 1U : right->len;
  double *gather_re = malloc(max_row * sizeof(*gather_re));
  double *gather_im = malloc(max_row * sizeof(*gather_im));
  double *product_re = malloc(max_row * sizeof(*product_re));
  double *product_im = malloc(max_row * sizeof(*product_im));
  /* A failed allocation just makes rw_complex64_execute_row take its scalar fallback below --
   * degrades performance, never correctness. */
  for (uint32_t i = 0; i < csr->left_count; i++) {
    rw_complex64_execute_row(csr->items, csr->offsets[i], csr->offsets[i + 1U], left->re[i],
                             left->im[i], right, ctx, out, gather_re, gather_im, product_re,
                             product_im);
  }
  free(gather_re);
  free(gather_im);
  free(product_re);
  free(product_im);
}
void rw_solve_single_mode_no_edges(const qsop_instance_t *qsop, uint32_t target_mode,
                                   long double *out_re, long double *out_im,
                                   uint64_t *complex_ops) {
  long double acc_re = 1.0L;
  long double acc_im = 0.0L;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    long double term_re = 0.0L;
    long double term_im = 0.0L;
    qsop_root_of_unity_l(qsop->r, target_mode, qsop->unary[v] % qsop->r, &term_re, &term_im);
    term_re += 1.0L;
    const long double new_re = acc_re * term_re - acc_im * term_im;
    const long double new_im = acc_re * term_im + acc_im * term_re;
    acc_re = new_re;
    acc_im = new_im;
    (*complex_ops)++;
  }
  *out_re = acc_re;
  *out_im = acc_im;
}
void rw_solve_single_mode_no_edges_f64(const qsop_instance_t *qsop, uint32_t target_mode,
                                       double *out_re, double *out_im, uint64_t *complex_ops) {
  double acc_re = 1.0;
  double acc_im = 0.0;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    double term_re = 0.0;
    double term_im = 0.0;
    qsop_root_of_unity_f64(qsop->r, target_mode, qsop->unary[v] % qsop->r, &term_re, &term_im);
    term_re += 1.0;
    const double new_re = acc_re * term_re - acc_im * term_im;
    const double new_im = acc_re * term_im + acc_im * term_re;
    acc_re = new_re;
    acc_im = new_im;
    (*complex_ops)++;
  }
  *out_re = acc_re;
  *out_im = acc_im;
}
bool rw_solve_single_mode_once(const qsop_instance_t *qsop,
                               const qsop_rankwidth_decomposition_t *decomposition,
                               const uint64_t *adj, uint32_t target_mode, int *out_scale_exp2,
                               long double *out_re, long double *out_im,
                               long double *out_numeric_error_bound,
                               qsop_rankwidth_single_kernel_t kernel,
                               uint64_t materialize_join_max_pairs, qsop_solve_stats_t *stats,
                               qsop_solve_trace_t *trace, qsop_error_t *error) {
  rw_complex_context_t ctx = {
      .r = qsop->r,
      .target_mode = target_mode,
      .sign_re = (target_mode % 2U == 0U) ? 1.0L : -1.0L,
      .stats = stats,
      .trace = trace,
  };
  if (stats != NULL) {
    stats->rankwidth_single_complex_kernel = 1U;
  }

  if (qsop->nedges == 0) {
    long double re = 0.0L;
    long double im = 0.0L;
    rw_solve_single_mode_no_edges(qsop, target_mode, &re, &im, &ctx.complex_ops);
    long double c_re = 0.0L;
    long double c_im = 0.0L;
    qsop_root_of_unity_l(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
    *out_re = re * c_re - im * c_im;
    *out_im = re * c_im + im * c_re;
    *out_scale_exp2 = 0;
    if (out_numeric_error_bound != NULL) {
      *out_numeric_error_bound = qsop_single_mode_error_bound_l(ctx.complex_ops);
    }
    return true;
  }

  rw_complex_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (tables == NULL) {
    qsop_set_error(error, "out of memory while allocating rankwidth single-mode solve state");
    return false;
  }

  rw_signature_pool_t pool = {0};
  if (!rw_signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    return false;
  }

  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  uint64_t *scratch = calloc(6U * w, sizeof(*scratch));
  if (scratch == NULL) {
    free(tables);
    rw_signature_pool_free(&pool);
    qsop_set_error(error, "out of memory while allocating rankwidth single-mode scratch");
    return false;
  }
  uint64_t *outside = scratch;
  uint64_t *scratch_sig = scratch + w;
  uint64_t *parent_assignment = scratch + 2U * w;
  uint64_t *leaf_zero = scratch + 3U * w;
  uint64_t *leaf_one = scratch + 4U * w;
  uint64_t *leaf_signature = scratch + 5U * w;

  uint64_t signature_entries = 0;
  uint64_t max_signature_entries = 0;
  uint64_t transition_bytes = 0;
  uint64_t dense_join_events = 0;
  uint64_t materialized_join_events = 0;
  uint64_t streaming_join_events = 0;
  const uint64_t max_pairs = materialize_join_max_pairs > 0 ? materialize_join_max_pairs
                                                            : RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT;

  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_complex(qsop, adj, node, target_mode, decomposition->words, &pool,
                              &tables[node_id], leaf_zero, leaf_one, leaf_signature, error);
      qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_leaf", 0, tables[node_id].len, start);
    } else {
      rw_fill_all_vars(outside, decomposition->nvars, decomposition->words);
      qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), decomposition->words);
      const uint64_t join_start = qsop_trace_begin(trace);
      const size_t left_len = tables[node->left].len;
      const size_t right_len = tables[node->right].len;
      if (left_len > 0 && right_len > UINT64_MAX / left_len) {
        qsop_set_error(error, "rankwidth single-mode join is too large");
        ok = false;
      } else {
        const uint64_t pair_forecast = (uint64_t)left_len * (uint64_t)right_len;
        bool use_dense = kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_DENSE;
        bool dense_preflight_failed = false;
        if (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO) {
          uint64_t dense_pair_count = 0;
          const rw_dense_join_feasibility_t dense_status = dense_single_join_feasibility(
              tables[node->left].signatures, left_len, tables[node->right].signatures, right_len,
              &pool, qsop->nvars, decomposition->words, &dense_pair_count, error);
          if (dense_status == RW_DENSE_JOIN_ERROR) {
            dense_preflight_failed = true;
            ok = false;
          } else {
            use_dense = dense_status == RW_DENSE_JOIN_FEASIBLE;
          }
          (void)dense_pair_count;
        }
        const bool use_materialized =
            !use_dense &&
            (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED ||
             (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO && pair_forecast <= max_pairs));
        if (dense_preflight_failed) {
          ok = false;
        } else if (use_dense) {
          ok = solve_join_complex_dense_reference(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], &ctx, &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, error);
          if (ok) {
            dense_join_events++;
            qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_join_dense", 0,
                                    tables[node_id].len, join_start);
          }
        } else if (use_materialized) {
          rw_complex_transition_csr_t csr = {0};
          ok = rw_complex_transition_csr_build(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, &csr, error);
          if (ok) {
            transition_bytes += rw_complex_transition_csr_bytes(&csr);
            materialized_join_events++;
            qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_join_map", 0,
                                    csr.transition_count, join_start);
            rw_execute_complex_transition_csr(&csr, &tables[node->left], &tables[node->right], &ctx,
                                              &tables[node_id]);
          }
          rw_complex_transition_csr_free(&csr);
        } else {
          ok = solve_join_complex_streaming(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], &ctx, &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, error);
          if (ok) {
            streaming_join_events++;
          }
        }
      }
      qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_join", 0, tables[node_id].len,
                              join_start);
      if (ok) {
        rw_complex_table_free(&tables[node->left]);
        if (node->right != node->left) {
          rw_complex_table_free(&tables[node->right]);
        }
      }
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_complex_table_free(&tables[t]);
      }
      free(scratch);
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
    complex_table_renormalize(&tables[node_id], &ctx);
    signature_entries += tables[node_id].len;
    if (tables[node_id].len > max_signature_entries) {
      max_signature_entries = tables[node_id].len;
    }
  }

  const rw_complex_table_t *root_table = &tables[decomposition->root];
  long double root_re = 0.0L;
  long double root_im = 0.0L;
  size_t root_index = 0;
  if (complex_table_find_signature(root_table, 0, &root_index)) {
    root_re = root_table->re[root_index];
    root_im = root_table->im[root_index];
  }

  long double c_re = 0.0L;
  long double c_im = 0.0L;
  qsop_root_of_unity_l(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
  *out_re = root_re * c_re - root_im * c_im;
  *out_im = root_re * c_im + root_im * c_re;
  *out_scale_exp2 = ctx.scale_exp2;

  if (stats != NULL) {
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = ctx.complex_ops;
    stats->rankwidth_transition_bytes += transition_bytes;
    stats->rankwidth_dense_join_events += dense_join_events;
    stats->rankwidth_materialized_join_events += materialized_join_events;
    stats->rankwidth_streaming_join_events += streaming_join_events;
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_complex_table_free(&tables[t]);
      }
      free(scratch);
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
  }
  if (out_numeric_error_bound != NULL) {
    *out_numeric_error_bound = qsop_single_mode_error_bound_l(ctx.complex_ops);
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    rw_complex_table_free(&tables[t]);
  }
  free(scratch);
  free(tables);
  rw_signature_pool_free(&pool);
  return true;
}
bool rw_solve_single_mode_once_f64(const qsop_instance_t *qsop,
                                   const qsop_rankwidth_decomposition_t *decomposition,
                                   const uint64_t *adj, uint32_t target_mode, int *out_scale_exp2,
                                   long double *out_re, long double *out_im,
                                   long double *out_numeric_error_bound,
                                   qsop_rankwidth_single_kernel_t kernel,
                                   uint64_t materialize_join_max_pairs,
                                   const qsop_simd_vtable_t *simd, qsop_solve_stats_t *stats,
                                   qsop_solve_trace_t *trace, qsop_error_t *error) {
  rw_complex64_context_t ctx = {
      .r = qsop->r,
      .target_mode = target_mode,
      .sign_re = (target_mode % 2U == 0U) ? 1.0 : -1.0,
      .simd = simd,
      .stats = stats,
      .trace = trace,
  };

  if (qsop->nedges == 0) {
    double re = 0.0;
    double im = 0.0;
    rw_solve_single_mode_no_edges_f64(qsop, target_mode, &re, &im, &ctx.complex_ops);
    double c_re = 0.0;
    double c_im = 0.0;
    qsop_root_of_unity_f64(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
    /* The edge-free product is 2^nvars in the worst case; long double is enough headroom for the
     * few thousand independent variables this branch can see, and the exponent stays 0. */
    *out_re = (long double)re * (long double)c_re - (long double)im * (long double)c_im;
    *out_im = (long double)re * (long double)c_im + (long double)im * (long double)c_re;
    *out_scale_exp2 = 0;
    if (stats != NULL) {
      stats->rankwidth_single_complex_kernel = 2U;
      stats->join_pairs = ctx.complex_ops;
      record_rankwidth_f64_simd_kernel(stats, simd);
      qsop_add_saturating_u64(&stats->simd_scalar_fallback_ops, ctx.complex_ops);
    }
    if (out_numeric_error_bound != NULL) {
      *out_numeric_error_bound = qsop_single_mode_error_bound_f64(ctx.complex_ops);
    }
    return true;
  }

  rw_complex64_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (tables == NULL) {
    qsop_set_error(error,
                   "out of memory while allocating rankwidth double single-mode solve state");
    return false;
  }

  rw_signature_pool_t pool = {0};
  if (!rw_signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    return false;
  }

  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  uint64_t *scratch = calloc(6U * w, sizeof(*scratch));
  if (scratch == NULL) {
    free(tables);
    rw_signature_pool_free(&pool);
    qsop_set_error(error, "out of memory while allocating rankwidth double single-mode scratch");
    return false;
  }
  uint64_t *outside = scratch;
  uint64_t *scratch_sig = scratch + w;
  uint64_t *parent_assignment = scratch + 2U * w;
  uint64_t *leaf_zero = scratch + 3U * w;
  uint64_t *leaf_one = scratch + 4U * w;
  uint64_t *leaf_signature = scratch + 5U * w;

  uint64_t signature_entries = 0;
  uint64_t max_signature_entries = 0;
  uint64_t transition_bytes = 0;
  uint64_t dense_join_events = 0;
  uint64_t materialized_join_events = 0;
  uint64_t streaming_join_events = 0;
  const uint64_t max_pairs = materialize_join_max_pairs > 0 ? materialize_join_max_pairs
                                                            : RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT;

  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_complex64(qsop, adj, node, target_mode, decomposition->words, &pool,
                                &tables[node_id], leaf_zero, leaf_one, leaf_signature, error);
      qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_leaf_f64", 0, tables[node_id].len,
                              start);
    } else {
      rw_fill_all_vars(outside, decomposition->nvars, decomposition->words);
      qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), decomposition->words);
      const uint64_t join_start = qsop_trace_begin(trace);
      const size_t left_len = tables[node->left].len;
      const size_t right_len = tables[node->right].len;
      if (left_len > 0 && right_len > UINT64_MAX / left_len) {
        qsop_set_error(error, "rankwidth double single-mode join is too large");
        ok = false;
      } else {
        const uint64_t pair_forecast = (uint64_t)left_len * (uint64_t)right_len;
        bool use_dense = kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_DENSE;
        bool dense_preflight_failed = false;
        if (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO) {
          uint64_t dense_pair_count = 0;
          const rw_dense_join_feasibility_t dense_status = dense_single_join_feasibility(
              tables[node->left].signatures, left_len, tables[node->right].signatures, right_len,
              &pool, qsop->nvars, decomposition->words, &dense_pair_count, error);
          if (dense_status == RW_DENSE_JOIN_ERROR) {
            dense_preflight_failed = true;
            ok = false;
          } else {
            use_dense = dense_status == RW_DENSE_JOIN_FEASIBLE;
          }
          (void)dense_pair_count;
        }
        const bool use_materialized =
            !use_dense &&
            (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED ||
             (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO && pair_forecast <= max_pairs));
        if (dense_preflight_failed) {
          ok = false;
        } else if (use_dense) {
          ok = solve_join_complex64_dense_reference(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], &ctx, &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, error);
          if (ok) {
            dense_join_events++;
            qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_join_dense_f64", 0,
                                    tables[node_id].len, join_start);
          }
        } else if (use_materialized) {
          rw_complex_transition_csr_t csr = {0};
          ok = rw_complex64_transition_csr_build(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, &csr, error);
          if (ok) {
            transition_bytes += rw_complex_transition_csr_bytes(&csr);
            materialized_join_events++;
            qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_join_map_f64", 0,
                                    csr.transition_count, join_start);
            rw_execute_complex64_transition_csr(&csr, &tables[node->left], &tables[node->right],
                                                &ctx, &tables[node_id]);
          }
          rw_complex_transition_csr_free(&csr);
        } else {
          ok = solve_join_complex64_streaming(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], &ctx, &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, error);
          if (ok) {
            streaming_join_events++;
          }
        }
      }
      qsop_trace_emit_elapsed(trace, "rankwidth.single_mode_join_f64", 0, tables[node_id].len,
                              join_start);
      if (ok) {
        rw_complex64_table_free(&tables[node->left]);
        if (node->right != node->left) {
          rw_complex64_table_free(&tables[node->right]);
        }
      }
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_complex64_table_free(&tables[t]);
      }
      free(scratch);
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
    /* Every table below the current node has a peak in [1,2), so a join can only reach 2^2 times
     * the number of pairs it accumulates before this pulls the exponent back out. */
    complex64_table_renormalize(&tables[node_id], &ctx);
    signature_entries += tables[node_id].len;
    if (tables[node_id].len > max_signature_entries) {
      max_signature_entries = tables[node_id].len;
    }
  }

  const rw_complex64_table_t *root_table = &tables[decomposition->root];
  double root_re = 0.0;
  double root_im = 0.0;
  size_t root_index = 0;
  if (complex64_table_find_signature(root_table, 0, &root_index)) {
    root_re = root_table->re[root_index];
    root_im = root_table->im[root_index];
  }

  double c_re = 0.0;
  double c_im = 0.0;
  qsop_root_of_unity_f64(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
  *out_re = (long double)(root_re * c_re - root_im * c_im);
  *out_im = (long double)(root_re * c_im + root_im * c_re);
  *out_scale_exp2 = ctx.scale_exp2;

  if (stats != NULL) {
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = ctx.complex_ops;
    stats->rankwidth_single_complex_kernel = 2U;
    record_rankwidth_f64_simd_kernel(stats, ctx.simd);
    stats->rankwidth_transition_bytes += transition_bytes;
    stats->rankwidth_dense_join_events += dense_join_events;
    stats->rankwidth_materialized_join_events += materialized_join_events;
    stats->rankwidth_streaming_join_events += streaming_join_events;
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_complex64_table_free(&tables[t]);
      }
      free(scratch);
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
  }
  if (out_numeric_error_bound != NULL) {
    *out_numeric_error_bound = qsop_single_mode_error_bound_f64(ctx.complex_ops);
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    rw_complex64_table_free(&tables[t]);
  }
  free(scratch);
  free(tables);
  rw_signature_pool_free(&pool);
  return true;
}
