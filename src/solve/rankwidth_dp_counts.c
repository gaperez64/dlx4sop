/* The count-table / all-modes-Fourier dynamic program over rank decompositions (integer and
 * CRT-residue arithmetic).
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

typedef struct rw_transition16 {
  uint16_t right_signature;
  uint16_t parent_signature;
  uint16_t residue_shift;
  uint16_t flags; /* reserved, keeps 8-byte alignment */
} rw_transition16_t;
typedef struct rw_transition32 {
  uint32_t right_signature;
  uint32_t parent_signature;
  uint32_t residue_shift;
  uint32_t flags;
} rw_transition32_t;
typedef enum rw_transition_layout_kind {
  RW_TRANSITION_LAYOUT_U16,
  RW_TRANSITION_LAYOUT_U32,
} rw_transition_layout_kind_t;
typedef struct rw_transition_csr {
  rw_transition_layout_kind_t kind;
  uint32_t left_sig_count;   /* number of unique left signatures */
  uint64_t transition_count; /* total number of valid transitions */
  uint32_t *left_signatures; /* [left_sig_count]: left sig id for each CSR row */
  uint32_t *offsets;         /* [left_sig_count + 1]: offset into items */
  union {
    rw_transition16_t *t16;
    rw_transition32_t *t32;
    void *raw;
  } items;
} rw_transition_csr_t;
static uint32_t fourier_odd_mode_count(uint32_t r) {
  return r / 2U;
}
typedef struct rw_join_workspace {
  uint64_t *acc;          /* [cap_entries] — zeroed by caller before each join */
  uint32_t *sig_map_idx;  /* [cap_sigs]    — 0xFF-filled by caller before each join */
  uint32_t *left_starts;  /* [cap_sigs] */
  uint32_t *left_ends;    /* [cap_sigs] */
  uint32_t *right_starts; /* [cap_sigs] */
  uint32_t *right_ends;   /* [cap_sigs] */
  size_t cap_entries;     /* capacity of acc in uint64_t elements (= cap_sigs * r) */
  size_t cap_sigs;        /* max signatures per node */
} rw_join_workspace_t;
typedef struct rw_linear_table {
  uint32_t *signatures;
  uint64_t *counts; /* row-major: signatures[row] x residue */
  uint32_t *slots;  /* signature -> row index, UINT32_MAX = empty */
  size_t len;
  size_t cap;
  size_t slots_mask;
} rw_linear_table_t;
static bool join_workspace_alloc(size_t cap_sigs, uint32_t r, rw_join_workspace_t *ws,
                                 qsop_error_t *error) {
  const size_t cap_entries = cap_sigs * (size_t)r;
  ws->cap_sigs = cap_sigs;
  ws->cap_entries = cap_entries;
  ws->acc = calloc(cap_entries == 0 ? 1U : cap_entries, sizeof(*ws->acc));
  ws->sig_map_idx = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->sig_map_idx));
  ws->left_starts = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->left_starts));
  ws->left_ends = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->left_ends));
  ws->right_starts = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->right_starts));
  ws->right_ends = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->right_ends));
  if (ws->acc == NULL || ws->sig_map_idx == NULL || ws->left_starts == NULL ||
      ws->left_ends == NULL || ws->right_starts == NULL || ws->right_ends == NULL) {
    free(ws->acc);
    free(ws->sig_map_idx);
    free(ws->left_starts);
    free(ws->left_ends);
    free(ws->right_starts);
    free(ws->right_ends);
    *ws = (rw_join_workspace_t){0};
    qsop_set_error(error, "out of memory allocating join workspace");
    return false;
  }
  return true;
}
static void join_workspace_free(rw_join_workspace_t *ws) {
  if (ws == NULL) {
    return;
  }
  free(ws->acc);
  free(ws->sig_map_idx);
  free(ws->left_starts);
  free(ws->left_ends);
  free(ws->right_starts);
  free(ws->right_ends);
  *ws = (rw_join_workspace_t){0};
}
static void linear_table_free(rw_linear_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->signatures);
  free(table->counts);
  free(table->slots);
  *table = (rw_linear_table_t){0};
}
static bool linear_table_reserve(rw_linear_table_t *table, size_t needed, uint32_t r,
                                 qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }
  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth linear table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (r != 0 && new_cap > SIZE_MAX / r / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth linear table is too large");
    return false;
  }
  uint32_t *signatures = malloc(new_cap * sizeof(*signatures));
  uint64_t *counts = calloc(new_cap * (size_t)r, sizeof(*counts));
  if (signatures == NULL || counts == NULL) {
    free(signatures);
    free(counts);
    qsop_set_error(error, "out of memory while growing rankwidth linear table");
    return false;
  }
  if (table->signatures != NULL && table->len != 0) {
    memcpy(signatures, table->signatures, table->len * sizeof(*signatures));
  }
  if (table->counts != NULL && table->len != 0) {
    memcpy(counts, table->counts, table->len * (size_t)r * sizeof(*counts));
  }
  free(table->signatures);
  free(table->counts);
  table->signatures = signatures;
  table->counts = counts;
  table->cap = new_cap;
  return true;
}
static bool linear_table_rehash(rw_linear_table_t *table, qsop_error_t *error) {
  size_t new_cap = table->slots_mask != 0 ? (table->slots_mask + 1U) * 2U : 16U;
  const size_t need = (table->len + 1U) * 2U;
  while (new_cap < need) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth linear signature index is too large");
      return false;
    }
    new_cap *= 2U;
  }
  uint32_t *slots = malloc(new_cap * sizeof(*slots));
  if (slots == NULL) {
    qsop_set_error(error, "out of memory while growing rankwidth linear signature index");
    return false;
  }
  memset(slots, 0xFF, new_cap * sizeof(*slots));
  const size_t mask = new_cap - 1U;
  for (size_t i = 0; i < table->len; i++) {
    size_t slot = rw_rep_hash(table->signatures[i]) & mask;
    while (slots[slot] != UINT32_MAX) {
      slot = (slot + 1U) & mask;
    }
    slots[slot] = (uint32_t)i;
  }
  free(table->slots);
  table->slots = slots;
  table->slots_mask = mask;
  return true;
}
static void linear_table_clear(rw_linear_table_t *table, uint32_t r) {
  if (table == NULL) {
    return;
  }
  if (table->counts != NULL && table->len != 0) {
    memset(table->counts, 0, table->len * (size_t)r * sizeof(*table->counts));
  }
  table->len = 0;
  if (table->slots != NULL) {
    memset(table->slots, 0xFF, (table->slots_mask + 1U) * sizeof(*table->slots));
  }
}
static bool linear_table_signature_index(rw_linear_table_t *table, uint32_t signature, uint32_t r,
                                         size_t *out, qsop_error_t *error) {
  if (table->slots == NULL || (table->len + 1U) * 2U > (table->slots_mask + 1U)) {
    if (!linear_table_rehash(table, error)) {
      return false;
    }
  }
  const size_t mask = table->slots_mask;
  size_t slot = rw_rep_hash(signature) & mask;
  while (table->slots[slot] != UINT32_MAX) {
    const uint32_t index = table->slots[slot];
    if (table->signatures[index] == signature) {
      *out = index;
      return true;
    }
    slot = (slot + 1U) & mask;
  }
  if (table->len > UINT32_MAX) {
    qsop_set_error(error, "rankwidth linear table exceeds uint32 rows");
    return false;
  }
  if (!linear_table_reserve(table, table->len + 1U, r, error)) {
    return false;
  }
  table->signatures[table->len] = signature;
  memset(&table->counts[table->len * (size_t)r], 0, (size_t)r * sizeof(*table->counts));
  table->slots[slot] = (uint32_t)table->len;
  *out = table->len++;
  return true;
}
static bool linear_table_add(uint64_t *dst, uint64_t value, uint64_t modulus, qsop_error_t *error) {
  if (value == 0) {
    return true;
  }
  if (modulus != 0) {
    *dst = qsop_mod_add_u64(*dst, value, modulus);
    return true;
  }
  return qsop_count_add(dst, value, error);
}
static uint64_t linear_table_nonzero_entries(const rw_linear_table_t *table, uint32_t r) {
  uint64_t count = 0;
  for (size_t i = 0; i < table->len; i++) {
    const uint64_t *row = &table->counts[i * (size_t)r];
    for (uint32_t residue = 0; residue < r; residue++) {
      count += row[residue] != 0;
    }
  }
  return count;
}
static void rw_transition_csr_free(rw_transition_csr_t *csr) {
  if (csr == NULL) {
    return;
  }
  free(csr->left_signatures);
  free(csr->offsets);
  free(csr->items.raw);
  *csr = (rw_transition_csr_t){0};
}
static uint64_t rw_transition_csr_bytes(const rw_transition_csr_t *csr) {
  if (csr == NULL || csr->left_sig_count == 0) {
    return 0;
  }
  const uint64_t item_size = (csr->kind == RW_TRANSITION_LAYOUT_U16) ? sizeof(rw_transition16_t)
                                                                     : sizeof(rw_transition32_t);
  return (uint64_t)(csr->left_sig_count + 1U) * sizeof(uint32_t) * 2U +
         csr->transition_count * item_size;
}
static uint32_t cross_parity_selected_rows(uint32_t nvars, const uint64_t *adj,
                                           const uint64_t *selected_assignment,
                                           const uint64_t *other_assignment, size_t words) {
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  uint32_t parity = 0;
  for (size_t w = 0; w < words; w++) {
    uint64_t bits = selected_assignment[w];
    while (bits != 0) {
      const uint32_t bit = rw_ctz_u64(bits);
      const size_t v = w * 64U + bit;
      if (v >= nvars) {
        break;
      }
      parity ^= qsop_bitset_popcount_intersection_simd(
                    qsop_bitset_const_row(adj, words, (uint32_t)v), other_assignment, words, simd) &
                1U;
      bits &= bits - 1U;
    }
  }
  return parity;
}
static uint32_t cross_parity_bitsets_weighted(uint32_t nvars, const uint64_t *adj,
                                              const uint64_t *left_assignment, uint32_t left_count,
                                              const uint64_t *right_assignment,
                                              uint32_t right_count, size_t words) {
  if (left_count == 0 || right_count == 0) {
    return 0;
  }
  if (right_count < left_count) {
    return cross_parity_selected_rows(nvars, adj, right_assignment, left_assignment, words);
  }
  return cross_parity_selected_rows(nvars, adj, left_assignment, right_assignment, words);
}
static bool solve_rankwidth_linear_count_table_mod_once(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, const uint32_t *order, uint64_t count_modulus, uint64_t *counts,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (count_modulus == 0 && qsop->nvars >= 64U) {
    qsop_set_error(error,
                   "rankwidth exact linear count-table handoff requires fewer than 64 variables");
    return false;
  }

  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats/_count_table_mod_stats above to
   * qsop->r <= UINT32_MAX before reaching this count-table path, which allocates O(r)
   * structures below. */
  const uint32_t r = (uint32_t)qsop->r;
  const uint32_t sign = r / 2U;
  const size_t words = decomposition->words;
  const size_t w = words == 0 ? 1U : words;
  rw_signature_pool_t pool = {0};
  rw_linear_table_t current = {0};
  rw_linear_table_t next = {0};
  uint64_t *suffix = calloc(w, sizeof(*suffix));
  uint64_t *zero_bits = calloc(w, sizeof(*zero_bits));
  uint64_t *one_bits = calloc(w, sizeof(*one_bits));
  bool ok = false;
  if (suffix == NULL || zero_bits == NULL || one_bits == NULL ||
      !rw_signature_pool_init(&pool, words, error)) {
    qsop_set_error(error, "out of memory while allocating rankwidth linear DP state");
    goto cleanup;
  }

  uint32_t zero_signature = 0;
  size_t zero_row = 0;
  if (!rw_signature_pool_intern(&pool, zero_bits, &zero_signature, error) ||
      !linear_table_signature_index(&current, zero_signature, r, &zero_row, error)) {
    goto cleanup;
  }
  current.counts[zero_row * (size_t)r] = count_modulus == 0 ? 1U : 1U % count_modulus;

  rw_fill_all_vars(suffix, qsop->nvars, words);
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  uint64_t value_transitions = 0;
  uint64_t signature_transitions = 0;
  const uint64_t start = qsop_trace_begin(trace);

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    const uint32_t v = order[pos];
    qsop_bitset_clear(suffix, v);
    linear_table_clear(&next, r);

    for (size_t row = 0; row < current.len; row++) {
      const uint32_t signature = current.signatures[row];
      const uint64_t *sig_bits = rw_signature_bits(&pool, signature);
      const bool incoming_odd = qsop_bitset_get(sig_bits, v);

      qsop_bitset_copy(zero_bits, sig_bits, words);
      qsop_bitset_and(zero_bits, suffix, words);

      qsop_bitset_copy(one_bits, zero_bits, words);
      const uint64_t *neighbors = qsop_bitset_const_row(adj, words, v);
      for (size_t word = 0; word < words; word++) {
        one_bits[word] ^= neighbors[word] & suffix[word];
      }

      uint32_t zero_sig = 0;
      uint32_t one_sig = 0;
      size_t zero_idx = 0;
      size_t one_idx = 0;
      if (!rw_signature_pool_intern(&pool, zero_bits, &zero_sig, error) ||
          !rw_signature_pool_intern(&pool, one_bits, &one_sig, error) ||
          !linear_table_signature_index(&next, zero_sig, r, &zero_idx, error) ||
          !linear_table_signature_index(&next, one_sig, r, &one_idx, error)) {
        goto cleanup;
      }
      signature_transitions += 2U;

      const uint32_t select_shift =
          (uint32_t)(((uint64_t)(qsop->unary[v] % r) + (incoming_odd ? (uint64_t)sign : 0U)) % r);
      const uint64_t *src = &current.counts[row * (size_t)r];
      uint64_t *zero_dst = &next.counts[zero_idx * (size_t)r];
      uint64_t *one_dst = &next.counts[one_idx * (size_t)r];
      for (uint32_t residue = 0; residue < r; residue++) {
        const uint64_t value = src[residue];
        if (value == 0) {
          continue;
        }
        if (!linear_table_add(&zero_dst[residue], value, count_modulus, error)) {
          goto cleanup;
        }
        uint32_t shifted = residue + select_shift;
        if (shifted >= r) {
          shifted -= r;
        }
        if (!linear_table_add(&one_dst[shifted], value, count_modulus, error)) {
          goto cleanup;
        }
        value_transitions += 2U;
      }
    }

    rw_linear_table_t tmp = current;
    current = next;
    next = tmp;

    const uint64_t step_entries = linear_table_nonzero_entries(&current, r);
    table_entries = qsop_saturating_add_u64(table_entries, step_entries);
    signature_entries = qsop_saturating_add_u64(signature_entries, current.len);
    if (step_entries > max_table_entries) {
      max_table_entries = step_entries;
    }
    if (current.len > max_signature_entries) {
      max_signature_entries = current.len;
    }
  }

  qsop_counts_clear(r, counts);
  for (size_t row = 0; row < current.len; row++) {
    const uint64_t *sig_bits = rw_signature_bits(&pool, current.signatures[row]);
    if (!qsop_bitset_empty(sig_bits, words)) {
      continue;
    }
    const uint64_t *src = &current.counts[row * (size_t)r];
    for (uint32_t residue = 0; residue < r; residue++) {
      if (src[residue] == 0) {
        continue;
      }
      uint32_t shifted = residue + (uint32_t)(qsop->constant % r);
      if (shifted >= r) {
        shifted -= r;
      }
      if (!linear_table_add(&counts[shifted], src[residue], count_modulus, error)) {
        goto cleanup;
      }
    }
  }

  qsop_trace_emit_elapsed(trace, "rankwidth.linear_dp", 0, value_transitions, start);
  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = value_transitions;
    stats->join_signature_pairs = signature_transitions;
    stats->rankwidth_linear_transition_events = value_transitions;
    stats->rankwidth_table_assignment_bytes = 0;
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      goto cleanup;
    }
  }

  ok = true;

