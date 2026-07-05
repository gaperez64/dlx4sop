#ifndef DLX4SOP_QSOP_SOLVE_H
#define DLX4SOP_QSOP_SOLVE_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct qsop_result {
  uint32_t r;
  uint64_t norm_h;
  uint64_t *counts;
  char **count_strings;
} qsop_result_t;

typedef struct qsop_simd_vtable qsop_simd_vtable_t;

/* Result of a single-Fourier-mode solve (Corollary 1): one complex value, with a
 * certified worst-case bound on the numerical (floating-point) error accumulated by the
 * DP. This is independent of the phase-rounding error a caller may have introduced
 * upstream (e.g. qasm2sop --approx's reported additive_amplitude_error_bound); the two
 * bounds are additive and a caller wanting a total certified error sums them. */
typedef struct qsop_amplitude {
  long double re;
  long double im;
  long double numeric_error_bound;
} qsop_amplitude_t;

typedef struct qsop_solve_stats {
  /* Branch / brute-force search */
  uint64_t search_nodes;
  uint64_t leaf_assignments;

  /* Branch residue cache */
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t cache_avoided_nodes;
  uint64_t cache_canonical_hits;
  uint64_t cache_canonical_lookups;
  uint64_t cache_canonical_stores;
  uint64_t cache_entries;
  uint64_t cache_canonical_entries;
  uint64_t cache_stored_residue_slots;
  uint64_t cache_key_bytes;
  uint64_t cache_count_bytes;
  uint64_t cache_estimated_bytes;

  /* DP table (shared by treewidth, rankwidth, branch-delegate) */
  uint64_t table_entries;
  uint64_t max_table_entries;
  uint64_t signature_entries;
  uint64_t max_signature_entries;
  uint64_t join_pairs;
  uint64_t join_signature_pairs;

  /* Rankwidth-specific */
  uint64_t rankwidth_table_forecast;
  uint64_t rankwidth_join_pair_forecast;
  uint64_t rankwidth_dense_table_forecast;
  uint64_t rankwidth_dense_even_join_forecast;
  uint64_t rankwidth_transition_bytes;
  uint64_t rankwidth_transition_layout_u16_events;
  uint64_t rankwidth_transition_layout_u32_events;
  uint64_t rankwidth_dense_join_events;
  uint64_t rankwidth_materialized_join_events;
  uint64_t rankwidth_streaming_join_events;
  uint64_t rankwidth_streaming_join_candidate_pairs;
  uint64_t rankwidth_streaming_join_emitted_pairs;
  uint64_t rankwidth_linear_transition_events;
  uint64_t rankwidth_table_assignment_bytes;
  uint32_t rankwidth_fourier_kernel;

  /* Branch dispatch counters */
  uint64_t treewidth_delegations;
  uint64_t rankwidth_delegations;
  uint64_t branch_fallthroughs;
  uint64_t branch_treewidth_skips;
  uint64_t branch_rankwidth_skips;

  /* Branch residual sizing */
  uint32_t max_residual_vars;
  uint32_t max_residual_edges;
  uint32_t max_residual_components;
  uint32_t max_residual_largest_component;
  uint32_t max_residual_min_fill_width;
  uint32_t max_residual_prefix_cut_rank;

  /* Decomposition */
  uint32_t components;
  uint32_t decomposition_width;
  uint32_t rankwidth_cutrank_width;

  /* Kernel diagnostics */
  uint32_t simd_kernel;
  uint32_t single_mode_precision;
  uint32_t treewidth_single_complex_kernel;
  uint32_t rankwidth_single_complex_kernel;
  uint32_t bitset_kernel;
  uint64_t simd_vectorized_ops;
  uint64_t simd_scalar_fallback_ops;
} qsop_solve_stats_t;

typedef struct qsop_rankwidth_decomposition qsop_rankwidth_decomposition_t;

typedef enum qsop_solve_mode {
  QSOP_SOLVE_MODE_COUNT_TABLE,
  QSOP_SOLVE_MODE_FOURIER,
} qsop_solve_mode_t;

