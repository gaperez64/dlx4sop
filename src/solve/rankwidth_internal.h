#ifndef DLX4SOP_RANKWIDTH_INTERNAL_H
#define DLX4SOP_RANKWIDTH_INTERNAL_H

/* Shared types and cross-translation-unit declarations for the rankwidth backend, split (pure
 * movement, no logic changes) across rankwidth_tables.c, rankwidth_decomp.c,
 * rankwidth_cutrank.c, rankwidth_dp_counts.c, rankwidth_dp_complex.c, and rankwidth_drivers.c.
 * Every function declared here keeps its original body verbatim; only `static` was dropped and
 * an `rw_` prefix added (if it didn't already carry one) where a definition moved to a
 * different translation unit than its callers. */

#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/simd.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define RW_JOIN_MAP_INITIAL_CAP 1024U
#define RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT UINT64_C(1000000)
#define RW_DENSE_REFERENCE_MAX_DIM 22U
#define RW_DENSE_REFERENCE_MAX_VALUES UINT64_C(4194304)
#define RW_SIG_HT_THRESHOLD 32U /* use linear scan below this; hash table above */
static inline const qsop_simd_vtable_t *rankwidth_bitset_simd(void) {
  static _Atomic(const qsop_simd_vtable_t *) cached;
  const qsop_simd_vtable_t *simd = atomic_load_explicit(&cached, memory_order_acquire);
  if (simd == NULL) {
    simd = qsop_simd_resolve(QSOP_SIMD_KERNEL_AUTO);
    atomic_store_explicit(&cached, simd, memory_order_release);
  }
  return simd;
}
typedef enum rw_node_kind {
  RW_NODE_UNDEFINED,
  RW_NODE_LEAF,
  RW_NODE_JOIN,
} rw_node_kind_t;
typedef struct rw_node {
  rw_node_kind_t kind;
  uint32_t var;
  uint32_t left;
  uint32_t right;
} rw_node_t;
struct qsop_rankwidth_decomposition {
  uint32_t nvars;
  uint32_t nnodes;
  uint32_t root;
  size_t words;
  rw_node_t *nodes;
  uint64_t *node_vars;
  uint32_t *postorder;
  uint32_t postorder_len;
  /* Cached score populated by qsop_rankwidth_decomposition_width.
   * When score_cached is true, rankwidth_record_decomposition_diagnostics
   * skips recomputing decomposition_score and uses these values directly. */
  bool score_cached;
  uint32_t cached_cutrank_width;
  uint64_t cached_table_forecast;
  uint64_t cached_join_pair_forecast;
};
typedef struct rw_entry {
  uint32_t signature;
  uint32_t residue;
  uint64_t count;
} rw_entry_t;
typedef struct rw_signature_rep {
  uint32_t signature;
} rw_signature_rep_t;
typedef struct rw_table {
  rw_entry_t *entries;
  size_t len;
  size_t cap;
  rw_signature_rep_t *reps;
  uint64_t *assignments;
  uint32_t *rep_weights;
  size_t reps_len;
  size_t reps_cap;
  /* Open-addressing index: signature -> rep index (UINT32_MAX = empty). Avoids the O(reps_len)
     linear scan in rw_table_add_rep (a rankwidth hot spot). Indices stay valid across reps growth.
   */
  uint32_t *rep_slots;
  size_t rep_slots_mask; /* (power-of-two capacity) - 1; 0 when unallocated */
} rw_table_t;
typedef struct rw_join_map_entry {
  uint32_t left_signature;
  uint32_t right_signature;
  uint32_t parent_signature;
  uint32_t residue_shift;
} rw_join_map_entry_t;
typedef struct rw_join_map {
  rw_join_map_entry_t *entries;
  uint64_t *assignments;
  size_t len;
  size_t cap;
} rw_join_map_t;
typedef struct rw_transition_eval {
  bool valid;
  uint32_t left_signature;
  uint32_t right_signature;
  uint32_t parent_signature;
  uint32_t residue_shift;
  bool sign_flip;
} rw_transition_eval_t;
typedef struct rw_fourier_table {
  uint32_t *signatures;
  uint64_t *assignments;
  uint32_t *assignment_weights;
  uint64_t *values;
  uint32_t *signature_slots;
  size_t len;
  size_t cap;
  size_t signature_slots_mask;
} rw_fourier_table_t;
typedef struct rw_complex_table {
  uint32_t *signatures;
  uint64_t *assignments;
  uint32_t *assignment_weights;
  long double *re;
  long double *im;
  uint32_t *signature_slots;
  size_t len;
  size_t cap;
  size_t signature_slots_mask;
} rw_complex_table_t;
typedef struct rw_complex64_table {
  uint32_t *signatures;
  uint64_t *assignments;
  uint32_t *assignment_weights;
  double *re;
  double *im;
  uint32_t *signature_slots;
  size_t len;
  size_t cap;
  size_t signature_slots_mask;
} rw_complex64_table_t;
typedef struct rw_dense_basis {
  uint64_t *pivot_rows;
  uint64_t *pivot_coords;
  uint32_t nbits;
  size_t words;
  uint32_t dim;
} rw_dense_basis_t;
typedef struct rw_sig_ht {
  uint32_t *slots; /* maps hash bucket → pool index (UINT32_MAX = empty) */
  uint64_t *keys;  /* parallel fingerprint for fast comparison without dereferencing pool */
  uint32_t mask;   /* slot count − 1 (power of two) */
} rw_sig_ht_t;
typedef struct rw_signature_pool {
  uint64_t *bits;
  uint64_t *fingerprints;
  size_t len;
  size_t cap;
  size_t words;
  rw_sig_ht_t ht; /* hash table index; only populated when len > RW_SIG_HT_THRESHOLD */
} rw_signature_pool_t;
typedef struct rw_decomposition_score {
  uint32_t cutrank_width;
  uint64_t table_forecast;
  uint64_t join_pair_forecast;
} rw_decomposition_score_t;
static inline uint64_t *node_vars(qsop_rankwidth_decomposition_t *decomposition, uint32_t node) {
  return qsop_bitset_row(decomposition->node_vars, decomposition->words, node);
}
static inline const uint64_t *node_vars_const(const qsop_rankwidth_decomposition_t *decomposition,
                                              uint32_t node) {
  return qsop_bitset_const_row(decomposition->node_vars, decomposition->words, node);
}
static inline uint64_t min_u64(uint64_t left, uint64_t right) {
  return left < right ? left : right;
}
static inline uint32_t rw_ctz_u64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctzll((unsigned long long)value);
#else
  uint32_t count = 0;
  while ((value & UINT64_C(1)) == 0) {
    value >>= 1U;
    count++;
  }
  return count;
