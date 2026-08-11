/* Per-join coordinate plans for the rankwidth DP pair loops (docs/table-representation-audit.md,
 * refactor steps 2-3).
 *
 * A plan is built once per join from the two child tables' realized signatures and turns the
 * per-pair full-width bitset work (crossing parity, parent-signature XOR/AND and intern) into a
 * handful of 64-bit word operations; see rw_join_plan_t in rankwidth_internal.h for the
 * coordinate identities.  Plans are best-effort: any realized span whose dimension exceeds the
 * dense-basis cap leaves the plan inactive and the callers keep the extrinsic per-pair path. */
#include "../core/qsop_internal.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/simd.h"
#include "rankwidth_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Callers hand the left/right signature ids over as bare uint32 arrays (&reps[0].signature for
 * rw_table_t); that is only sound while the rep struct carries nothing else. */
_Static_assert(sizeof(rw_signature_rep_t) == sizeof(uint32_t),
               "rw_signature_rep_t must stay a bare signature id");

static uint64_t fold_coord_columns(const uint64_t *columns, uint64_t coord) {
  uint64_t acc = 0;
  while (coord != 0) {
    acc ^= columns[rw_ctz_u64(coord)];
    coord &= coord - 1U;
  }
  return acc;
}
void rw_join_plan_free(rw_join_plan_t *plan) {
  if (plan == NULL) {
    return;
  }
  free(plan->lcoord);
  free(plan->rcoord);
  free(plan->lproj);
  free(plan->rproj);
  free(plan->mfold);
  free(plan->sig_memo);
  *plan = (rw_join_plan_t){0};
}
bool rw_join_plan_build(uint32_t nvars, const rw_signature_pool_t *pool, const uint32_t *left_sigs,
                        const uint64_t *left_assignments, size_t llen, const uint32_t *right_sigs,
                        const uint64_t *right_assignments, size_t rlen, const uint64_t *outside,
                        size_t words, rw_join_plan_t *plan, qsop_error_t *error) {
  /* The crossing form is evaluated as |left witness AND right signature| mod 2, so only the
   * left side's witness assignments are read; the right-side array stays in the signature so
   * callers need not know which half-product the implementation picks. */
  (void)right_assignments;
  *plan = (rw_join_plan_t){0};
  if (llen == 0 || rlen == 0 || words == 0) {
    return true; /* nothing to join (or no bitset lanes); stay inactive */
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  bool feasible = true;
  bool ok = false;
  rw_dense_basis_t lbasis = {0};
  rw_dense_basis_t rbasis = {0};
  rw_dense_basis_t pbasis = {0};
  uint64_t *scratch = calloc(words, sizeof(*scratch));
  uint64_t *proj = calloc(words, sizeof(*proj));
  uint32_t lgen[RW_DENSE_REFERENCE_MAX_DIM];
  uint32_t rgen[RW_DENSE_REFERENCE_MAX_DIM];
  uint64_t pleft[RW_DENSE_REFERENCE_MAX_DIM];
  uint64_t pright[RW_DENSE_REFERENCE_MAX_DIM];
  uint64_t brow[RW_DENSE_REFERENCE_MAX_DIM];
  plan->lcoord = calloc(llen, sizeof(*plan->lcoord));
  plan->lproj = calloc(llen, sizeof(*plan->lproj));
  plan->rcoord = calloc(rlen, sizeof(*plan->rcoord));
  plan->rproj = calloc(rlen, sizeof(*plan->rproj));
  plan->mfold = calloc(rlen, sizeof(*plan->mfold));
  if (scratch == NULL || proj == NULL || plan->lcoord == NULL || plan->lproj == NULL ||
      plan->rcoord == NULL || plan->rproj == NULL || plan->mfold == NULL) {
    qsop_set_error(error, "out of memory while building rankwidth join plan");
    goto cleanup;
  }
  if (!rw_dense_basis_init(&lbasis, nvars, words, error) ||
      !rw_dense_basis_init(&rbasis, nvars, words, error) ||
      !rw_dense_basis_init(&pbasis, nvars, words, error)) {
    goto cleanup;
  }

  /* Realized child bases; remember which representative introduced each generator so its
   * assignment can witness the crossing form below (any witness with the right signature gives
   * the same parity by lem:chi-well-defined). */
  for (size_t i = 0; i < llen && feasible; i++) {
    const uint32_t before = lbasis.dim;
    if (!rw_dense_basis_add(&lbasis, rw_signature_bits(pool, left_sigs[i]), scratch, NULL)) {
      feasible = false; /* realized span exceeds the dense-basis cap */
    } else if (lbasis.dim > before) {
      lgen[before] = (uint32_t)i;
    }
  }
  for (size_t j = 0; j < rlen && feasible; j++) {
    const uint32_t before = rbasis.dim;
    if (!rw_dense_basis_add(&rbasis, rw_signature_bits(pool, right_sigs[j]), scratch, NULL)) {
      feasible = false;
    } else if (rbasis.dim > before) {
      rgen[before] = (uint32_t)j;
    }
  }

  /* Parent basis spans every projected generator; the projected realized parent signatures all
   * live inside it because sigma -> sigma & outside is linear. */
  for (uint32_t u = 0; u < lbasis.dim && feasible; u++) {
    qsop_bitset_copy(proj, rw_signature_bits(pool, left_sigs[lgen[u]]), words);
    qsop_bitset_and(proj, outside, words);
    if (!rw_dense_basis_add(&pbasis, proj, scratch, NULL)) {
      feasible = false;
    }
  }
  for (uint32_t v = 0; v < rbasis.dim && feasible; v++) {
    qsop_bitset_copy(proj, rw_signature_bits(pool, right_sigs[rgen[v]]), words);
    qsop_bitset_and(proj, outside, words);
    if (!rw_dense_basis_add(&pbasis, proj, scratch, NULL)) {
      feasible = false;
    }
  }
  for (uint32_t u = 0; u < lbasis.dim && feasible; u++) {
    qsop_bitset_copy(proj, rw_signature_bits(pool, left_sigs[lgen[u]]), words);
    qsop_bitset_and(proj, outside, words);
    if (!rw_dense_basis_reduce(&pbasis, proj, scratch, &pleft[u])) {
      assert(0 && "projected left generator escapes the parent basis");
      feasible = false;
    }
  }
  for (uint32_t v = 0; v < rbasis.dim && feasible; v++) {
    qsop_bitset_copy(proj, rw_signature_bits(pool, right_sigs[rgen[v]]), words);
    qsop_bitset_and(proj, outside, words);
    if (!rw_dense_basis_reduce(&pbasis, proj, scratch, &pright[v])) {
      assert(0 && "projected right generator escapes the parent basis");
      feasible = false;
    }
  }

  /* Per-representative coordinates and their parent projections. */
  for (size_t i = 0; i < llen && feasible; i++) {
    if (!rw_dense_basis_reduce(&lbasis, rw_signature_bits(pool, left_sigs[i]), scratch,
                               &plan->lcoord[i])) {
      assert(0 && "left signature escapes its own realized basis");
      feasible = false;
    } else {
      plan->lproj[i] = fold_coord_columns(pleft, plan->lcoord[i]);
    }
  }
  for (size_t j = 0; j < rlen && feasible; j++) {
    if (!rw_dense_basis_reduce(&rbasis, rw_signature_bits(pool, right_sigs[j]), scratch,
                               &plan->rcoord[j])) {
      assert(0 && "right signature escapes its own realized basis");
      feasible = false;
    } else {
      plan->rproj[j] = fold_coord_columns(pright, plan->rcoord[j]);
    }
  }
  if (!feasible) {
    ok = true; /* stay inactive; callers keep the extrinsic per-pair path */
    goto cleanup;
  }

  /* Crossing form on generator pairs via the cached half-product identity (step 1), then folded
   * with each right coordinate so the pair loop pays one AND+popcount. */
  for (uint32_t u = 0; u < lbasis.dim; u++) {
    const uint64_t *witness = qsop_bitset_const_row(left_assignments, words, lgen[u]);
    uint64_t row = 0;
    for (uint32_t v = 0; v < rbasis.dim; v++) {
      const uint64_t *right_bits = rw_signature_bits(pool, right_sigs[rgen[v]]);
      row |= (uint64_t)(qsop_bitset_popcount_intersection_simd(witness, right_bits, words, simd) &
                        1U)
             << v;
    }
    brow[u] = row;
  }
  for (size_t j = 0; j < rlen; j++) {
    uint64_t fold = 0;
    for (uint32_t u = 0; u < lbasis.dim; u++) {
      fold |= (uint64_t)(qsop_popcount_u64(brow[u] & plan->rcoord[j]) & 1U) << u;
    }
    plan->mfold[j] = fold;
  }

  plan->sig_memo = malloc(((size_t)1U << pbasis.dim) * sizeof(*plan->sig_memo));
  if (plan->sig_memo == NULL) {
    qsop_set_error(error, "out of memory while building rankwidth join plan memo");
    goto cleanup;
  }
  memset(plan->sig_memo, 0xFF, ((size_t)1U << pbasis.dim) * sizeof(*plan->sig_memo));
  plan->ldim = lbasis.dim;
  plan->rdim = rbasis.dim;
  plan->pdim = pbasis.dim;
  plan->llen = llen;
  plan->rlen = rlen;
  plan->active = true;
  ok = true;

cleanup:
  rw_dense_basis_free(&lbasis);
  rw_dense_basis_free(&rbasis);
  rw_dense_basis_free(&pbasis);
  free(scratch);
  free(proj);
  if (!ok || !plan->active) {
    const bool failed = !ok;
    rw_join_plan_free(plan);
    return !failed;
  }
  return true;
}
bool rw_join_plan_parent_signature(rw_join_plan_t *plan, rw_signature_pool_t *pool,
                                   uint64_t parent_coord, uint32_t left_signature,
                                   uint32_t right_signature, const uint64_t *outside,
                                   uint64_t *scratch_sig, size_t words, uint32_t *out,
                                   qsop_error_t *error) {
  uint32_t signature = plan->sig_memo[parent_coord];
  if (signature == UINT32_MAX) {
    /* First occurrence of this parent coordinate: intern the full-width bits exactly as the
     * extrinsic path would, so pool ids (and everything downstream) match it bit for bit. */
    const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
    qsop_bitset_copy(scratch_sig, rw_signature_bits(pool, left_signature), words);
    qsop_bitset_xor_simd(scratch_sig, rw_signature_bits(pool, right_signature), words, simd);
    qsop_bitset_and_simd(scratch_sig, outside, words, simd);
    if (!rw_signature_pool_intern(pool, scratch_sig, &signature, error)) {
      return false;
    }
    plan->sig_memo[parent_coord] = signature;
  }
#ifndef NDEBUG
  {
    /* Differential oracle: the memoized id must equal a fresh extrinsic computation. */
    const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
    uint32_t check = 0;
    qsop_bitset_copy(scratch_sig, rw_signature_bits(pool, left_signature), words);
    qsop_bitset_xor_simd(scratch_sig, rw_signature_bits(pool, right_signature), words, simd);
    qsop_bitset_and_simd(scratch_sig, outside, words, simd);
    if (!rw_signature_pool_intern(pool, scratch_sig, &check, error)) {
      return false;
    }
    assert(check == signature);
  }
#endif
  *out = signature;
  return true;
}