cleanup:
  free(suffix);
  free(zero_bits);
  free(one_bits);
  linear_table_free(&current);
  linear_table_free(&next);
  rw_signature_pool_free(&pool);
  return ok;
}
static bool solve_rankwidth_linear_count_table_crt(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, const uint32_t *order, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  uint64_t *all_counts = NULL;
  uint64_t *residues = NULL;
  qsop_result_t *result = NULL;
  size_t nprimes = 0;
  bool ok = false;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth linear CRT count table is too large");
    goto cleanup;
  }
  all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
  residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  result = calloc(1, sizeof(*result));
  if (all_counts == NULL || residues == NULL || result == NULL) {
    qsop_set_error(error, "out of memory for rankwidth linear CRT state");
    goto cleanup;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(r32, sizeof(*result->count_strings));
  if (result->count_strings == NULL) {
    qsop_set_error(error, "out of memory for rankwidth linear CRT result strings");
    goto cleanup;
  }

  ok = true;
  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!solve_rankwidth_linear_count_table_mod_once(qsop, decomposition, adj, order, primes[p],
                                                     &all_counts[p * (size_t)r32], stats_for_prime,
                                                     trace_for_prime, error)) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    goto cleanup;
  }

  for (uint32_t residue = 0; residue < r32; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)r32 + residue];
    }
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                      error)) {
      ok = false;
      goto cleanup;
    }
  }

  *out = result;
  result = NULL;