#endif
}

uint64_t *rw_adjacency_bitsets(const qsop_instance_t *qsop, size_t words, qsop_error_t *error);

uint64_t rw_binary_signature_bound(uint32_t width);

bool rw_bitset_first_set_limited(const uint64_t *bits, size_t words, uint32_t nbits, uint32_t *out);

bool rw_build_sig_range_index(const rw_table_t *table, uint32_t max_sig, uint32_t **starts_out,
                              uint32_t **ends_out, qsop_error_t *error);

void rw_build_sig_range_index_into(const rw_table_t *table, uint32_t max_sig, uint32_t *starts,
                                   uint32_t *ends);

uint64_t *rw_complex64_assignment(const rw_complex64_table_t *table, size_t index, size_t words);

bool rw_complex64_slots_rehash(rw_complex64_table_t *table, qsop_error_t *error);

void rw_complex64_table_free(rw_complex64_table_t *table);

uint64_t *rw_complex_assignment(const rw_complex_table_t *table, size_t index, size_t words);

bool rw_complex_slots_rehash(rw_complex_table_t *table, qsop_error_t *error);

void rw_complex_table_free(rw_complex_table_t *table);

bool rw_compute_join_transition_sign(uint32_t nvars, const uint64_t *adj, rw_signature_pool_t *pool,
                                     const uint64_t *outside, size_t words, uint32_t r,
                                     uint32_t left_signature, const uint64_t *left_rep,
                                     uint32_t left_weight, uint32_t right_signature,
                                     const uint64_t *right_rep, uint32_t right_weight,
                                     uint64_t *scratch_sig, rw_transition_eval_t *out,
                                     qsop_error_t *error);