typedef enum qsop_rankwidth_generator {
  QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP,
  QSOP_RANKWIDTH_GENERATOR_BALANCED,
  QSOP_RANKWIDTH_GENERATOR_MIN_FILL,
  QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
  /* Derive rank decomposition from min-fill treewidth elimination tree. */
  QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH,
  QSOP_RANKWIDTH_GENERATOR_BEST,
  QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH,
} qsop_rankwidth_generator_t;

typedef enum qsop_rankwidth_solve_mode {
  QSOP_RANKWIDTH_SOLVE_COUNT_TABLE,
  QSOP_RANKWIDTH_SOLVE_FOURIER,
} qsop_rankwidth_solve_mode_t;

typedef enum qsop_rankwidth_fourier_kernel {
  QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO,
  QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING,
  QSOP_RANKWIDTH_FOURIER_KERNEL_HYBRID_EVEN_FWHT,
  QSOP_RANKWIDTH_FOURIER_KERNEL_DENSE_REFERENCE,
} qsop_rankwidth_fourier_kernel_t;

typedef enum qsop_rankwidth_single_kernel {
  QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO,
  QSOP_RANKWIDTH_SINGLE_KERNEL_STREAMING,
  QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED,
  QSOP_RANKWIDTH_SINGLE_KERNEL_DENSE,
} qsop_rankwidth_single_kernel_t;

typedef enum qsop_treewidth_order {
  QSOP_TREEWIDTH_ORDER_MIN_FILL,
  QSOP_TREEWIDTH_ORDER_MIN_DEGREE,
  QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE,
} qsop_treewidth_order_t;

typedef enum qsop_branch_heuristic {
  QSOP_BRANCH_HEURISTIC_SPLIT,
  QSOP_BRANCH_HEURISTIC_TREEWIDTH,
  QSOP_BRANCH_HEURISTIC_CUTRANK_PROXY,
} qsop_branch_heuristic_t;

typedef enum qsop_rankwidth_join_strategy {
  QSOP_RANKWIDTH_JOIN_AUTO,         /* use streaming when forecast exceeds threshold */
  QSOP_RANKWIDTH_JOIN_MATERIALIZED, /* always build full CSR transition table */
  QSOP_RANKWIDTH_JOIN_STREAMING,    /* always use streaming (no transition table) */
} qsop_rankwidth_join_strategy_t;

/* Per-solve options for the rankwidth solver.  Zero-initialize for defaults. */
typedef struct qsop_rankwidth_solve_options {
  qsop_rankwidth_join_strategy_t join_strategy; /* default AUTO */
  uint64_t materialize_join_max_pairs;           /* 0 = use built-in default */
  qsop_rankwidth_fourier_kernel_t fourier_kernel; /* default AUTO */
} qsop_rankwidth_solve_options_t;

typedef struct qsop_rankwidth_single_mode_options {
  qsop_rankwidth_single_kernel_t kernel; /* default AUTO */
  uint64_t materialize_join_max_pairs;   /* 0 = use built-in default */
  const qsop_simd_vtable_t *simd;        /* optional; used by f64 kernels */
} qsop_rankwidth_single_mode_options_t;

/* Policy for how the branch solver sources a rank decomposition when considering
 * rankwidth delegation. */
typedef enum qsop_branch_rw_source {
  QSOP_BRANCH_RW_SOURCE_AUTO,           /* default: derive from treewidth elimination tree */
  QSOP_BRANCH_RW_SOURCE_NATIVE,         /* use min-fill-cut (original behavior) */
  QSOP_BRANCH_RW_SOURCE_FROM_TREEWIDTH, /* derive from treewidth elimination tree */
  QSOP_BRANCH_RW_SOURCE_BOTH,           /* try both; keep better forecast */
  QSOP_BRANCH_RW_SOURCE_NONE,           /* never attempt rankwidth delegation */
} qsop_branch_rw_source_t;

typedef struct qsop_solve_trace_event {
  const char *phase;
  uint32_t depth;
  uint64_t items;
  uint64_t elapsed_ns;
} qsop_solve_trace_event_t;