cleanup:
  free(primes);
  free(all_counts);
  free(residues);
  qsop_result_free(result);
  return ok;
}
static bool solve_rankwidth_linear_count_table(const qsop_instance_t *qsop,
                                               const qsop_rankwidth_decomposition_t *decomposition,
                                               const uint64_t *adj, const uint32_t *order,
                                               qsop_result_t **out, qsop_solve_stats_t *stats,
                                               qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (qsop->nvars >= 64U) {
    return solve_rankwidth_linear_count_table_crt(qsop, decomposition, adj, order, out, stats,
                                                  trace, error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL || !qsop_counts_alloc((uint32_t)qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth linear result");
    return false;
  }
  result->r = (uint32_t)qsop->r;
  result->norm_h = qsop->norm_h;
  if (!solve_rankwidth_linear_count_table_mod_once(qsop, decomposition, adj, order, 0,
                                                   result->counts, stats, trace, error)) {
    qsop_result_free(result);
    return false;
  }
  *out = result;
  return true;
}
bool rw_compute_join_transition_sign(uint32_t nvars, const uint64_t *adj, rw_signature_pool_t *pool,
                                     const uint64_t *outside, size_t words, uint32_t r,
                                     uint32_t left_signature, const uint64_t *left_rep,
                                     uint32_t left_weight, uint32_t right_signature,
                                     const uint64_t *right_rep, uint32_t right_weight,
                                     uint64_t *scratch_sig, rw_transition_eval_t *out,
                                     qsop_error_t *error) {
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  const uint32_t sign = r / 2U;
  const uint32_t parity = cross_parity_bitsets_weighted(nvars, adj, left_rep, left_weight,
                                                        right_rep, right_weight, words);
  qsop_bitset_copy(scratch_sig, rw_signature_bits(pool, left_signature), words);
  qsop_bitset_xor_simd(scratch_sig, rw_signature_bits(pool, right_signature), words, simd);
  qsop_bitset_and_simd(scratch_sig, outside, words, simd);
  uint32_t parent_sig = 0;
  if (!rw_signature_pool_intern(pool, scratch_sig, &parent_sig, error)) {
    return false;
  }
  *out = (rw_transition_eval_t){
      .valid = true,
      .left_signature = left_signature,
      .right_signature = right_signature,
      .parent_signature = parent_sig,
      .residue_shift = r == 0 ? 0U : (uint32_t)(((uint64_t)sign * parity) % r),
      .sign_flip = parity != 0,
  };
  return true;
}
static bool rw_transition_csr_build_sign(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t node_id __attribute__((unused)), const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_table_t *left, const rw_table_t *right, const uint64_t *outside, uint64_t *scratch_sig,
    rw_transition_csr_t *out, uint64_t *u16_events, uint64_t *u32_events, qsop_error_t *error) {
  const size_t words = decomposition->words;
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t r = (uint32_t)qsop->r;
  const size_t lreps = left->reps_len;
  const size_t rreps = right->reps_len;
  if (lreps == 0 || rreps == 0) {
    return true;
  }

  /* Pass 1: count transitions per left rep and track max parent sig id. */
  uint64_t *counts = calloc(lreps, sizeof(*counts));
  if (counts == NULL) {
    qsop_set_error(error, "out of memory building CSR transition counts");
    return false;
  }
  uint32_t max_rsig = 0;
  uint32_t max_psig = 0;
  uint64_t total = 0;
  for (uint32_t i = 0; i < (uint32_t)lreps; i++) {
    const uint64_t *lrep = rw_table_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)rreps; j++) {
      const uint64_t *rrep = rw_table_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(qsop->nvars, adj, pool, outside, words, r,
                                           left->reps[i].signature, lrep, left->rep_weights[i],
                                           right->reps[j].signature, rrep, right->rep_weights[j],
                                           scratch_sig, &eval, error)) {
        free(counts);
        return false;
      }
      if (!eval.valid)
        continue;
      counts[i]++;
      total++;
      if (right->reps[j].signature > max_rsig)
        max_rsig = right->reps[j].signature;
      if (eval.parent_signature > max_psig)
        max_psig = eval.parent_signature;
    }
  }

  /* Determine layout. */
  const uint32_t left_sig_count = (uint32_t)lreps;
  rw_transition_layout_kind_t kind;
  if (left_sig_count <= UINT16_MAX && max_rsig <= UINT16_MAX && max_psig <= UINT16_MAX &&
      r <= UINT16_MAX) {
    kind = RW_TRANSITION_LAYOUT_U16;
    if (u16_events != NULL)
      (*u16_events)++;
  } else {
    kind = RW_TRANSITION_LAYOUT_U32;
    if (u32_events != NULL)
      (*u32_events)++;
  }

  /* Build offsets from counts. */
  uint32_t *left_signatures = malloc(left_sig_count * sizeof(*left_signatures));
  uint32_t *offsets = malloc((left_sig_count + 1U) * sizeof(*offsets));
  if (left_signatures == NULL || offsets == NULL) {
    free(counts);
    free(left_signatures);
    free(offsets);
    qsop_set_error(error, "out of memory building CSR offsets");
    return false;
  }
  for (uint32_t i = 0; i < left_sig_count; i++) {
    left_signatures[i] = left->reps[i].signature;
  }
  offsets[0] = 0;
  for (uint32_t i = 0; i < left_sig_count; i++) {
    offsets[i + 1U] = offsets[i] + (uint32_t)counts[i];
  }
  free(counts);

  /* Allocate items. */
  void *items = NULL;
  if (total > 0) {
    if (kind == RW_TRANSITION_LAYOUT_U16) {
      items = calloc(total, sizeof(rw_transition16_t));
    } else {
      items = calloc(total, sizeof(rw_transition32_t));
    }
    if (items == NULL) {
      free(left_signatures);
      free(offsets);
      qsop_set_error(error, "out of memory building CSR transition items");
      return false;
    }
  }

  /* Pass 2: fill items using cursor array. */
  uint32_t *cursors = malloc(left_sig_count * sizeof(*cursors));
  if (cursors == NULL) {
    free(left_signatures);
    free(offsets);
    free(items);
    qsop_set_error(error, "out of memory building CSR cursors");
    return false;
  }
  memcpy(cursors, offsets, left_sig_count * sizeof(*cursors));

  for (uint32_t i = 0; i < (uint32_t)lreps; i++) {
    const uint64_t *lrep = rw_table_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)rreps; j++) {
      const uint64_t *rrep = rw_table_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(qsop->nvars, adj, pool, outside, words, r,
                                           left->reps[i].signature, lrep, left->rep_weights[i],
                                           right->reps[j].signature, rrep, right->rep_weights[j],
                                           scratch_sig, &eval, error)) {
        free(left_signatures);
        free(offsets);
        free(items);
        free(cursors);
        return false;
      }
      if (!eval.valid)
        continue;
      const uint32_t pos = cursors[i]++;
      if (kind == RW_TRANSITION_LAYOUT_U16) {
        rw_transition16_t *t = (rw_transition16_t *)items + pos;
        t->right_signature = (uint16_t)eval.right_signature;
        t->parent_signature = (uint16_t)eval.parent_signature;
        t->residue_shift = (uint16_t)eval.residue_shift;
        t->flags = 0;
      } else {
        rw_transition32_t *t = (rw_transition32_t *)items + pos;
        t->right_signature = eval.right_signature;
        t->parent_signature = eval.parent_signature;
        t->residue_shift = eval.residue_shift;
        t->flags = 0;
      }
    }
  }
  free(cursors);

  *out = (rw_transition_csr_t){
      .kind = kind,
      .left_sig_count = left_sig_count,
      .transition_count = total,
      .left_signatures = left_signatures,
      .offsets = offsets,
      .items = {.raw = items},
  };
  return true;
}
static bool rw_execute_csr_join_sign(const qsop_instance_t *qsop, const rw_transition_csr_t *csr,
                                     const rw_table_t *left, const rw_table_t *right,
                                     rw_table_t *out, size_t words, uint64_t *join_pairs_out,
                                     rw_join_workspace_t *ws, qsop_error_t *error) {
  if (csr->transition_count == 0) {
    return true;
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();

  /* Find max parent sig across all items. */
  uint32_t max_psig = 0;
  uint32_t max_lsig = 0;
  uint32_t max_rsig = 0;
  if (csr->kind == RW_TRANSITION_LAYOUT_U16) {
    for (uint64_t p = 0; p < csr->transition_count; p++) {
      const rw_transition16_t *t = csr->items.t16 + p;
      if (t->parent_signature > max_psig)
        max_psig = t->parent_signature;
      if (t->right_signature > max_rsig)
        max_rsig = t->right_signature;
    }
  } else {
    for (uint64_t p = 0; p < csr->transition_count; p++) {
      const rw_transition32_t *t = csr->items.t32 + p;
      if (t->parent_signature > max_psig)
        max_psig = t->parent_signature;
      if (t->right_signature > max_rsig)
        max_rsig = t->right_signature;
    }
  }
  for (uint32_t i = 0; i < csr->left_sig_count; i++) {
    if (csr->left_signatures[i] > max_lsig)
      max_lsig = csr->left_signatures[i];
  }

  const size_t n_psigs = (size_t)max_psig + 1U;
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t r = (uint32_t)qsop->r;
  const uint32_t r_mask = r - 1U;
  const bool r_pow2 = (r & r_mask) == 0;

  /* Allocate or reuse workspace. */
  const bool use_ws = ws != NULL && n_psigs <= ws->cap_sigs && (size_t)max_lsig < ws->cap_sigs &&
                      (size_t)max_rsig < ws->cap_sigs;
  uint64_t *acc;
  uint32_t *sig_map_left;  /* parent_sig -> left_rep_index for witness */
  uint32_t *sig_map_right; /* parent_sig -> right_rep_index for witness */
  uint32_t *left_starts, *left_ends, *right_starts, *right_ends;
  if (use_ws) {
    acc = ws->acc;
    left_starts = ws->left_starts;
    left_ends = ws->left_ends;
    right_starts = ws->right_starts;
    right_ends = ws->right_ends;
    memset(acc, 0, n_psigs * r * sizeof(*acc));
    rw_build_sig_range_index_into(left, max_lsig, left_starts, left_ends);
    rw_build_sig_range_index_into(right, max_rsig, right_starts, right_ends);
  } else {
    acc = calloc(n_psigs * r, sizeof(*acc));
    left_starts = right_starts = left_ends = right_ends = NULL;
    if (acc == NULL || !rw_build_sig_range_index(left, max_lsig, &left_starts, &left_ends, error) ||
        !rw_build_sig_range_index(right, max_rsig, &right_starts, &right_ends, error)) {
      free(acc);
      free(left_starts);
      free(left_ends);
      free(right_starts);
      free(right_ends);
      if (acc == NULL)
        qsop_set_error(error, "out of memory in CSR join accumulator");
      return false;
    }
  }
  /* Per-parent-sig witness: store (left_rep_idx, right_rep_idx) for table_add_rep. */
  sig_map_left = malloc(n_psigs * sizeof(*sig_map_left));
  sig_map_right = malloc(n_psigs * sizeof(*sig_map_right));
  if (sig_map_left == NULL || sig_map_right == NULL) {
    if (!use_ws) {
      free(acc);
      free(left_starts);
      free(left_ends);
      free(right_starts);
      free(right_ends);
    }
    free(sig_map_left);
    free(sig_map_right);
    qsop_set_error(error, "out of memory in CSR join witness map");
    return false;
  }
  memset(sig_map_left, 0xFF, n_psigs * sizeof(*sig_map_left));
  memset(sig_map_right, 0xFF, n_psigs * sizeof(*sig_map_right));

  /* Build rep-index lookup: sig -> rep index in left/right tables (one per sig). */
  uint32_t *left_rep_idx = malloc(((size_t)max_lsig + 1U) * sizeof(*left_rep_idx));
  uint32_t *right_rep_idx = malloc(((size_t)max_rsig + 1U) * sizeof(*right_rep_idx));
  if (left_rep_idx == NULL || right_rep_idx == NULL) {
    if (!use_ws) {
      free(acc);
      free(left_starts);
      free(left_ends);
      free(right_starts);
      free(right_ends);
    }
    free(sig_map_left);
    free(sig_map_right);
    free(left_rep_idx);
    free(right_rep_idx);
    qsop_set_error(error, "out of memory building rep index for CSR join");
    return false;
  }
  memset(left_rep_idx, 0xFF, ((size_t)max_lsig + 1U) * sizeof(*left_rep_idx));
  memset(right_rep_idx, 0xFF, ((size_t)max_rsig + 1U) * sizeof(*right_rep_idx));
  for (size_t k = 0; k < left->reps_len; k++) {
    left_rep_idx[left->reps[k].signature] = (uint32_t)k;
  }
  for (size_t k = 0; k < right->reps_len; k++) {
    right_rep_idx[right->reps[k].signature] = (uint32_t)k;
  }

#define CSR_JOIN_CLEANUP()                                                                         \
  do {                                                                                             \
    if (!use_ws) {                                                                                 \
      free(acc);                                                                                   \
      free(left_starts);                                                                           \
      free(left_ends);                                                                             \
      free(right_starts);                                                                          \
      free(right_ends);                                                                            \
    }                                                                                              \
    free(sig_map_left);                                                                            \
    free(sig_map_right);                                                                           \
    free(left_rep_idx);                                                                            \
    free(right_rep_idx);                                                                           \
  } while (0)

  /* Main accumulation loop over CSR rows (left signatures). */
  uint64_t join_pairs = 0;
  for (uint32_t ci = 0; ci < csr->left_sig_count; ci++) {
    const uint32_t lsig = csr->left_signatures[ci];
    const uint32_t l_ri = left_rep_idx[lsig]; /* rep index in left table */
    if (l_ri == UINT32_MAX)
      continue;
    const size_t l_start = (left_starts[lsig] != UINT32_MAX) ? left_starts[lsig] : 0;
    const size_t l_end = (left_starts[lsig] != UINT32_MAX) ? left_ends[lsig] : 0;
    const uint32_t begin = csr->offsets[ci];
    const uint32_t end = csr->offsets[ci + 1U];

    for (uint32_t p = begin; p < end; p++) {
      uint32_t rsig, psig, shift;
      if (csr->kind == RW_TRANSITION_LAYOUT_U16) {
        const rw_transition16_t *t = csr->items.t16 + p;
        rsig = t->right_signature;
        psig = t->parent_signature;
        shift = t->residue_shift;
      } else {
        const rw_transition32_t *t = csr->items.t32 + p;
        rsig = t->right_signature;
        psig = t->parent_signature;
        shift = t->residue_shift;
      }
      if (sig_map_left[psig] == UINT32_MAX) {
        sig_map_left[psig] = l_ri;
        sig_map_right[psig] = (rsig <= max_rsig) ? right_rep_idx[rsig] : UINT32_MAX;
      }
      const size_t r_start = (right_starts[rsig] != UINT32_MAX) ? right_starts[rsig] : 0;
      const size_t r_end = (right_starts[rsig] != UINT32_MAX) ? right_ends[rsig] : 0;

      for (size_t i = l_start; i < l_end; i++) {
        const uint32_t l_res = left->entries[i].residue;
        const uint64_t l_cnt = left->entries[i].count;
        for (size_t j = r_start; j < r_end; j++) {
          const uint64_t rsum = (uint64_t)l_res + right->entries[j].residue + shift;
          const uint32_t res = r_pow2 ? (uint32_t)(rsum & r_mask) : (uint32_t)(rsum % r);
          uint64_t product = 0;
          if (!qsop_count_mul(l_cnt, right->entries[j].count, &product, error) ||
              !qsop_count_add(&acc[(size_t)psig * r + res], product, error)) {
            CSR_JOIN_CLEANUP();
            return false;
          }
          join_pairs++;
        }
      }
    }
  }

  /* Flush accumulator to output table. */
  size_t nonzero_count = 0;
  for (size_t i = 0; i < n_psigs * r; i++) {
    if (acc[i] != 0)
      nonzero_count++;
  }
  if (!rw_reserve_entries(out, out->len + nonzero_count, error)) {
    CSR_JOIN_CLEANUP();
    return false;
  }

  /* Temp buffer for parent rep (left_rep | right_rep). D3.1: reconstructed, not stored. */
  const size_t w = words == 0 ? 1U : words;
  uint64_t *parent_rep = calloc(w, sizeof(*parent_rep));
  if (parent_rep == NULL) {
    CSR_JOIN_CLEANUP();
    qsop_set_error(error, "out of memory for CSR join parent rep");
    return false;
  }

  for (uint32_t s = 0; s < (uint32_t)n_psigs; s++) {
    if (sig_map_left[s] == UINT32_MAX)
      continue;
    const uint32_t li = sig_map_left[s];
    const uint32_t ri = sig_map_right[s];
    /* Reconstruct parent representative: left_rep | right_rep. */
    if (li != UINT32_MAX && ri != UINT32_MAX) {
      qsop_bitset_copy(parent_rep, rw_table_assignment(left, li, words), words);
      qsop_bitset_or_simd(parent_rep, rw_table_assignment(right, ri, words), words, simd);
    } else {
      memset(parent_rep, 0, w * sizeof(*parent_rep));
    }
    if (!rw_table_add_rep(out, s, parent_rep, words, error)) {
      free(parent_rep);
      CSR_JOIN_CLEANUP();
      return false;
    }
    for (uint32_t res = 0; res < r; res++) {
      const uint64_t cnt = acc[(size_t)s * r + res];
      if (cnt == 0)
        continue;
      out->entries[out->len++] = (rw_entry_t){.signature = s, .residue = res, .count = cnt};
    }
  }
  free(parent_rep);
#undef CSR_JOIN_CLEANUP

  if (!use_ws) {
    free(acc);
    free(left_starts);
    free(left_ends);
    free(right_starts);
    free(right_ends);
  }
  free(sig_map_left);
  free(sig_map_right);
  free(left_rep_idx);
  free(right_rep_idx);

  if (join_pairs_out != NULL)
    *join_pairs_out += join_pairs;
  return true;
}
static bool rw_join_count_table_streaming_sign(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition __attribute__((unused)),
    uint32_t node_id __attribute__((unused)), const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_table_t *left, const rw_table_t *right, rw_table_t *out, const uint64_t *outside,
    uint64_t *scratch_sig, size_t words, uint64_t *candidate_pairs_out, uint64_t *emitted_pairs_out,
    qsop_error_t *error) {
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t r = (uint32_t)qsop->r;
  const uint32_t r_mask = r - 1U;
  const bool r_pow2 = (r & r_mask) == 0;
  const size_t lreps = left->reps_len;
  const size_t rreps = right->reps_len;
  if (lreps == 0 || rreps == 0) {
    return true;
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();

  uint32_t max_lsig = 0, max_rsig = 0;
  for (size_t i = 0; i < lreps; i++) {
    if (left->reps[i].signature > max_lsig)
      max_lsig = left->reps[i].signature;
  }
  for (size_t j = 0; j < rreps; j++) {
    if (right->reps[j].signature > max_rsig)
      max_rsig = right->reps[j].signature;
  }

  uint32_t *left_starts = NULL, *left_ends = NULL;
  uint32_t *right_starts = NULL, *right_ends = NULL;
  if (!rw_build_sig_range_index(left, max_lsig, &left_starts, &left_ends, error) ||
      !rw_build_sig_range_index(right, max_rsig, &right_starts, &right_ends, error)) {
    free(left_starts);
    free(left_ends);
    free(right_starts);
    free(right_ends);
    return false;
  }

  uint64_t candidate_pairs = 0;
  uint64_t emitted_pairs = 0;

  /* We need a dynamic accumulator (parent_sig may grow during iteration). */
  /* Use a simple per-(sig,res) accumulator map built on-the-fly. */
  /* For correctness we accumulate into the output table directly using table_add_entry.
   * This is less efficient than the CSR path but avoids pre-knowing max_psig. */
  for (size_t i = 0; i < lreps; i++) {
    const uint64_t *lrep = rw_table_assignment(left, i, words);
    const uint32_t lsig = left->reps[i].signature;
    const size_t l_start = (left_starts[lsig] != UINT32_MAX) ? left_starts[lsig] : 0;
    const size_t l_end = (left_starts[lsig] != UINT32_MAX) ? left_ends[lsig] : 0;

    for (size_t j = 0; j < rreps; j++) {
      const uint64_t *rrep = rw_table_assignment(right, j, words);
      rw_transition_eval_t eval;
      candidate_pairs++;
      if (!rw_compute_join_transition_sign(qsop->nvars, adj, pool, outside, words, r, lsig, lrep,
                                           left->rep_weights[i], right->reps[j].signature, rrep,
                                           right->rep_weights[j], scratch_sig, &eval, error)) {
        free(left_starts);
        free(left_ends);
        free(right_starts);
        free(right_ends);
        return false;
      }
      if (!eval.valid)
        continue;
      emitted_pairs++;

      const uint32_t rsig = eval.right_signature;
      const uint32_t psig = eval.parent_signature;
      const uint32_t shift = eval.residue_shift;

      /* Add representative for parent signature (once per unique psig). */
      {
        uint64_t *parent_rep = scratch_sig; /* reuse scratch after transition is computed */
        const size_t w = words == 0 ? 1U : words;
        qsop_bitset_copy(parent_rep, lrep, words);
        qsop_bitset_or_simd(parent_rep, rrep, words, simd);
        if (!rw_table_add_rep(out, psig, parent_rep, words, error)) {
          free(left_starts);
          free(left_ends);
          free(right_starts);
          free(right_ends);
          return false;
        }
        /* scratch_sig is now clobbered — restore clean zeroes for next transition call */
        memset(scratch_sig, 0, w * sizeof(*scratch_sig));
      }

      const size_t r_start = (right_starts[rsig] != UINT32_MAX) ? right_starts[rsig] : 0;
      const size_t r_end = (right_starts[rsig] != UINT32_MAX) ? right_ends[rsig] : 0;

      for (size_t li = l_start; li < l_end; li++) {
        const uint32_t l_res = left->entries[li].residue;
        const uint64_t l_cnt = left->entries[li].count;
        for (size_t rj = r_start; rj < r_end; rj++) {
          const uint64_t rsum = (uint64_t)l_res + right->entries[rj].residue + shift;
          const uint32_t res = r_pow2 ? (uint32_t)(rsum & r_mask) : (uint32_t)(rsum % r);
          uint64_t product = 0;
          if (!qsop_count_mul(l_cnt, right->entries[rj].count, &product, error) ||
              !rw_table_add_entry(out, psig, res, product, error)) {
            free(left_starts);
            free(left_ends);
            free(right_starts);
            free(right_ends);
            return false;
          }
        }
      }
    }
  }

  free(left_starts);
  free(left_ends);
  free(right_starts);
  free(right_ends);
  if (candidate_pairs_out != NULL)
    *candidate_pairs_out += candidate_pairs;
  if (emitted_pairs_out != NULL)
    *emitted_pairs_out += emitted_pairs;
  return true;
}
static bool build_join_map(const qsop_instance_t *qsop,
                           const qsop_rankwidth_decomposition_t *decomposition, uint32_t node_id,
                           const uint64_t *adj, rw_signature_pool_t *pool, const rw_table_t *left,
                           const rw_table_t *right, rw_join_map_t *map, qsop_error_t *error) {
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t sign = (uint32_t)qsop->r / 2U;
  if (left->reps_len > 0 && right->reps_len > SIZE_MAX / left->reps_len) {
    qsop_set_error(error, "rankwidth join map is too large");
    return false;
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  const size_t words = decomposition->words;
  if (!rw_reserve_join_map(map, left->reps_len * right->reps_len, words, error)) {
    return false;
  }
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  uint64_t *signature = calloc(words == 0 ? 1U : words, sizeof(*signature));
  if (outside == NULL || signature == NULL) {
    free(outside);
    free(signature);
    qsop_set_error(error, "out of memory while building rankwidth join map");
    return false;
  }
  rw_fill_all_vars(outside, decomposition->nvars, words);
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->reps_len; i++) {
    for (size_t j = 0; j < right->reps_len; j++) {
      const uint64_t *left_rep = rw_table_assignment(left, i, words);
      const uint64_t *right_rep = rw_table_assignment(right, j, words);
      const uint32_t parity =
          cross_parity_bitsets_weighted(qsop->nvars, adj, left_rep, left->rep_weights[i], right_rep,
                                        right->rep_weights[j], words);
      qsop_bitset_copy(signature, rw_signature_bits(pool, left->reps[i].signature), words);
      qsop_bitset_xor_simd(signature, rw_signature_bits(pool, right->reps[j].signature), words,
                           simd);
      qsop_bitset_and(signature, outside, words);
      uint32_t parent_signature = 0;
      if (!rw_signature_pool_intern(pool, signature, &parent_signature, error)) {
        free(outside);
        free(signature);
        return false;
      }

      const size_t index = map->len++;
      uint64_t *assignment = rw_join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or_simd(assignment, right_rep, words, simd);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->reps[i].signature,
          .right_signature = right->reps[j].signature,
          .parent_signature = parent_signature,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  free(outside);
  free(signature);
  return rw_join_map_build_sorted_idx(map, error);
}
static bool build_join_map_arena(const qsop_instance_t *qsop,
                                 const qsop_rankwidth_decomposition_t *decomposition,
                                 uint32_t node_id, const uint64_t *adj, rw_signature_pool_t *pool,
                                 const rw_table_t *left, const rw_table_t *right,
                                 rw_join_map_t *map, uint64_t *scratch, qsop_error_t *error) {
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t sign = (uint32_t)qsop->r / 2U;
  if (left->reps_len > 0 && right->reps_len > SIZE_MAX / left->reps_len) {
    qsop_set_error(error, "rankwidth join map is too large");
    return false;
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  const size_t words = decomposition->words;
  const size_t w = words == 0 ? 1U : words;
  if (!rw_reserve_join_map(map, left->reps_len * right->reps_len, words, error)) {
    return false;
  }
  uint64_t *outside = scratch;
  uint64_t *signature = scratch + w;
  rw_fill_all_vars(outside, decomposition->nvars, words);
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->reps_len; i++) {
    for (size_t j = 0; j < right->reps_len; j++) {
      const uint64_t *left_rep = rw_table_assignment(left, i, words);
      const uint64_t *right_rep = rw_table_assignment(right, j, words);
      const uint32_t parity =
          cross_parity_bitsets_weighted(qsop->nvars, adj, left_rep, left->rep_weights[i], right_rep,
                                        right->rep_weights[j], words);
      qsop_bitset_copy(signature, rw_signature_bits(pool, left->reps[i].signature), words);
      qsop_bitset_xor_simd(signature, rw_signature_bits(pool, right->reps[j].signature), words,
                           simd);
      qsop_bitset_and(signature, outside, words);
      uint32_t parent_signature = 0;
      if (!rw_signature_pool_intern(pool, signature, &parent_signature, error)) {
        return false;
      }

      const size_t index = map->len++;
      uint64_t *assignment = rw_join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or_simd(assignment, right_rep, words, simd);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->reps[i].signature,
          .right_signature = right->reps[j].signature,
          .parent_signature = parent_signature,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  return true;
}
static const rw_join_map_entry_t *join_map_get(const rw_join_map_t *map, uint32_t left_signature,
                                               uint32_t right_signature, size_t *index_out) {
  if (map->sorted_keys != NULL) {
    const uint64_t target = ((uint64_t)left_signature << 32) | right_signature;
    size_t lo = 0, hi = map->len;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (map->sorted_keys[mid] < target) {
        lo = mid + 1;
      } else if (map->sorted_keys[mid] > target) {
        hi = mid;
      } else {
        if (index_out != NULL) {
          *index_out = map->sorted_idx[mid];
        }
        return &map->entries[map->sorted_idx[mid]];
      }
    }
    return NULL;
  }
  for (size_t i = 0; i < map->len; i++) {
    if (map->entries[i].left_signature == left_signature &&
        map->entries[i].right_signature == right_signature) {
      if (index_out != NULL) {
        *index_out = i;
      }
      return &map->entries[i];
    }
  }
  return NULL;
}
static bool solve_leaf_arena(const qsop_instance_t *qsop, const uint64_t *adj,
                             const rw_node_t *node, size_t words, rw_signature_pool_t *pool,
                             rw_table_t *table, uint64_t *scratch, qsop_error_t *error) {
  const size_t w = words == 0 ? 1U : words;
  uint64_t *zero = scratch;
  uint64_t *assignment = scratch + w;
  uint64_t *signature = scratch + 2U * w;

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature, node->var);
  qsop_bitset_set(assignment, node->var);
  return rw_signature_pool_intern(pool, zero, &zero_signature, error) &&
         rw_signature_pool_intern(pool, signature, &one_signature, error) &&
         rw_table_add_rep(table, zero_signature, zero, words, error) &&
         rw_table_add_entry(table, zero_signature, 0, 1, error) &&
         rw_table_add_rep(table, one_signature, assignment, words, error) &&
         rw_table_add_entry(table, one_signature, (uint32_t)(qsop->unary[node->var] % qsop->r), 1,
                            error);
}
static bool solve_leaf_mod(const qsop_instance_t *qsop, const uint64_t *adj, const rw_node_t *node,
                           size_t words, rw_signature_pool_t *pool, uint64_t modulus,
                           rw_table_t *table, qsop_error_t *error) {
  uint64_t *zero = calloc(words == 0 ? 1U : words, sizeof(*zero));
  uint64_t *assignment = calloc(words == 0 ? 1U : words, sizeof(*assignment));
  uint64_t *signature = calloc(words == 0 ? 1U : words, sizeof(*signature));
  if (zero == NULL || assignment == NULL || signature == NULL) {
    free(zero);
    free(assignment);
    free(signature);
    qsop_set_error(error, "out of memory while solving modular rankwidth leaf");
    return false;
  }

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature, node->var);
  qsop_bitset_set(assignment, node->var);
  const bool ok =
      rw_signature_pool_intern(pool, zero, &zero_signature, error) &&
      rw_signature_pool_intern(pool, signature, &one_signature, error) &&
      rw_table_add_rep(table, zero_signature, zero, words, error) &&
      rw_table_add_entry_mod(table, zero_signature, 0, 1, modulus, error) &&
      rw_table_add_rep(table, one_signature, assignment, words, error) &&
      rw_table_add_entry_mod(table, one_signature, (uint32_t)(qsop->unary[node->var] % qsop->r), 1,
                             modulus, error);
  free(zero);
  free(assignment);
  free(signature);
  return ok;
}
static bool solve_join_mod(const qsop_instance_t *qsop, const rw_join_map_t *map,
                           const rw_table_t *left, const rw_table_t *right, uint64_t modulus,
                           rw_table_t *out, size_t words, uint64_t *join_pairs,
                           qsop_error_t *error) {
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      size_t map_index = 0;
      const rw_join_map_entry_t *mapped =
          join_map_get(map, left->entries[i].signature, right->entries[j].signature, &map_index);
      if (mapped == NULL) {
        qsop_set_error(error, "internal error: missing modular rankwidth join-map entry");
        return false;
      }
      const uint32_t residue = (uint32_t)(((uint64_t)left->entries[i].residue +
                                           right->entries[j].residue + mapped->residue_shift) %
                                          qsop->r);
      const uint64_t product =
          qsop_mod_mul_u64(left->entries[i].count, right->entries[j].count, modulus);
      if (!rw_table_add_rep(out, mapped->parent_signature,
                            rw_join_map_assignment(map, map_index, words), words, error) ||
          !rw_table_add_entry_mod(out, mapped->parent_signature, residue, product, modulus,
                                  error)) {
        return false;
      }
      (*join_pairs)++;
    }
  }
  return true;
}
static bool solve_join_acc_mod(const qsop_instance_t *qsop, const rw_join_map_t *map,
                               const rw_table_t *left, const rw_table_t *right, uint64_t modulus,
                               rw_table_t *out, size_t words, uint64_t *join_pairs,
                               qsop_error_t *error) {
  if (map->len == 0) {
    return true;
  }

  uint32_t max_parent_sig = 0, max_left_sig = 0, max_right_sig = 0;
  for (size_t m = 0; m < map->len; m++) {
    const rw_join_map_entry_t *me = &map->entries[m];
    if (me->parent_signature > max_parent_sig)
      max_parent_sig = me->parent_signature;
    if (me->left_signature > max_left_sig)
      max_left_sig = me->left_signature;
    if (me->right_signature > max_right_sig)
      max_right_sig = me->right_signature;
  }
  const size_t n_sigs = (size_t)max_parent_sig + 1U;
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t r = (uint32_t)qsop->r;

  uint64_t *acc = calloc(n_sigs * r, sizeof(*acc));
  uint32_t *sig_map_idx = malloc(n_sigs * sizeof(*sig_map_idx));
  uint32_t *left_starts = NULL;
  uint32_t *left_ends = NULL;
  uint32_t *right_starts = NULL;
  uint32_t *right_ends = NULL;
  if (acc == NULL || sig_map_idx == NULL ||
      !rw_build_sig_range_index(left, max_left_sig, &left_starts, &left_ends, error) ||
      !rw_build_sig_range_index(right, max_right_sig, &right_starts, &right_ends, error)) {
    free(acc);
    free(sig_map_idx);
    free(left_starts);
    free(left_ends);
    free(right_starts);
    free(right_ends);
    if (acc == NULL || sig_map_idx == NULL) {
      qsop_set_error(error, "out of memory while allocating rankwidth join accumulator");
    }
    return false;
  }
  memset(sig_map_idx, 0xFF, n_sigs * sizeof(*sig_map_idx));

  for (size_t m = 0; m < map->len; m++) {
    const rw_join_map_entry_t *me = &map->entries[m];
    const uint32_t lsig = me->left_signature;
    const uint32_t rsig = me->right_signature;
    const uint32_t ps = me->parent_signature;
    size_t l_start = 0;
    size_t l_end = 0;
    if (left_starts[lsig] != UINT32_MAX) {
      l_start = left_starts[lsig];
      l_end = left_ends[lsig];
    }
    size_t r_start = 0;
    size_t r_end = 0;
    if (right_starts[rsig] != UINT32_MAX) {
      r_start = right_starts[rsig];
      r_end = right_ends[rsig];
    }
    if (sig_map_idx[ps] == UINT32_MAX) {
      sig_map_idx[ps] = (uint32_t)m;
    }
    for (size_t i = l_start; i < l_end; i++) {
      const uint32_t l_res = left->entries[i].residue;
      const uint64_t l_cnt = left->entries[i].count;
      for (size_t j = r_start; j < r_end; j++) {
        const uint32_t res =
            (uint32_t)(((uint64_t)l_res + right->entries[j].residue + me->residue_shift) % r);
        const uint64_t product = qsop_mod_mul_u64(l_cnt, right->entries[j].count, modulus);
        acc[(size_t)ps * r + res] = qsop_mod_add_u64(acc[(size_t)ps * r + res], product, modulus);
        (*join_pairs)++;
      }
    }
  }

  size_t nonzero_count = 0;
  for (size_t i = 0; i < n_sigs * r; i++) {
    if (acc[i] != 0) {
      nonzero_count++;
    }
  }
  if (!rw_reserve_entries(out, out->len + nonzero_count, error)) {
    free(acc);
    free(sig_map_idx);
    free(left_starts);
    free(left_ends);
    free(right_starts);
    free(right_ends);
    return false;
  }
  for (uint32_t s = 0; s < (uint32_t)n_sigs; s++) {
    if (sig_map_idx[s] == UINT32_MAX) {
      continue;
    }
    const size_t m = sig_map_idx[s];
    if (!rw_table_add_rep(out, s, rw_join_map_assignment(map, m, words), words, error)) {
      free(acc);
      free(sig_map_idx);
      free(left_starts);
      free(left_ends);
      free(right_starts);
      free(right_ends);
      return false;
    }
    for (uint32_t res = 0; res < r; res++) {
      const uint64_t cnt = acc[(size_t)s * r + res];
      if (cnt == 0) {
        continue;
      }
      out->entries[out->len++] = (rw_entry_t){.signature = s, .residue = res, .count = cnt};
    }
  }

  free(acc);
  free(sig_map_idx);
  free(left_starts);
  free(left_ends);
  free(right_starts);
  free(right_ends);
  return true;
}
static bool fourier_table_signature_index(rw_fourier_table_t *table, uint32_t signature,
                                          const uint64_t *assignment, uint32_t value_slots,
                                          size_t words, size_t *out, qsop_error_t *error) {
  if (table->signature_slots == NULL ||
      (table->len + 1U) * 2U > (table->signature_slots_mask + 1U)) {
    if (!rw_fourier_slots_rehash(table, error)) {
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
  if (!rw_reserve_fourier_table(table, table->len + 1U, value_slots, words, error)) {
    return false;
  }
  const size_t index = table->len++;
  table->signatures[index] = signature;
  qsop_bitset_copy(rw_fourier_assignment(table, index, words), assignment, words);
  table->assignment_weights[index] = qsop_bitset_popcount(assignment, words);
  memset(&table->values[index * (size_t)value_slots], 0,
         (size_t)value_slots * sizeof(*table->values));
  table->signature_slots[slot] = (uint32_t)index;
  *out = index;
  return true;
}
static bool solve_fourier_leaf(const qsop_instance_t *qsop, const uint64_t *adj,
                               const rw_node_t *node, const uint64_t *powers, uint64_t prime,
                               size_t words, rw_signature_pool_t *pool, rw_fourier_table_t *table,
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
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this all-modes-Fourier path, which allocates O(r) structures below. */
  const uint32_t odd_modes = fourier_odd_mode_count((uint32_t)qsop->r);
  size_t zero = 0;
  size_t one = 0;
  if (!rw_signature_pool_intern(pool, zero_bits, &zero_signature, error) ||
      !rw_signature_pool_intern(pool, signature_bits_buffer, &one_signature, error) ||
      !fourier_table_signature_index(table, zero_signature, zero_bits, odd_modes, words, &zero,
                                     error) ||
      !fourier_table_signature_index(table, one_signature, one_bits, odd_modes, words, &one,
                                     error)) {
    return false;
  }
  for (uint32_t odd = 0; odd < odd_modes; odd++) {
    const uint32_t mode = 2U * odd + 1U;
    table->values[zero * (size_t)odd_modes + odd] =
        qsop_mod_add_u64(table->values[zero * (size_t)odd_modes + odd], 1, prime);
    table->values[one * (size_t)odd_modes + odd] = qsop_mod_add_u64(
        table->values[one * (size_t)odd_modes + odd],
        powers[(size_t)mode * qsop->r + (qsop->unary[node->var] % qsop->r)], prime);
  }
  return true;
}
static bool fourier_table_find_signature(const rw_fourier_table_t *table, uint32_t signature,
                                         size_t *out);
static bool dense_basis_from_fourier_table(const rw_fourier_table_t *table,
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
static bool fourier_values_nonzero(const uint64_t *values, uint32_t slots) {
  for (uint32_t i = 0; i < slots; i++) {
    if (values[i] != 0) {
      return true;
    }
  }
  return false;
}
static bool solve_fourier_join_dense_reference(const qsop_instance_t *qsop, const uint64_t *adj,
                                               rw_signature_pool_t *pool,
                                               const rw_fourier_table_t *left,
                                               const rw_fourier_table_t *right, uint64_t prime,
                                               rw_fourier_table_t *out, const uint64_t *outside,
                                               uint64_t *scratch_sig, uint64_t *parent_assignment,
                                               size_t words, uint64_t *join_signature_pairs,
                                               qsop_error_t *error) {
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this all-modes-Fourier path, which allocates O(r) structures below. */
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  const uint32_t odd_modes = fourier_odd_mode_count((uint32_t)qsop->r);
  const size_t w = words == 0 ? 1U : words;
  uint64_t *basis_scratch = calloc(w, sizeof(*basis_scratch));
  if (basis_scratch == NULL) {
    qsop_set_error(error, "out of memory while allocating dense rankwidth scratch");
    return false;
  }

  rw_dense_basis_t left_basis = {0};
  rw_dense_basis_t right_basis = {0};
  if (!dense_basis_from_fourier_table(left, pool, qsop->nvars, &left_basis, basis_scratch, error) ||
      !dense_basis_from_fourier_table(right, pool, qsop->nvars, &right_basis, basis_scratch,
                                      error)) {
    rw_dense_basis_free(&left_basis);
    rw_dense_basis_free(&right_basis);
    free(basis_scratch);
    return false;
  }

  size_t left_value_count = 0;
  size_t right_value_count = 0;
  if (!rw_dense_reference_value_count(left_basis.dim, odd_modes, &left_value_count, error) ||
      !rw_dense_reference_value_count(right_basis.dim, odd_modes, &right_value_count, error)) {
    rw_dense_basis_free(&left_basis);
    rw_dense_basis_free(&right_basis);
    free(basis_scratch);
    return false;
  }
  const size_t left_signatures = (size_t)1U << left_basis.dim;
  const size_t right_signatures = (size_t)1U << right_basis.dim;

  uint64_t *left_dense = calloc(left_value_count == 0 ? 1U : left_value_count, sizeof(*left_dense));
  uint64_t *right_dense =
      calloc(right_value_count == 0 ? 1U : right_value_count, sizeof(*right_dense));
  uint32_t *left_index = malloc(left_signatures * sizeof(*left_index));
  uint32_t *right_index = malloc(right_signatures * sizeof(*right_index));
  if (left_dense == NULL || right_dense == NULL || left_index == NULL || right_index == NULL) {
    free(left_dense);
    free(right_dense);
    free(left_index);
    free(right_index);
    rw_dense_basis_free(&left_basis);
    rw_dense_basis_free(&right_basis);
    free(basis_scratch);
    qsop_set_error(error, "out of memory while allocating dense-reference rankwidth tables");
    return false;
  }
  memset(left_index, 0xFF, left_signatures * sizeof(*left_index));
  memset(right_index, 0xFF, right_signatures * sizeof(*right_index));

  for (size_t i = 0; i < left->len; i++) {
    uint64_t coord = 0;
    if (!rw_dense_basis_coord(&left_basis, rw_signature_bits(pool, left->signatures[i]),
                              basis_scratch, &coord, error)) {
      free(left_dense);
      free(right_dense);
      free(left_index);
      free(right_index);
      rw_dense_basis_free(&left_basis);
      rw_dense_basis_free(&right_basis);
      free(basis_scratch);
      return false;
    }
    left_index[coord] = (uint32_t)i;
    memcpy(&left_dense[(size_t)coord * odd_modes], &left->values[i * (size_t)odd_modes],
           (size_t)odd_modes * sizeof(*left_dense));
  }
  for (size_t i = 0; i < right->len; i++) {
    uint64_t coord = 0;
    if (!rw_dense_basis_coord(&right_basis, rw_signature_bits(pool, right->signatures[i]),
                              basis_scratch, &coord, error)) {
      free(left_dense);
      free(right_dense);
      free(left_index);
      free(right_index);
      rw_dense_basis_free(&left_basis);
      rw_dense_basis_free(&right_basis);
      free(basis_scratch);
      return false;
    }
    right_index[coord] = (uint32_t)i;
    memcpy(&right_dense[(size_t)coord * odd_modes], &right->values[i * (size_t)odd_modes],
           (size_t)odd_modes * sizeof(*right_dense));
  }

  for (size_t lc = 0; lc < left_signatures; lc++) {
    const uint32_t li = left_index[lc];
    if (li == UINT32_MAX) {
      continue;
    }
    const uint64_t *left_values = &left_dense[lc * (size_t)odd_modes];
    if (!fourier_values_nonzero(left_values, odd_modes)) {
      continue;
    }
    const uint64_t *left_rep = rw_fourier_assignment(left, li, words);
    for (size_t rc = 0; rc < right_signatures; rc++) {
      const uint32_t ri = right_index[rc];
      if (ri == UINT32_MAX) {
        continue;
      }
      const uint64_t *right_values = &right_dense[rc * (size_t)odd_modes];
      if (!fourier_values_nonzero(right_values, odd_modes)) {
        continue;
      }

      const uint64_t *right_rep = rw_fourier_assignment(right, ri, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(
              qsop->nvars, adj, pool, outside, words, (uint32_t)qsop->r, left->signatures[li],
              left_rep, left->assignment_weights[li], right->signatures[ri], right_rep,
              right->assignment_weights[ri], scratch_sig, &eval, error)) {
        free(left_dense);
        free(right_dense);
        free(left_index);
        free(right_index);
        rw_dense_basis_free(&left_basis);
        rw_dense_basis_free(&right_basis);
        free(basis_scratch);
        return false;
      }
      if (!eval.valid) {
        continue;
      }

      size_t out_index = 0;
      if (!fourier_table_find_signature(out, eval.parent_signature, &out_index)) {
        qsop_bitset_copy(parent_assignment, left_rep, words);
        qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
        if (!fourier_table_signature_index(out, eval.parent_signature, parent_assignment, odd_modes,
                                           words, &out_index, error)) {
          free(left_dense);
          free(right_dense);
          free(left_index);
          free(right_index);
          rw_dense_basis_free(&left_basis);
          rw_dense_basis_free(&right_basis);
          free(basis_scratch);
          return false;
        }
      }
      uint64_t *out_values = &out->values[out_index * (size_t)odd_modes];
      const bool negate_odd_modes = eval.residue_shift != 0;
      for (uint32_t odd = 0; odd < odd_modes; odd++) {
        const uint64_t left_value = left_values[odd];
        const uint64_t right_value = right_values[odd];
        if (left_value == 0 || right_value == 0) {
          continue;
        }
        uint64_t value = qsop_mod_mul_u64(left_value, right_value, prime);
        if (negate_odd_modes && value != 0) {
          value = prime - value;
        }
        out_values[odd] = qsop_mod_add_u64(out_values[odd], value, prime);
      }
      if (join_signature_pairs != NULL) {
        (*join_signature_pairs)++;
      }
    }
  }

  free(left_dense);
  free(right_dense);
  free(left_index);
  free(right_index);
  rw_dense_basis_free(&left_basis);
  rw_dense_basis_free(&right_basis);
  free(basis_scratch);
  return true;
}
static bool solve_fourier_join_streaming(const qsop_instance_t *qsop, const uint64_t *adj,
                                         rw_signature_pool_t *pool, const rw_fourier_table_t *left,
                                         const rw_fourier_table_t *right, uint64_t prime,
                                         rw_fourier_table_t *out, const uint64_t *outside,
                                         uint64_t *scratch_sig, uint64_t *parent_assignment,
                                         size_t words, uint64_t *join_signature_pairs,
                                         qsop_error_t *error) {
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this all-modes-Fourier path, which allocates O(r) structures below. */
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  const uint32_t r = (uint32_t)qsop->r;
  const uint32_t odd_modes = fourier_odd_mode_count(r);
  for (size_t i = 0; i < left->len; i++) {
    const uint64_t *left_rep = rw_fourier_assignment(left, i, words);
    const uint64_t *left_values = &left->values[i * (size_t)odd_modes];
    for (size_t j = 0; j < right->len; j++) {
      const uint64_t *right_rep = rw_fourier_assignment(right, j, words);
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
      if (!fourier_table_find_signature(out, eval.parent_signature, &out_index)) {
        qsop_bitset_copy(parent_assignment, left_rep, words);
        qsop_bitset_or_simd(parent_assignment, right_rep, words, simd);
        if (!fourier_table_signature_index(out, eval.parent_signature, parent_assignment, odd_modes,
                                           words, &out_index, error)) {
          return false;
        }
      }
      const uint64_t *right_values = &right->values[j * (size_t)odd_modes];
      uint64_t *out_values = &out->values[out_index * (size_t)odd_modes];
      const bool negate_odd_modes = eval.residue_shift != 0;
      for (uint32_t odd = 0; odd < odd_modes; odd++) {
        const uint64_t left_value = left_values[odd];
        const uint64_t right_value = right_values[odd];
        if (left_value == 0 || right_value == 0) {
          continue;
        }
        uint64_t value = qsop_mod_mul_u64(left_value, right_value, prime);
        if (negate_odd_modes && value != 0) {
          value = prime - value;
        }
        out_values[odd] = qsop_mod_add_u64(out_values[odd], value, prime);
      }
      if (join_signature_pairs != NULL) {
        (*join_signature_pairs)++;
      }
    }
  }
  return true;
}
static uint64_t fourier_factorized_mode_value(const qsop_instance_t *qsop, const uint64_t *powers,
                                              uint64_t prime, uint32_t mode) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t acc = 1;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    const uint32_t residue = (uint32_t)(qsop->unary[v] % r32);
    const uint64_t term = qsop_mod_add_u64(1, powers[(size_t)mode * r32 + residue], prime);
    acc = qsop_mod_mul_u64(acc, term, prime);
  }
  return acc;
}
static void fourier_fill_root_modes_sign(const qsop_instance_t *qsop,
                                         const rw_fourier_table_t *root_table,
                                         const uint64_t *powers, uint64_t prime, uint64_t *modes) {
  const uint32_t r32 = (uint32_t)qsop->r;
  qsop_counts_clear(r32, modes);
  const uint32_t odd_modes = fourier_odd_mode_count(r32);
  size_t root_index = 0;
  if (fourier_table_find_signature(root_table, 0, &root_index)) {
    const uint64_t *odd_values = &root_table->values[root_index * (size_t)odd_modes];
    for (uint32_t odd = 0; odd < odd_modes; odd++) {
      modes[2U * odd + 1U] = odd_values[odd];
    }
  }
  for (uint32_t mode = 0; mode < r32; mode += 2U) {
    modes[mode] = fourier_factorized_mode_value(qsop, powers, prime, mode);
  }
}
static bool fourier_table_find_signature(const rw_fourier_table_t *table, uint32_t signature,
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
bool rw_solve_count_table_mod_once(const qsop_instance_t *qsop,
                                   const qsop_rankwidth_decomposition_t *decomposition,
                                   const uint64_t *adj, uint64_t modulus, uint64_t *counts,
                                   qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                   qsop_error_t *error) {
  uint32_t *linear_order = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*linear_order));
  if (linear_order == NULL) {
    qsop_set_error(error, "out of memory while allocating rankwidth linear order");
    return false;
  }
  if (rw_extract_left_deep_order(decomposition, linear_order)) {
    const bool ok = solve_rankwidth_linear_count_table_mod_once(
        qsop, decomposition, adj, linear_order, modulus, counts, stats, trace, error);
    free(linear_order);
    return ok;
  }
  free(linear_order);

  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  rw_signature_pool_t pool = {0};
  if (tables == NULL || !rw_signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    qsop_set_error(error, "out of memory while allocating modular rankwidth solve state");
    return false;
  }

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_mod(qsop, adj, node, decomposition->words, &pool, modulus, &tables[node_id],
                          error);
      qsop_trace_emit_elapsed(trace, "rankwidth.crt_leaf", 0, tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_join_map(qsop, decomposition, node_id, adj, &pool, &tables[node->left],
                          &tables[node->right], &map, error);
      if (ok) {
        join_signature_pairs += map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join_mod(qsop, &map, &tables[node->left], &tables[node->right], modulus,
                            &tables[node_id], decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join", 0, tables[node_id].len, join_start);
      }
      rw_join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
  }

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (uint32_t)((root->entries[i].residue + qsop->constant) % qsop->r);
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    rw_table_free(&tables[t]);
  }
  free(tables);
  rw_signature_pool_free(&pool);
  return true;
}
static bool solve_sign_edge_crt_build_maps(const qsop_instance_t *qsop,
                                           const qsop_rankwidth_decomposition_t *decomposition,
                                           const uint64_t *adj, uint64_t modulus, uint64_t *counts,
                                           rw_signature_pool_t *pool, rw_join_map_t *maps,
                                           uint64_t *scratch, qsop_solve_stats_t *stats,
                                           qsop_solve_trace_t *trace, qsop_error_t *error) {
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_table_t *tables = calloc(nnodes, sizeof(*tables));
  if (tables == NULL) {
    qsop_set_error(error, "out of memory in sign-edge CRT transition build");
    return false;
  }
  uint64_t join_pairs = 0, join_signature_pairs = 0;
  uint64_t table_entries = 0, signature_entries = 0;
  uint64_t max_table_entries = 0, max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_mod(qsop, adj, node, decomposition->words, pool, modulus, &tables[node_id],
                          error);
      qsop_trace_emit_elapsed(trace, "rankwidth.crt_leaf", 0, tables[node_id].len, start);
    } else {
      maps[node_id].len = 0;
      ok = build_join_map_arena(qsop, decomposition, node_id, adj, pool, &tables[node->left],
                                &tables[node->right], &maps[node_id], scratch, error);
      if (ok) {
        join_signature_pairs += maps[node_id].len;
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join_map", 0, maps[node_id].len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok =
            solve_join_acc_mod(qsop, &maps[node_id], &tables[node->left], &tables[node->right],
                               modulus, &tables[node_id], decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join", 0, tables[node_id].len, join_start);
      }
    }
    if (!ok) {
      for (uint32_t t = 0; t < nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
  }
  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (uint32_t)((root->entries[i].residue + qsop->constant) % qsop->r);
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }
  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    rw_table_free(&tables[t]);
  }
  free(tables);
  return true;
}
static bool solve_sign_edge_crt_use_maps(const qsop_instance_t *qsop,
                                         const qsop_rankwidth_decomposition_t *decomposition,
                                         const uint64_t *adj, uint64_t modulus, uint64_t *counts,
                                         rw_signature_pool_t *pool, const rw_join_map_t *maps,
                                         qsop_error_t *error) {
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_table_t *tables = calloc(nnodes, sizeof(*tables));
  if (tables == NULL) {
    qsop_set_error(error, "out of memory in sign-edge CRT cached pass");
    return false;
  }
  uint64_t join_pairs = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_mod(qsop, adj, node, decomposition->words, pool, modulus, &tables[node_id],
                          error);
    } else {
      ok = solve_join_acc_mod(qsop, &maps[node_id], &tables[node->left], &tables[node->right],
                              modulus, &tables[node_id], decomposition->words, &join_pairs, error);
    }
    if (!ok) {
      for (uint32_t t = 0; t < nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
  }
  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (uint32_t)((root->entries[i].residue + qsop->constant) % qsop->r);
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    rw_table_free(&tables[t]);
  }
  free(tables);
  return true;
}
static bool solve_rankwidth_count_table_crt(const qsop_instance_t *qsop,
                                            const qsop_rankwidth_decomposition_t *decomposition,
                                            const uint64_t *adj, qsop_result_t **out,
                                            qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                            qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    free(primes);
    qsop_set_error(error, "rankwidth CRT count table is too large");
    return false;
  }
  uint64_t *all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (all_counts == NULL || residues == NULL || result == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory for rankwidth CRT state");
    return false;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(r32, sizeof(*result->count_strings));
  if (result->count_strings == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory for rankwidth CRT result strings");
    return false;
  }
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  rw_signature_pool_t pool = {0};
  rw_join_map_t *maps = calloc(nnodes, sizeof(*maps));
  uint64_t *scratch = calloc(3U * w, sizeof(*scratch));
  if (maps == NULL || scratch == NULL ||
      !rw_signature_pool_init(&pool, decomposition->words, error)) {
    free(scratch);
    free(maps);
    if (pool.bits != NULL) {
      rw_signature_pool_free(&pool);
    }
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory for rankwidth CRT transition cache");
    return false;
  }
  bool ok = solve_sign_edge_crt_build_maps(qsop, decomposition, adj, primes[0], &all_counts[0],
                                           &pool, maps, scratch, stats, trace, error);
  for (size_t p = 1; p < nprimes && ok; p++) {
    ok = solve_sign_edge_crt_use_maps(qsop, decomposition, adj, primes[p],
                                      &all_counts[p * (size_t)r32], &pool, maps, error);
  }
  if (!ok) {
    for (uint32_t t = 0; t < nnodes; t++) {
      rw_join_map_free(&maps[t]);
    }
    free(maps);
    free(scratch);
    rw_signature_pool_free(&pool);
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    return false;
  }
  for (uint32_t residue = 0; residue < r32; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)r32 + residue];
    }
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                      error)) {
      for (uint32_t t = 0; t < nnodes; t++) {
        rw_join_map_free(&maps[t]);
      }
      free(maps);
      free(scratch);
      rw_signature_pool_free(&pool);
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    rw_join_map_free(&maps[t]);
  }
  free(maps);
  free(scratch);
  rw_signature_pool_free(&pool);
  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}