uint32_t rw_cut_rank_bitsets(uint32_t nvars, const uint64_t *adj, const uint64_t *left,
                             const uint64_t *right, size_t words, qsop_solve_stats_t *stats,
                             qsop_error_t *error);

bool rw_decomposition_score(const qsop_instance_t *qsop,
                            const qsop_rankwidth_decomposition_t *decomposition,
                            const uint64_t *adj, rw_decomposition_score_t *out,
                            qsop_error_t *error);

uint32_t rw_decomposition_width(const qsop_rankwidth_decomposition_t *decomposition,
                                const uint64_t *adj, qsop_solve_stats_t *stats,
                                qsop_error_t *error);

bool rw_dense_basis_add(rw_dense_basis_t *basis, const uint64_t *bits, uint64_t *scratch,
                        qsop_error_t *error);

bool rw_dense_basis_coord(const rw_dense_basis_t *basis, const uint64_t *bits, uint64_t *scratch,
                          uint64_t *coord_out, qsop_error_t *error);

void rw_dense_basis_free(rw_dense_basis_t *basis);

bool rw_dense_basis_init(rw_dense_basis_t *basis, uint32_t nbits, size_t words,
                         qsop_error_t *error);

uint64_t *rw_dense_basis_pivot_row(const rw_dense_basis_t *basis, uint32_t pivot);

bool rw_dense_basis_reduce(const rw_dense_basis_t *basis, const uint64_t *bits, uint64_t *scratch,
                           uint64_t *coord_out);

void rw_dense_fourier_forecast(const qsop_instance_t *qsop,
                               const qsop_rankwidth_decomposition_t *decomposition,
                               uint32_t cutrank_width, uint64_t *dense_table_out,
                               uint64_t *dense_even_join_out);

bool rw_dense_reference_value_count(uint32_t dim, uint32_t slots_per_signature, size_t *out,
                                    qsop_error_t *error);

bool rw_dense_single_pair_count(size_t left_signatures, size_t right_signatures, uint64_t *out,
                                qsop_error_t *error);

bool rw_extract_left_deep_order(const qsop_rankwidth_decomposition_t *decomposition,
                                uint32_t *order);

void rw_fill_all_vars(uint64_t *bits, uint32_t nvars, size_t words);

uint64_t *rw_fourier_assignment(const rw_fourier_table_t *table, size_t index, size_t words);

bool rw_fourier_slots_rehash(rw_fourier_table_t *table, qsop_error_t *error);

void rw_fourier_table_free(rw_fourier_table_t *table);

uint64_t *rw_join_map_assignment(const rw_join_map_t *map, size_t index, size_t words);

void rw_join_map_free(rw_join_map_t *map);

bool rw_record_decomposition_diagnostics(const qsop_instance_t *qsop,
                                         const qsop_rankwidth_decomposition_t *decomposition,
                                         qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                         qsop_error_t *error);

bool rw_refuse_large_modulus(const qsop_instance_t *qsop, qsop_error_t *error);

size_t rw_rep_hash(uint32_t signature);

bool rw_rep_slots_rehash(rw_table_t *table, qsop_error_t *error);

bool rw_reserve_complex64_table(rw_complex64_table_t *table, size_t needed, size_t words,
                                qsop_error_t *error);

bool rw_reserve_complex_table(rw_complex_table_t *table, size_t needed, size_t words,
                              qsop_error_t *error);

bool rw_reserve_entries(rw_table_t *table, size_t needed, qsop_error_t *error);

bool rw_reserve_fourier_table(rw_fourier_table_t *table, size_t needed, uint32_t value_slots,
                              size_t words, qsop_error_t *error);

bool rw_reserve_join_map(rw_join_map_t *map, size_t needed, size_t words, qsop_error_t *error);

bool rw_reserve_reps(rw_table_t *table, size_t needed, size_t words, qsop_error_t *error);

const uint64_t *rw_signature_bits(const rw_signature_pool_t *pool, uint32_t signature);

void rw_signature_pool_free(rw_signature_pool_t *pool);

bool rw_signature_pool_init(rw_signature_pool_t *pool, size_t words, qsop_error_t *error);