typedef void (*qsop_solve_trace_fn)(void *user, const qsop_solve_trace_event_t *event);

typedef struct qsop_solve_trace {
  qsop_solve_trace_fn emit;
  void *user;
} qsop_solve_trace_t;

void qsop_result_free(qsop_result_t *result);

bool qsop_solve_bruteforce(const qsop_instance_t *qsop, uint32_t max_vars, qsop_result_t **out,
                           qsop_error_t *error);

bool qsop_solve_bruteforce_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                 qsop_result_t **out, qsop_solve_stats_t *stats,
                                 qsop_error_t *error);

bool qsop_solve_bruteforce_trace_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                       qsop_result_t **out, qsop_solve_stats_t *stats,
                                       qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_bruteforce_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_vars, qsop_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_components_bruteforce(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                      qsop_result_t **out, qsop_error_t *error);

bool qsop_solve_components_bruteforce_stats(const qsop_instance_t *qsop,
                                            uint32_t max_component_vars, qsop_result_t **out,
                                            qsop_solve_stats_t *stats, qsop_error_t *error);

bool qsop_solve_components_bruteforce_trace_stats(const qsop_instance_t *qsop,
                                                  uint32_t max_component_vars, qsop_result_t **out,
                                                  qsop_solve_stats_t *stats,
                                                  qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_components_bruteforce_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_component_vars, qsop_solve_mode_t mode,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error);

bool qsop_solve_treewidth(const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_result_t **out,
                          qsop_error_t *error);

bool qsop_solve_treewidth_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                qsop_result_t **out, qsop_solve_stats_t *stats,
                                qsop_error_t *error);

bool qsop_solve_treewidth_trace_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                      qsop_result_t **out, qsop_solve_stats_t *stats,
                                      qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_treewidth_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_solve_mode_t mode,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error);

bool qsop_solve_treewidth_order_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error);

bool qsop_solve_treewidth_order_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order,
    qsop_solve_mode_t mode, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error);

/* Compute a single Fourier mode (target_mode, typically 1 for one amplitude) directly via
 * a complex-arithmetic dynamic program: table size is O(2^k) per elimination step,
 * independent of r (unlike the count-table/all-modes-Fourier paths above, which are
 * O(r*2^k)). Suitable for QSOP instances with r far too large for those paths to allocate
 * (e.g. qasm2sop --approx output at a tight error budget). */
bool qsop_solve_treewidth_single_mode(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                      qsop_treewidth_order_t order_policy, uint32_t target_mode,
                                      qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                      qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_treewidth_single_mode_f64(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                          qsop_treewidth_order_t order_policy,
                                          uint32_t target_mode,
                                          const qsop_simd_vtable_t *simd,
                                          qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                          qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_treewidth_order_alloc(const qsop_instance_t *qsop, qsop_treewidth_order_t order,
                                uint32_t **order_out, uint32_t *width_out,
                                qsop_error_t *error);

bool qsop_solve_treewidth_precomputed_order_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_treewidth_precomputed_order_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, qsop_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error);

/* Precomputed-order sibling of qsop_solve_treewidth_single_mode: lets a caller (the branch
 * backend's single-fourier delegation) share one min-fill elimination order across the
 * treewidth delegate and the from-treewidth rankwidth generator, exactly as it already does
 * for count-table/all-modes-Fourier via qsop_solve_treewidth_precomputed_order_mode_trace_stats. */
bool qsop_solve_treewidth_precomputed_order_single_mode(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, uint32_t target_mode, qsop_amplitude_t *out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_treewidth_precomputed_order_count_mod_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, uint64_t count_modulus, uint64_t *counts,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error);


bool qsop_rankwidth_decomposition_parse_file(FILE *file, const char *path, uint32_t nvars,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error);

bool qsop_rankwidth_decomposition_generate(const qsop_instance_t *qsop,
                                           qsop_rankwidth_generator_t generator,
                                           qsop_rankwidth_decomposition_t **out,
                                           qsop_error_t *error);

/* Build a from-treewidth rankwidth decomposition from a precomputed elimination order,
 * avoiding a second min-fill run when the caller already holds a treewidth order. */
bool qsop_rankwidth_decomposition_from_order(const qsop_instance_t *qsop,
                                             const uint32_t *order,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error);

bool qsop_rankwidth_decomposition_width(
    const qsop_instance_t *qsop, qsop_rankwidth_decomposition_t *decomposition,
    uint32_t *cutrank_width_out, qsop_error_t *error);

bool qsop_rankwidth_decomposition_forecast(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint64_t *max_table_entries_out, uint64_t *join_pairs_out, qsop_error_t *error);

void qsop_rankwidth_decomposition_free(qsop_rankwidth_decomposition_t *decomposition);

bool qsop_solve_rankwidth_count_table_mod_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint64_t count_modulus, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_rankwidth_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_rankwidth_options_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode,
    const qsop_rankwidth_solve_options_t *options,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error);