static void rankwidth_no_edges_stats(const qsop_instance_t *qsop, qsop_solve_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  const uint64_t table_entries = qsop->nvars == 0 ? 1U : qsop->r;
  stats->table_entries = table_entries;
  stats->max_table_entries = table_entries;
  stats->signature_entries = 1;
  stats->max_signature_entries = 1;
  stats->join_pairs = 0;
  stats->join_signature_pairs = 0;
  stats->rankwidth_table_forecast = table_entries;
  stats->rankwidth_join_pair_forecast = 0;
  stats->rankwidth_dense_table_forecast = qsop->r;
  stats->rankwidth_dense_even_join_forecast = 0;
  stats->rankwidth_transition_bytes = 0;
  stats->rankwidth_transition_layout_u16_events = 0;
  stats->rankwidth_transition_layout_u32_events = 0;
  stats->rankwidth_materialized_join_events = 0;
  stats->rankwidth_streaming_join_events = 0;
  stats->rankwidth_streaming_join_candidate_pairs = 0;
  stats->rankwidth_streaming_join_emitted_pairs = 0;
  stats->rankwidth_linear_transition_events = 0;
  stats->rankwidth_table_assignment_bytes = 0;
  stats->decomposition_width = 0;
  stats->rankwidth_cutrank_width = 0;
}
static bool rankwidth_no_edges_add_count(uint64_t *dst, uint64_t value, uint64_t count_modulus,
                                         qsop_error_t *error) {
  if (count_modulus != 0) {
    *dst = qsop_mod_add_u64(*dst, value % count_modulus, count_modulus);
    return true;
  }
  return qsop_count_add(dst, value, error);
}
bool rw_solve_no_edges_count_table_mod_once(const qsop_instance_t *qsop, uint64_t count_modulus,
                                            uint64_t *counts, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (count_modulus == 0 && qsop->nvars >= 64U) {
    qsop_set_error(error,
                   "rankwidth exact no-edge count-table handoff requires fewer than 64 variables");
    return false;
  }
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *current = NULL;
  uint64_t *next = NULL;
  if (!qsop_counts_alloc(r32, &current, error) || !qsop_counts_alloc(r32, &next, error)) {
    free(current);
    free(next);
    return false;
  }

  const uint64_t start = qsop_trace_begin(trace);
  current[0] = count_modulus == 0 ? 1U : 1U % count_modulus;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    qsop_counts_clear(r32, next);
    const uint32_t shift = (uint32_t)(qsop->unary[v] % r32);
    for (uint32_t residue = 0; residue < r32; residue++) {
      const uint64_t value = current[residue];
      if (value == 0) {
        continue;
      }
      uint32_t shifted = residue + shift;
      if (shifted >= r32) {
        shifted -= r32;
      }
      if (!rankwidth_no_edges_add_count(&next[residue], value, count_modulus, error) ||
          !rankwidth_no_edges_add_count(&next[shifted], value, count_modulus, error)) {
        free(current);
        free(next);
        return false;
      }
    }
    uint64_t *tmp = current;
    current = next;
    next = tmp;
  }

  qsop_counts_clear(r32, counts);
  const uint32_t constant = (uint32_t)(qsop->constant % r32);
  for (uint32_t residue = 0; residue < r32; residue++) {
    if (current[residue] == 0) {
      continue;
    }
    uint32_t shifted = residue + constant;
    if (shifted >= r32) {
      shifted -= r32;
    }
    if (!rankwidth_no_edges_add_count(&counts[shifted], current[residue], count_modulus, error)) {
      free(current);
      free(next);
      return false;
    }
  }
  qsop_trace_emit_elapsed(trace, "rankwidth.count_table_factorized", 0,
                          qsop_saturating_mul_u64(qsop->nvars, r32), start);
  rankwidth_no_edges_stats(qsop, stats);
  free(current);
  free(next);
  return true;
}
static bool solve_rankwidth_no_edges_count_table_crt(const qsop_instance_t *qsop,
                                                     qsop_result_t **out, qsop_solve_stats_t *stats,
                                                     qsop_solve_trace_t *trace,
                                                     qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    free(primes);
    qsop_set_error(error, "rankwidth no-edge CRT count table is too large");
    return false;
  }

  uint64_t *all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (all_counts == NULL || residues == NULL || result == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth no-edge CRT state");
    return false;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(r32, sizeof(*result->count_strings));
  if (result->count_strings == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth no-edge CRT result strings");
    return false;
  }

  bool ok = true;
  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!rw_solve_no_edges_count_table_mod_once(qsop, primes[p], &all_counts[p * (size_t)r32],
                                                stats_for_prime, trace_for_prime, error)) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    return false;
  }

  for (uint32_t residue = 0; residue < r32; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)r32 + residue];
    }
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                      error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}
bool rw_solve_no_edges_count_table(const qsop_instance_t *qsop, qsop_result_t **out,
                                   qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                   qsop_error_t *error) {
  if (qsop->nvars >= 64U) {
    return solve_rankwidth_no_edges_count_table_crt(qsop, out, stats, trace, error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL || !qsop_counts_alloc((uint32_t)qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth no-edge result");
    return false;
  }
  result->r = (uint32_t)qsop->r;
  result->norm_h = qsop->norm_h;
  if (!rw_solve_no_edges_count_table_mod_once(qsop, 0, result->counts, stats, trace, error)) {
    qsop_result_free(result);
    return false;
  }
  *out = result;
  return true;
}
bool rw_solve_count_table(const qsop_instance_t *qsop,
                          const qsop_rankwidth_decomposition_t *decomposition, const uint64_t *adj,
                          qsop_rankwidth_join_strategy_t join_strategy,
                          uint64_t materialize_join_max_pairs, qsop_result_t **out,
                          qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                          qsop_error_t *error) {
  if (qsop->nedges == 0) {
    (void)decomposition;
    (void)adj;
    (void)join_strategy;
    (void)materialize_join_max_pairs;
    return rw_solve_no_edges_count_table(qsop, out, stats, trace, error);
  }
  if (join_strategy == QSOP_RANKWIDTH_JOIN_AUTO) {
    uint32_t *linear_order = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*linear_order));
    if (linear_order == NULL) {
      qsop_set_error(error, "out of memory while allocating rankwidth linear order");
      return false;
    }
    if (rw_extract_left_deep_order(decomposition, linear_order)) {
      const bool ok = solve_rankwidth_linear_count_table(qsop, decomposition, adj, linear_order,
                                                         out, stats, trace, error);
      free(linear_order);
      return ok;
    }
    free(linear_order);
  }
  if (qsop->nvars >= 64U) {
    return solve_rankwidth_count_table_crt(qsop, decomposition, adj, out, stats, trace, error);
  }

  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this count-table path, which allocates O(r) structures below. */
  const uint32_t r32 = (uint32_t)qsop->r;
  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(r32, &result->counts, error)) {
    free(tables);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth solve state");
    return false;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;

  rw_signature_pool_t pool = {0};
  if (!rw_signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    qsop_result_free(result);
    return false;
  }

  /* Arena: 3 scratch bitsets (zero | assignment | sig_temp) shared across all nodes. */
  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  uint64_t *scratch = calloc(3U * w, sizeof(*scratch));
  if (scratch == NULL) {
    for (uint32_t t = 0; t < decomposition->nnodes; t++)
      rw_table_free(&tables[t]);
    free(tables);
    rw_signature_pool_free(&pool);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth scratch");
    return false;
  }

  /* Precompute per-node outside bitsets and scratch_sig buffer (reuse scratch[2w]). */
  uint64_t *scratch_sig = scratch + 2U * w; /* scratch[2w..3w-1]: sig temp for transitions */

  /* Pre-allocate join workspace for CSR path (amortizes malloc over all join nodes). */
  const size_t ws_cap_sigs = decomposition->score_cached && decomposition->cached_table_forecast > 0
                                 ? (size_t)(decomposition->cached_table_forecast / r32) + 1U
                                 : 0U;
  rw_join_workspace_t join_ws = {0};
  if (ws_cap_sigs > 0 && !join_workspace_alloc(ws_cap_sigs, r32, &join_ws, error)) {
    free(scratch);
    for (uint32_t t = 0; t < decomposition->nnodes; t++)
      rw_table_free(&tables[t]);
    free(tables);
    rw_signature_pool_free(&pool);
    qsop_result_free(result);
    return false;
  }

  /* Outside bitset per join node (scratch[0..w-1] reused as temp; allocated per node). */
  uint64_t *outside = calloc(w, sizeof(*outside));
  if (outside == NULL) {
    join_workspace_free(&join_ws);
    free(scratch);
    for (uint32_t t = 0; t < decomposition->nnodes; t++)
      rw_table_free(&tables[t]);
    free(tables);
    rw_signature_pool_free(&pool);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth outside bitset");
    return false;
  }

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  uint64_t transition_bytes = 0;
  uint64_t u16_events = 0;
  uint64_t u32_events = 0;
  uint64_t materialized_join_events = 0;
  uint64_t streaming_join_events = 0;
  uint64_t streaming_candidate_pairs = 0;
  uint64_t streaming_emitted_pairs = 0;

  const uint64_t max_pairs = (materialize_join_max_pairs > 0)
                                 ? materialize_join_max_pairs
                                 : RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT;

  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      /* Zero scratch[0] (zero bitset) and scratch[w] (assignment) before each leaf. */
      memset(scratch, 0, 2U * w * sizeof(*scratch));
      ok = solve_leaf_arena(qsop, adj, node, decomposition->words, &pool, &tables[node_id], scratch,
                            error);
      qsop_trace_emit_elapsed(trace, "rankwidth.leaf", 0, tables[node_id].len, start);
    } else {
      /* Build outside bitset for this join node. */
      rw_fill_all_vars(outside, decomposition->nvars, decomposition->words);
      qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), decomposition->words);

      /* Forecast pair count: left_reps × right_reps. */
      const size_t lreps = tables[node->left].reps_len;
      const size_t rreps = tables[node->right].reps_len;
      const uint64_t pair_forecast =
          (lreps > 0 && rreps > UINT64_MAX / lreps) ? UINT64_MAX : (uint64_t)lreps * rreps;

      /* Select strategy for this node. */
      const bool do_streaming =
          (join_strategy == QSOP_RANKWIDTH_JOIN_STREAMING) ||
          (join_strategy == QSOP_RANKWIDTH_JOIN_AUTO && pair_forecast > max_pairs);

      if (do_streaming) {
        streaming_join_events++;
        memset(scratch_sig, 0, w * sizeof(*scratch_sig));
        uint64_t cand = 0, emit = 0;
        ok = rw_join_count_table_streaming_sign(
            qsop, decomposition, node_id, adj, &pool, &tables[node->left], &tables[node->right],
            &tables[node_id], outside, scratch_sig, decomposition->words, &cand, &emit, error);
        streaming_candidate_pairs += cand;
        streaming_emitted_pairs += emit;
        join_pairs += emit;
        join_signature_pairs += emit;
        qsop_trace_emit_elapsed(trace, "rankwidth.streaming_join", 0, tables[node_id].len, start);
      } else {
        /* D4.1: CSR materialized path. */
        rw_transition_csr_t csr = {0};
        memset(scratch_sig, 0, w * sizeof(*scratch_sig));
        ok = rw_transition_csr_build_sign(qsop, decomposition, node_id, adj, &pool,
                                          &tables[node->left], &tables[node->right], outside,
                                          scratch_sig, &csr, &u16_events, &u32_events, error);
        if (ok) {
          join_signature_pairs += csr.transition_count;
          transition_bytes += rw_transition_csr_bytes(&csr);
          materialized_join_events++;
          qsop_trace_emit_elapsed(trace, "rankwidth.join_map", 0, csr.transition_count, start);
          const uint64_t join_start = qsop_trace_begin(trace);
          memset(scratch_sig, 0, w * sizeof(*scratch_sig));
          ok = rw_execute_csr_join_sign(qsop, &csr, &tables[node->left], &tables[node->right],
                                        &tables[node_id], decomposition->words, &join_pairs,
                                        join_ws.acc ? &join_ws : NULL, error);
          qsop_trace_emit_elapsed(trace, "rankwidth.join", 0, tables[node_id].len, join_start);
        }
        rw_transition_csr_free(&csr);
      }
    }
    if (!ok) {
      join_workspace_free(&join_ws);
      free(scratch);
      free(outside);
      for (uint32_t t = 0; t < decomposition->nnodes; t++)
        rw_table_free(&tables[t]);
      free(tables);
      rw_signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
    rw_table_sort(&tables[node_id]);
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries)
      max_table_entries = tables[node_id].len;
    if (tables[node_id].reps_len > max_signature_entries)
      max_signature_entries = tables[node_id].reps_len;
  }

  join_workspace_free(&join_ws);
  free(scratch);
  free(outside);

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (uint32_t)((root->entries[i].residue + qsop->constant) % qsop->r);
    if (!qsop_count_add(&result->counts[residue], root->entries[i].count, error)) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      rw_signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->rankwidth_transition_bytes += transition_bytes;
    stats->rankwidth_transition_layout_u16_events += u16_events;
    stats->rankwidth_transition_layout_u32_events += u32_events;
    stats->rankwidth_materialized_join_events += materialized_join_events;
    stats->rankwidth_streaming_join_events += streaming_join_events;
    stats->rankwidth_streaming_join_candidate_pairs += streaming_candidate_pairs;
    stats->rankwidth_streaming_join_emitted_pairs += streaming_emitted_pairs;
    stats->rankwidth_table_assignment_bytes =
        (uint64_t)signature_entries * decomposition->words * sizeof(uint64_t);
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_table_free(&tables[t]);
      }
      free(tables);
      rw_signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    rw_table_free(&tables[t]);
  }
  free(tables);
  rw_signature_pool_free(&pool);
  *out = result;
  return true;
}
static void rankwidth_constant_stats(const qsop_instance_t *qsop, qsop_solve_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  stats->table_entries = 1;
  stats->max_table_entries = 1;
  stats->signature_entries = 1;
  stats->max_signature_entries = 1;
  stats->join_pairs = 0;
  stats->join_signature_pairs = 0;
  stats->rankwidth_table_forecast = 1;
  stats->rankwidth_join_pair_forecast = 0;
  stats->rankwidth_dense_table_forecast = qsop->r;
  stats->rankwidth_dense_even_join_forecast = 0;
  stats->rankwidth_linear_transition_events = 0;
  stats->decomposition_width = 0;
  stats->rankwidth_cutrank_width = 0;
  (void)qsop;
}
bool rw_solve_constant_result(const qsop_instance_t *qsop, qsop_result_t **out,
                              qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                              qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL || !qsop_counts_alloc(r32, &result->counts, error)) {
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth constant result");
    return false;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;
  result->counts[qsop->constant % r32] = 1;
  rankwidth_constant_stats(qsop, stats);
  qsop_trace_emit(trace, "rankwidth.constant", 0, 1, 0);
  *out = result;
  return true;
}
bool rw_solve_constant_mod(const qsop_instance_t *qsop, uint64_t count_modulus, uint64_t *counts,
                           qsop_solve_stats_t *stats, qsop_solve_trace_t *trace) {
  const uint32_t r32 = (uint32_t)qsop->r;
  qsop_counts_clear(r32, counts);
  counts[qsop->constant % r32] = count_modulus == 0 ? 1 : 1 % count_modulus;
  rankwidth_constant_stats(qsop, stats);
  qsop_trace_emit(trace, "rankwidth.constant", 0, 1, 0);
  return true;
}
static bool rankwidth_fourier_prime_state(uint32_t r, uint64_t prime, uint64_t *root_out,
                                          uint64_t **powers_out, uint64_t **inv_powers_out,
                                          qsop_error_t *error) {
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;
  if (!qsop_fourier_find_order_root(prime, r, &root, error)) {
    return false;
  }
  inv_root = qsop_mod_pow_u64(root, prime - 2U, prime);
  if (!qsop_fourier_make_root_powers(r, root, prime, &powers, error) ||
      !qsop_fourier_make_root_powers(r, inv_root, prime, &inv_powers, error)) {
    free(powers);
    free(inv_powers);
    return false;
  }
  *root_out = root;
  *powers_out = powers;
  *inv_powers_out = inv_powers;
  return true;
}
static bool solve_rankwidth_fourier_no_edges_mod_once(
    const qsop_instance_t *qsop, const uint64_t *powers, const uint64_t *inv_powers, uint64_t prime,
    qsop_rankwidth_fourier_kernel_t kernel, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *modes = calloc(r32, sizeof(*modes));
  if (modes == NULL) {
    qsop_set_error(error, "out of memory while allocating factorized rankwidth Fourier modes");
    return false;
  }
  const uint64_t start = qsop_trace_begin(trace);
  for (uint32_t mode = 0; mode < r32; mode++) {
    modes[mode] = fourier_factorized_mode_value(qsop, powers, prime, mode);
  }
  qsop_trace_emit_elapsed(trace, "rankwidth.fourier_factorized", 0, r32, start);
  const bool ok = qsop_fourier_inverse_counts(r32, modes, (uint32_t)qsop->constant, powers,
                                              inv_powers, prime, counts, error);
  free(modes);
  if (!ok) {
    return false;
  }
  if (stats != NULL) {
    rankwidth_no_edges_stats(qsop, stats);
    stats->rankwidth_fourier_kernel = (uint32_t)kernel;
  }
  return true;
}
static bool solve_rankwidth_fourier_mod_once(const qsop_instance_t *qsop,
                                             const qsop_rankwidth_decomposition_t *decomposition,
                                             const uint64_t *adj, uint64_t prime,
                                             const uint64_t *powers, const uint64_t *inv_powers,
                                             qsop_rankwidth_fourier_kernel_t kernel,
                                             uint64_t *counts, qsop_solve_stats_t *stats,
                                             qsop_solve_trace_t *trace, qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  if (qsop->nedges == 0) {
    (void)decomposition;
    (void)adj;
    return solve_rankwidth_fourier_no_edges_mod_once(qsop, powers, inv_powers, prime, kernel,
                                                     counts, stats, trace, error);
  }

  rw_fourier_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (tables == NULL) {
    free(tables);
    qsop_set_error(error, "out of memory while allocating rankwidth Fourier solve state");
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
    qsop_set_error(error, "out of memory while allocating rankwidth Fourier scratch");
    return false;
  }
  uint64_t *outside = scratch;
  uint64_t *scratch_sig = scratch + w;
  uint64_t *parent_assignment = scratch + 2U * w;
  uint64_t *leaf_zero = scratch + 3U * w;
  uint64_t *leaf_one = scratch + 4U * w;
  uint64_t *leaf_signature = scratch + 5U * w;

  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  const uint32_t odd_modes = fourier_odd_mode_count(r32);
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_fourier_leaf(qsop, adj, node, powers, prime, decomposition->words, &pool,
                              &tables[node_id], leaf_zero, leaf_one, leaf_signature, error);
      qsop_trace_emit_elapsed(trace, "rankwidth.fourier_leaf", 0, tables[node_id].len, start);
    } else {
      const size_t left_len = tables[node->left].len;
      const size_t right_len = tables[node->right].len;
      if (left_len > 0 && right_len > UINT64_MAX / left_len) {
        qsop_set_error(error, "rankwidth Fourier streaming join is too large");
        ok = false;
      } else {
        const uint64_t pair_forecast = (uint64_t)left_len * (uint64_t)right_len;
        rw_fill_all_vars(outside, decomposition->nvars, decomposition->words);
        qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), decomposition->words);
        qsop_trace_emit_elapsed(trace, "rankwidth.fourier_join_map", 0, pair_forecast, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        if (kernel == QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING) {
          ok = solve_fourier_join_streaming(qsop, adj, &pool, &tables[node->left],
                                            &tables[node->right], prime, &tables[node_id], outside,
                                            scratch_sig, parent_assignment, decomposition->words,
                                            &join_signature_pairs, error);
        } else if (kernel == QSOP_RANKWIDTH_FOURIER_KERNEL_DENSE_REFERENCE) {
          ok = solve_fourier_join_dense_reference(
              qsop, adj, &pool, &tables[node->left], &tables[node->right], prime, &tables[node_id],
              outside, scratch_sig, parent_assignment, decomposition->words, &join_signature_pairs,
              error);
        } else {
          qsop_set_error(error, "rankwidth Fourier kernel is not implemented for this join");
          ok = false;
        }
        qsop_trace_emit_elapsed(trace, "rankwidth.fourier_join", 0, tables[node_id].len,
                                join_start);
      }
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_fourier_table_free(&tables[t]);
      }
      free(scratch);
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
    const uint64_t node_entries = (uint64_t)tables[node_id].len * odd_modes;
    table_entries += node_entries;
    signature_entries += tables[node_id].len;
    if (node_entries > max_table_entries) {
      max_table_entries = node_entries;
    }
    if (tables[node_id].len > max_signature_entries) {
      max_signature_entries = tables[node_id].len;
    }
  }

  const rw_fourier_table_t *root_table = &tables[decomposition->root];
  uint64_t *root_modes = calloc(r32, sizeof(*root_modes));
  if (root_modes == NULL) {
    for (uint32_t t = 0; t < decomposition->nnodes; t++) {
      rw_fourier_table_free(&tables[t]);
    }
    free(scratch);
    free(tables);
    rw_signature_pool_free(&pool);
    qsop_set_error(error, "out of memory while allocating rankwidth Fourier root modes");
    return false;
  }
  const uint64_t even_start = qsop_trace_begin(trace);
  fourier_fill_root_modes_sign(qsop, root_table, powers, prime, root_modes);
  qsop_trace_emit_elapsed(trace, "rankwidth.fourier_even_closed_form", 0, (r32 + 1U) / 2U,
                          even_start);

  if (!qsop_fourier_inverse_counts(r32, root_modes, (uint32_t)qsop->constant, powers, inv_powers,
                                   prime, counts, error)) {
    for (uint32_t t = 0; t < decomposition->nnodes; t++) {
      rw_fourier_table_free(&tables[t]);
    }
    free(root_modes);
    free(scratch);
    free(tables);
    rw_signature_pool_free(&pool);
    return false;
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = qsop_saturating_mul_u64(join_signature_pairs, odd_modes);
    stats->join_signature_pairs = join_signature_pairs;
    stats->rankwidth_fourier_kernel = (uint32_t)kernel;
    stats->decomposition_width = rw_decomposition_width(decomposition, adj, stats, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        rw_fourier_table_free(&tables[t]);
      }
      free(scratch);
      free(tables);
      rw_signature_pool_free(&pool);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    rw_fourier_table_free(&tables[t]);
  }
  free(root_modes);
  free(scratch);
  free(tables);
  rw_signature_pool_free(&pool);
  return true;
}
static const char *rankwidth_fourier_kernel_internal_name(qsop_rankwidth_fourier_kernel_t kernel) {
  switch (kernel) {
  case QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO:
    return "auto";
  case QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING:
    return "streaming";
  case QSOP_RANKWIDTH_FOURIER_KERNEL_HYBRID_EVEN_FWHT:
    return "hybrid-even-fwht";
  case QSOP_RANKWIDTH_FOURIER_KERNEL_DENSE_REFERENCE:
    return "dense-reference";
  }
  return "unknown";
}
static bool rankwidth_fourier_kernel_resolve(qsop_rankwidth_fourier_kernel_t requested,
                                             qsop_rankwidth_fourier_kernel_t *resolved,
                                             qsop_error_t *error) {
  if (requested == QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO) {
    *resolved = QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING;
    return true;
  }
  if (requested == QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING) {
    *resolved = requested;
    return true;
  }
  if (requested == QSOP_RANKWIDTH_FOURIER_KERNEL_DENSE_REFERENCE) {
    *resolved = requested;
    return true;
  }
  qsop_set_error(error, "rankwidth Fourier kernel '%s' is not implemented yet",
                 rankwidth_fourier_kernel_internal_name(requested));
  return false;
}
bool rw_solve_fourier(const qsop_instance_t *qsop,
                      const qsop_rankwidth_decomposition_t *decomposition, const uint64_t *adj,
                      qsop_rankwidth_fourier_kernel_t requested_kernel, qsop_result_t **out,
                      qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  qsop_rankwidth_fourier_kernel_t kernel = QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING;
  if (!rankwidth_fourier_kernel_resolve(requested_kernel, &kernel, error)) {
    return false;
  }
  if (stats != NULL) {
    stats->rankwidth_fourier_kernel = (uint32_t)kernel;
  }
  /* Gated by qsop_solve_rankwidth_options_mode_trace_stats above to qsop->r <= UINT32_MAX
   * before reaching this all-modes-Fourier path, which allocates O(r) structures below. */
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_fourier_find_ntt_primes_for_nvars(r32, qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    free(primes);
    qsop_set_error(error, "rankwidth Fourier CRT count table is too large");
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  uint64_t *all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  if (result == NULL || all_counts == NULL || residues == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    qsop_set_error(error, "out of memory while allocating rankwidth Fourier CRT solve state");
    return false;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;
  if (nprimes == 1U) {
    if (!qsop_counts_alloc(r32, &result->counts, error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  } else {
    result->count_strings = calloc(r32, sizeof(*result->count_strings));
    if (result->count_strings == NULL) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      qsop_set_error(error, "out of memory while allocating rankwidth Fourier CRT result strings");
      return false;
    }
  }

  for (size_t p = 0; p < nprimes; p++) {
    uint64_t root = 0;
    uint64_t *powers = NULL;
    uint64_t *inv_powers = NULL;
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    const bool ok =
        rankwidth_fourier_prime_state(r32, primes[p], &root, &powers, &inv_powers, error) &&
        solve_rankwidth_fourier_mod_once(qsop, decomposition, adj, primes[p], powers, inv_powers,
                                         kernel, &all_counts[p * (size_t)r32], stats_for_prime,
                                         trace_for_prime, error);
    (void)root;
    free(powers);
    free(inv_powers);
    if (!ok) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  if (nprimes == 1U) {
    memcpy(result->counts, all_counts, (size_t)r32 * sizeof(*result->counts));
  } else {
    for (uint32_t residue = 0; residue < r32; residue++) {
      for (size_t p = 0; p < nprimes; p++) {
        residues[p] = all_counts[p * (size_t)r32 + residue];
      }
      if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                        error)) {
        free(primes);
        free(all_counts);
        free(residues);
        qsop_result_free(result);
        return false;
      }
    }
  }

  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}