bool rw_signature_pool_intern(rw_signature_pool_t *pool, const uint64_t *bits, uint32_t *out,
                              qsop_error_t *error);

bool rw_signature_pool_reserve(rw_signature_pool_t *pool, size_t needed, qsop_error_t *error);

bool rw_solve_constant_mod(const qsop_instance_t *qsop, uint64_t count_modulus, uint64_t *counts,
                           qsop_solve_stats_t *stats, qsop_solve_trace_t *trace);

bool rw_solve_constant_result(const qsop_instance_t *qsop, qsop_result_t **out,
                              qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                              qsop_error_t *error);

bool rw_solve_count_table(const qsop_instance_t *qsop,
                          const qsop_rankwidth_decomposition_t *decomposition, const uint64_t *adj,
                          qsop_rankwidth_join_strategy_t join_strategy,
                          uint64_t materialize_join_max_pairs, qsop_result_t **out,
                          qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                          qsop_error_t *error);

bool rw_solve_count_table_mod_once(const qsop_instance_t *qsop,
                                   const qsop_rankwidth_decomposition_t *decomposition,
                                   const uint64_t *adj, uint64_t modulus, uint64_t *counts,
                                   qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                   qsop_error_t *error);

bool rw_solve_fourier(const qsop_instance_t *qsop,
                      const qsop_rankwidth_decomposition_t *decomposition, const uint64_t *adj,
                      qsop_rankwidth_fourier_kernel_t requested_kernel, qsop_result_t **out,
                      qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error);

bool rw_solve_no_edges_count_table(const qsop_instance_t *qsop, qsop_result_t **out,
                                   qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                   qsop_error_t *error);

bool rw_solve_no_edges_count_table_mod_once(const qsop_instance_t *qsop, uint64_t count_modulus,
                                            uint64_t *counts, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error);

void rw_solve_single_mode_no_edges(const qsop_instance_t *qsop, uint32_t target_mode,
                                   long double *out_re, long double *out_im, uint64_t *complex_ops);

void rw_solve_single_mode_no_edges_f64(const qsop_instance_t *qsop, uint32_t target_mode,
                                       double *out_re, double *out_im, uint64_t *complex_ops);

bool rw_solve_single_mode_once(const qsop_instance_t *qsop,
                               const qsop_rankwidth_decomposition_t *decomposition,
                               const uint64_t *adj, uint32_t target_mode, int *out_scale_exp2,
                               long double *out_re, long double *out_im,
                               long double *out_numeric_error_bound,
                               qsop_rankwidth_single_kernel_t kernel,
                               uint64_t materialize_join_max_pairs, qsop_solve_stats_t *stats,
                               qsop_solve_trace_t *trace, qsop_error_t *error);

bool rw_solve_single_mode_once_f64(const qsop_instance_t *qsop,
                                   const qsop_rankwidth_decomposition_t *decomposition,
                                   const uint64_t *adj, uint32_t target_mode, int *out_scale_exp2,
                                   long double *out_re, long double *out_im,
                                   long double *out_numeric_error_bound,
                                   qsop_rankwidth_single_kernel_t kernel,
                                   uint64_t materialize_join_max_pairs,
                                   const qsop_simd_vtable_t *simd, qsop_solve_stats_t *stats,
                                   qsop_solve_trace_t *trace, qsop_error_t *error);

bool rw_table_add_entry(rw_table_t *table, uint32_t signature, uint32_t residue, uint64_t count,
                        qsop_error_t *error);

bool rw_table_add_entry_mod(rw_table_t *table, uint32_t signature, uint32_t residue, uint64_t count,
                            uint64_t modulus, qsop_error_t *error);

bool rw_table_add_rep(rw_table_t *table, uint32_t signature, const uint64_t *assignment,
                      size_t words, qsop_error_t *error);

bool rw_table_rep_index(rw_table_t *table, uint32_t signature, const uint64_t *assignment,
                        size_t words, uint32_t *index_out, qsop_error_t *error);

bool rw_table_find_rep_index(const rw_table_t *table, uint32_t signature, uint32_t *index_out);

uint64_t *rw_table_assignment(const rw_table_t *table, size_t index, size_t words);

void rw_table_free(rw_table_t *table);

void rw_table_sort(rw_table_t *table);

#endif