bool qsop_solve_rankwidth_trace_stats(const qsop_instance_t *qsop,
                                      const qsop_rankwidth_decomposition_t *decomposition,
                                      uint32_t max_vars, qsop_result_t **out,
                                      qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                      qsop_error_t *error);

/* Compute a single Fourier mode (target_mode, typically 1 for one amplitude) directly via
 * a complex-arithmetic dynamic program over the given rank-decomposition: table size is
 * O(2^k) per node, independent of r (unlike the count-table/all-modes-Fourier paths
 * above, which are O(r*2^k)). Mirrors qsop_solve_treewidth_single_mode. */
bool qsop_solve_rankwidth_single_mode(const qsop_instance_t *qsop,
                                      const qsop_rankwidth_decomposition_t *decomposition,
                                      uint32_t max_vars, uint32_t target_mode,
                                      qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                      qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_rankwidth_single_mode_options(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, uint32_t target_mode,
    const qsop_rankwidth_single_mode_options_t *options,
    qsop_amplitude_t *out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error);

bool qsop_solve_rankwidth_single_mode_f64(const qsop_instance_t *qsop,
                                          const qsop_rankwidth_decomposition_t *decomposition,
                                          uint32_t max_vars, uint32_t target_mode,
                                          const qsop_simd_vtable_t *simd,
                                          qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                          qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_rankwidth_single_mode_f64_options(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, uint32_t target_mode,
    const qsop_rankwidth_single_mode_options_t *options,
    qsop_amplitude_t *out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error);

bool qsop_rankwidth_decomposition_write_file(FILE *file,
                                             const qsop_rankwidth_decomposition_t *decomposition,
                                             qsop_error_t *error);

bool qsop_result_write_residue_vector(FILE *file, const qsop_result_t *result, qsop_error_t *error);

/* ---------------------------------------------------------------------------
 * JSONL backend-decision sink
 *
 * Create on the stack and pass to qsop_solve_residual_branch_*_sink_* to
 * emit one JSON object per delegation decision.  Set file = NULL to disable.
 * next_id is read/written by the emitter; initialise to 0.
 * --------------------------------------------------------------------------- */

typedef struct qsop_backend_stats_sink {
  FILE *file;               /* JSONL output file — NULL disables emission */
  const char *instance;     /* value for the "instance" JSON field */
  uint64_t next_id;         /* monotone record counter, incremented per record */
  bool calibrate_backends;  /* if true, run the losing backend too for timing data */
} qsop_backend_stats_sink_t;

/* Tuning policy for the branch solver's rankwidth delegation decision.
 * Pass NULL to use built-in defaults.  All zero fields take their defaults. */
typedef struct qsop_branch_policy {
  /* Early cheap-treewidth veto: skip rankwidth probe when treewidth is obviously cheap. */
  uint32_t rw_min_treewidth_width;    /* veto when tw_width <= this (default 2) */
  uint64_t rw_min_treewidth_forecast; /* veto when tw_table_forecast <= this (default 512) */
  uint32_t rw_min_residual_vars;      /* veto small-residual when tw_width <= 5 (default 16) */
  uint32_t rw_low_rank_bypass;        /* bypass cheap-tw veto when prefix_cut_rank <= this (default 4) */

  /* Cost-model coefficients for choosing rw vs tw. */
  uint64_t rw_fixed_overhead_ns;  /* fixed rw overhead (default 20000) */
  uint64_t tw_fixed_overhead_ns;  /* fixed tw overhead (default 10000) */
  uint64_t C_rw_table;            /* ns per rw table entry (default 80) */
  uint64_t C_rw_join;             /* ns per rw join pair (default 40) */
  uint64_t C_rw_sig;              /* ns per rw signature (default 2000) */
  uint64_t C_tw_table;            /* ns per tw table entry (default 20) */
  uint64_t C_tw_join;             /* ns per tw join pair (default 10) */
  double   rw_min_speedup;        /* select rw only when rw_est * speedup < tw_est (default 1.1) */
  uint64_t rw_memory_penalty_ns;  /* extra cost added to rw estimate for memory risk (default 0) */
} qsop_branch_policy_t;

/* Per-solve options for the branch solver.  Zero-initialize for defaults:
 * heuristic=split, rw_source=auto/from-treewidth, mode=count-table,
 * zero policy (all defaults), no sink, no trace. */
typedef struct qsop_branch_solve_options {
  qsop_branch_heuristic_t heuristic;    /* default SPLIT (0) */
  qsop_branch_rw_source_t rw_source;   /* default AUTO (0) */
  qsop_solve_mode_t mode;              /* default COUNT_TABLE (0) */
  qsop_branch_policy_t policy;         /* all-zero fields take built-in defaults */
  qsop_backend_stats_sink_t *sink;     /* NULL to disable JSONL sink */
  qsop_solve_trace_t *trace;           /* NULL to disable tracing */
} qsop_branch_solve_options_t;

bool qsop_solve_branch(const qsop_instance_t *qsop, uint32_t max_vars,
                       const qsop_branch_solve_options_t *options,
                       qsop_result_t **out, qsop_solve_stats_t *stats,
                       qsop_error_t *error);

typedef enum qsop_branch_single_fallback {
  QSOP_BRANCH_SINGLE_FALLBACK_AUTO = 0,
  QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY,
  QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
} qsop_branch_single_fallback_t;

typedef enum qsop_branch_single_precision {
  QSOP_BRANCH_SINGLE_PRECISION_AUTO = 0,
  QSOP_BRANCH_SINGLE_PRECISION_DOUBLE,
  QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE,
} qsop_branch_single_precision_t;

typedef enum qsop_branch_single_kernel {
  QSOP_BRANCH_SINGLE_KERNEL_AUTO = 0,
  QSOP_BRANCH_SINGLE_KERNEL_SCALAR,
  QSOP_BRANCH_SINGLE_KERNEL_SIMD_AVX2,
} qsop_branch_single_kernel_t;

/* Per-solve options for qsop_solve_branch_single_mode. Zero-initialize for defaults:
 * rw_source=auto/from-treewidth, heuristic=split, fallback=auto, precision/kernel=auto,
 * and zero numeric caps selecting built-in limits. */
typedef struct qsop_branch_single_mode_options {
  qsop_branch_rw_source_t rw_source;
  qsop_branch_policy_t policy;
  qsop_branch_heuristic_t heuristic;
  qsop_branch_single_fallback_t fallback;
  qsop_branch_single_precision_t precision;
  qsop_branch_single_kernel_t kernel;

  uint64_t max_search_nodes;
  uint32_t max_fallback_vars;
  uint32_t max_recursion_depth;
  uint64_t cache_budget_mib;
  uint32_t cache_min_vars;
  uint32_t phase_cache_lg_cap;

  qsop_backend_stats_sink_t *sink;
  qsop_solve_trace_t *trace;
} qsop_branch_single_mode_options_t;

/* Single Fourier mode for the branch backend: delegates connected components to treewidth or
 * rankwidth when cheap, then optionally falls back to scalar residual branching without ever
 * allocating O(R) count vectors. */
bool qsop_solve_branch_single_mode(const qsop_instance_t *qsop, uint32_t max_vars,
                                   uint32_t target_mode,
                                   const qsop_branch_single_mode_options_t *options,
                                   qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                   qsop_error_t *error);

#endif
