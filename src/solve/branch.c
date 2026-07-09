#include "component_key.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/qsop_stats.h"
#include "dlx4sop/residual.h"
#include "dlx4sop/residue.h"
#include "trace.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Private policy defaults (kept out of the public header).
 * --------------------------------------------------------------------------- */
#define BRANCH_POLICY_DEFAULT_RW_MIN_TW_WIDTH 2U
#define BRANCH_POLICY_DEFAULT_RW_MIN_TW_FORECAST 512UL
#define BRANCH_POLICY_DEFAULT_RW_MIN_RESIDUAL_VARS 16U
#define BRANCH_POLICY_DEFAULT_RW_LOW_RANK_BYPASS 4U
#define BRANCH_POLICY_DEFAULT_RW_FIXED_OVERHEAD_NS 20000UL
#define BRANCH_POLICY_DEFAULT_TW_FIXED_OVERHEAD_NS 10000UL
#define BRANCH_POLICY_DEFAULT_C_RW_TABLE 80UL
#define BRANCH_POLICY_DEFAULT_C_RW_JOIN 40UL
#define BRANCH_POLICY_DEFAULT_C_RW_SIG 2000UL
#define BRANCH_POLICY_DEFAULT_C_TW_TABLE 20UL
#define BRANCH_POLICY_DEFAULT_C_TW_JOIN 10UL
#define BRANCH_POLICY_DEFAULT_C_RW_PROBE 2UL
#define BRANCH_POLICY_DEFAULT_RW_MIN_SPEEDUP 1.1
#define BRANCH_SINGLE_DEFAULT_MAX_FALLBACK_VARS 64U
#define BRANCH_SINGLE_DEFAULT_MAX_SEARCH_NODES UINT64_C(10000000)
#define BRANCH_SINGLE_DEFAULT_CACHE_BUDGET_MIB 256U
#define BRANCH_SINGLE_DEFAULT_CACHE_MIN_VARS 12U
#define BRANCH_SINGLE_DEFAULT_PHASE_CACHE_LG_CAP 16U
#define BRANCH_SINGLE_MAX_PHASE_CACHE_LG_CAP 30U
/* Purely a defensive bound on the root-level component-split allocation's O(nvars) cost, not
 * a solvability gate -- that job belongs to a per-component --max-vars check instead (in
 * qsop_solve_branch_single_mode's case, the one inside branch_single_mode_delegate_component,
 * which fires before the expensive width diagnostic; in qsop_solve_branch's case, the one added
 * to branch_sum_uncached's fallthrough-to-branching step, since that recursion has no separate
 * max_fallback_vars-style cap of its own). Shared by both entry points' root nvars checks. */
#define BRANCH_ROOT_SANITY_MULTIPLIER 64U

/* Fill every zero field with its built-in default so callers read fields directly. */
static qsop_branch_policy_t branch_policy_normalize(const qsop_branch_policy_t *in) {
  qsop_branch_policy_t p = in != NULL ? *in : (qsop_branch_policy_t){0};
  if (!p.rw_min_treewidth_width)
    p.rw_min_treewidth_width = BRANCH_POLICY_DEFAULT_RW_MIN_TW_WIDTH;
  if (!p.rw_min_treewidth_forecast)
    p.rw_min_treewidth_forecast = BRANCH_POLICY_DEFAULT_RW_MIN_TW_FORECAST;
  if (!p.rw_min_residual_vars)
    p.rw_min_residual_vars = BRANCH_POLICY_DEFAULT_RW_MIN_RESIDUAL_VARS;
  if (!p.rw_low_rank_bypass)
    p.rw_low_rank_bypass = BRANCH_POLICY_DEFAULT_RW_LOW_RANK_BYPASS;
  if (!p.rw_fixed_overhead_ns)
    p.rw_fixed_overhead_ns = BRANCH_POLICY_DEFAULT_RW_FIXED_OVERHEAD_NS;
  if (!p.tw_fixed_overhead_ns)
    p.tw_fixed_overhead_ns = BRANCH_POLICY_DEFAULT_TW_FIXED_OVERHEAD_NS;
  if (!p.C_rw_table)
    p.C_rw_table = BRANCH_POLICY_DEFAULT_C_RW_TABLE;
  if (!p.C_rw_join)
    p.C_rw_join = BRANCH_POLICY_DEFAULT_C_RW_JOIN;
  if (!p.C_rw_sig)
    p.C_rw_sig = BRANCH_POLICY_DEFAULT_C_RW_SIG;
  if (!p.C_tw_table)
    p.C_tw_table = BRANCH_POLICY_DEFAULT_C_TW_TABLE;
  if (!p.C_tw_join)
    p.C_tw_join = BRANCH_POLICY_DEFAULT_C_TW_JOIN;
  if (!p.C_rw_probe)
    p.C_rw_probe = BRANCH_POLICY_DEFAULT_C_RW_PROBE;
  if (p.rw_min_speedup <= 0.0)
    p.rw_min_speedup = BRANCH_POLICY_DEFAULT_RW_MIN_SPEEDUP;
  return p;
}

/* ---------------------------------------------------------------------------
 * JSONL backend-decision emission helpers
 * --------------------------------------------------------------------------- */

static inline double branch_ns_to_ms(uint64_t ns) {
  return (double)ns * 1e-6;
}

/* Per-call rankwidth data collected inside branch_try_rankwidth_delegate. */
typedef struct branch_rw_decision_data {
  bool attempted;
  const char *veto_reason; /* NULL = no early veto (decomposition was generated) */
  double generation_ms;
  uint32_t cutrank_width;
  uint64_t forecast_entries;
  uint64_t forecast_join_pairs;
  double actual_ms;
} branch_rw_decision_data_t;

static void branch_jsonl_write_string(FILE *f, const char *s) {
  fputc('"', f);
  if (s == NULL) {
    fputc('"', f);
    return;
  }
  for (; *s != '\0'; s++) {
    if (*s == '"') {
      fputs("\\\"", f);
    } else if (*s == '\\') {
      fputs("\\\\", f);
    } else {
      fputc(*s, f);
    }
  }
  fputc('"', f);
}

static void branch_emit_jsonl_record(qsop_backend_stats_sink_t *sink, uint32_t n_active_vars,
                                     uint32_t n_active_edges, uint32_t modulus_r,
                                     const char *backend_chosen, const char *veto_reason,
                                     double tw_probe_ms, bool tw_width_set, uint32_t tw_width,
                                     bool tw_forecast_set, uint64_t tw_forecast_entries,
                                     uint64_t tw_forecast_join_pairs, bool tw_actual_set,
                                     double tw_actual_ms, const branch_rw_decision_data_t *rw) {
  if (sink == NULL || sink->file == NULL) {
    return;
  }
  FILE *f = sink->file;
  const uint64_t id = sink->next_id++;
  fprintf(f, "{\"schema\":\"sop_solve_backend_stats_v1\"");
  fprintf(f, ",\"instance\":");
  branch_jsonl_write_string(f, sink->instance);
  fprintf(f, ",\"residual_id\":%" PRIu64, id);
  fprintf(f, ",\"component_id\":0");
  fprintf(f, ",\"backend_chosen\":");
  branch_jsonl_write_string(f, backend_chosen);
  fprintf(f, ",\"selector_policy\":\"conservative\"");
  fprintf(f, ",\"veto_reason\":");
  if (veto_reason != NULL) {
    branch_jsonl_write_string(f, veto_reason);
  } else {
    fputs("null", f);
  }
  fprintf(f, ",\"n_active_vars\":%" PRIu32, n_active_vars);
  fprintf(f, ",\"n_active_terms\":%" PRIu32, n_active_edges);
  fprintf(f, ",\"modulus_r\":%" PRIu32, modulus_r);
  fprintf(f, ",\"treewidth_probe_ms\":%.4f", tw_probe_ms);
  if (tw_width_set) {
    fprintf(f, ",\"treewidth_width\":%" PRIu32, tw_width);
  } else {
    fputs(",\"treewidth_width\":null", f);
  }
  if (tw_forecast_set) {
    fprintf(f, ",\"treewidth_forecast_entries\":%" PRIu64, tw_forecast_entries);
    fprintf(f, ",\"treewidth_forecast_join_pairs\":%" PRIu64, tw_forecast_join_pairs);
  } else {
    fputs(",\"treewidth_forecast_entries\":null", f);
    fputs(",\"treewidth_forecast_join_pairs\":null", f);
  }
  if (tw_actual_set) {
    fprintf(f, ",\"treewidth_actual_ms\":%.4f", tw_actual_ms);
  } else {
    fputs(",\"treewidth_actual_ms\":null", f);
  }
  if (rw != NULL && rw->attempted) {
    fprintf(f, ",\"rankwidth_generation_ms\":%.4f", rw->generation_ms);
    /* Show detailed rankwidth data when: (a) rankwidth won, or (b) calibration populated it. */
    const bool show_rw_details = (rw->veto_reason == NULL) || (rw->cutrank_width > 0);
    if (show_rw_details) {
      fprintf(f, ",\"rankwidth_cutrank_width\":%" PRIu32, rw->cutrank_width);
      fprintf(f, ",\"rankwidth_forecast_entries\":%" PRIu64, rw->forecast_entries);
      fprintf(f, ",\"rankwidth_forecast_join_pairs\":%" PRIu64, rw->forecast_join_pairs);
      if (rw->actual_ms > 0.0) {
        fprintf(f, ",\"rankwidth_actual_ms\":%.4f", rw->actual_ms);
      } else {
        fputs(",\"rankwidth_actual_ms\":null", f);
      }
    } else {
      fputs(",\"rankwidth_cutrank_width\":null", f);
      fputs(",\"rankwidth_forecast_entries\":null", f);
      fputs(",\"rankwidth_forecast_join_pairs\":null", f);
      fputs(",\"rankwidth_actual_ms\":null", f);
    }
  } else {
    fputs(",\"rankwidth_generation_ms\":0.0", f);
    fputs(",\"rankwidth_cutrank_width\":null", f);
    fputs(",\"rankwidth_forecast_entries\":null", f);
    fputs(",\"rankwidth_forecast_join_pairs\":null", f);
    fputs(",\"rankwidth_actual_ms\":null", f);
  }
  fputs("}\n", f);
  (void)fflush(f);
}

static void set_error(qsop_error_t *error, const char *fmt, ...) {
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

typedef struct residual_cache_key {
  uint64_t fingerprint;
  bool canonical;
  uint64_t r;
  uint32_t nvars;
  uint32_t nedges;
  uint64_t constant;
  uint32_t active_vars;
  uint32_t active_edges;
  uint8_t *active_var;
  uint8_t *active_edge;
  uint64_t *unary;
  uint32_t *edge_u;
  uint32_t *edge_v;
} residual_cache_key_t;

typedef struct residual_cache_entry {
  residual_cache_key_t key;
  uint64_t *counts;
  uint64_t search_nodes;
  size_t next;
} residual_cache_entry_t;

typedef struct residual_cache {
  residual_cache_entry_t *entries;
  size_t *buckets;
  size_t len;
  size_t cap;
  size_t bucket_count;
} residual_cache_t;

/* Small ring-buffer cache of treewidth elimination orders keyed on graph
 * adjacency (nvars, nedges, edge endpoints), ignoring edge/unary coefficients.
 * Lets sibling residuals (x=0 vs x=1 branches) reuse the same min-fill order
 * since both have identical adjacency after the branched variable is removed. */
#define BRANCH_ORDER_CACHE_CAP 16U

typedef struct branch_order_cache_entry {
  uint64_t fp;
  uint32_t nvars;
  uint32_t nedges;
  uint32_t width;
  uint32_t *order;
} branch_order_cache_entry_t;

typedef struct branch_order_cache {
  branch_order_cache_entry_t slots[BRANCH_ORDER_CACHE_CAP];
  uint32_t next;
} branch_order_cache_t;

static uint64_t branch_order_adj_fp(const qsop_instance_t *sub) {
  uint64_t fp = UINT64_C(1469598103934665603);
  fp ^= (uint64_t)sub->nvars;
  fp *= UINT64_C(1099511628211);
  fp ^= (uint64_t)sub->nedges;
  fp *= UINT64_C(1099511628211);
  for (uint32_t e = 0; e < sub->nedges; e++) {
    fp ^= (uint64_t)sub->edge_u[e];
    fp *= UINT64_C(1099511628211);
    fp ^= (uint64_t)sub->edge_v[e];
    fp *= UINT64_C(1099511628211);
  }
  return fp;
}

static void branch_order_cache_free_entries(branch_order_cache_t *c) {
  for (uint32_t i = 0; i < BRANCH_ORDER_CACHE_CAP; i++) {
    free(c->slots[i].order);
    c->slots[i].order = NULL;
  }
}

/* Returns a malloc'd copy of a cached order; NULL if not found. */
static uint32_t *branch_order_cache_lookup(branch_order_cache_t *c, const qsop_instance_t *sub,
                                           uint64_t fp, uint32_t *width_out) {
  for (uint32_t i = 0; i < BRANCH_ORDER_CACHE_CAP; i++) {
    const branch_order_cache_entry_t *e = &c->slots[i];
    if (e->order != NULL && e->fp == fp && e->nvars == sub->nvars && e->nedges == sub->nedges) {
      uint32_t *copy = malloc((size_t)sub->nvars * sizeof(*copy));
      if (copy == NULL) {
        return NULL;
      }
      memcpy(copy, e->order, (size_t)sub->nvars * sizeof(*copy));
      *width_out = e->width;
      return copy;
    }
  }
  return NULL;
}

static void branch_order_cache_insert(branch_order_cache_t *c, const qsop_instance_t *sub,
                                      uint64_t fp, const uint32_t *order, uint32_t width) {
  const uint32_t slot = c->next % BRANCH_ORDER_CACHE_CAP;
  free(c->slots[slot].order);
  c->slots[slot].order = NULL;
  uint32_t *copy = malloc((size_t)sub->nvars * sizeof(*copy));
  if (copy == NULL) {
    c->next++;
    return;
  }
  memcpy(copy, order, (size_t)sub->nvars * sizeof(*copy));
  c->slots[slot] = (branch_order_cache_entry_t){
      .fp = fp, .nvars = sub->nvars, .nedges = sub->nedges, .width = width, .order = copy};
  c->next++;
}

typedef struct branch_search_stats {
  uint64_t nodes;
  uint64_t leaves;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t cache_avoided_nodes;
  uint64_t cache_canonical_hits;
  uint64_t cache_canonical_lookups;
  uint64_t cache_canonical_stores;
  uint64_t table_entries;
  uint64_t max_table_entries;
  uint64_t signature_entries;
  uint64_t max_signature_entries;
  uint64_t join_pairs;
  uint64_t join_signature_pairs;
  uint64_t rankwidth_table_forecast;
  uint64_t rankwidth_join_pair_forecast;
  uint64_t treewidth_delegations;
  uint64_t rankwidth_delegations;
  uint64_t branch_fallthroughs;
  uint64_t branch_treewidth_skips;
  uint64_t branch_rankwidth_skips;
  uint32_t max_residual_vars;
  uint32_t max_residual_edges;
  uint32_t max_residual_components;
  uint32_t max_residual_largest_component;
  uint32_t max_residual_min_fill_width;
  uint32_t max_residual_prefix_cut_rank;
  uint64_t *work;
  uint64_t *tmp;
  residual_cache_t cache;
  branch_order_cache_t order_cache;
  qsop_solve_trace_t *trace;
  qsop_backend_stats_sink_t *sink;
  qsop_branch_heuristic_t heuristic;
  qsop_branch_rw_source_t rw_source;
  qsop_branch_policy_t policy; /* effective tuning policy (defaults resolved at init) */
  qsop_solve_mode_t mode;
  uint64_t count_modulus;
  uint32_t depth;
  uint32_t decomposition_width;
  uint32_t rankwidth_cutrank_width;
  /* Unlike single-fourier's branch_single_mode_state_t, this recursion's branching fallback
   * (see branch_sum_uncached) has no separate max_fallback_vars-style cap of its own -- the
   * root nvars check in qsop_solve_branch is deliberately loose (see BRANCH_ROOT_SANITY_
   * MULTIPLIER), so max_vars must be threaded down here to gate the fallthrough-to-branching
   * step per component instead. */
  uint32_t max_vars;
} branch_search_stats_t;

#define BRANCH_TREEWIDTH_DELEGATE_MIN_VARS 16U
#define BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH 14U
#define BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS (BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH + 1U)
/* Root-only widening for the top-level treewidth fast path.  This admits the low-width
 * inferq outliers and qwalk11-sized instances before the coarse root nvars sanity check,
 * while avoiding several-thousand-variable roots where even computing an order is costly. */
#define BRANCH_ROOT_TREEWIDTH_WIDE_MAX_WIDTH 18U
#define BRANCH_ROOT_TREEWIDTH_WIDE_MAX_BAG_VARS (BRANCH_ROOT_TREEWIDTH_WIDE_MAX_WIDTH + 1U)
#define BRANCH_ROOT_TREEWIDTH_WIDE_MAX_VARS 2500U
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH 25U
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_BAG_VARS                                              \
  (BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH + 1U)
#define BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH 12U
#define BRANCH_RANKWIDTH_TREEWIDTH_MARGIN 2U
/* When prefix_cut_rank is at most this threshold, bypass the blanket
 * "treewidth preferred" early veto and let the full forecast comparison decide.
 * A very small cut rank (e.g. 1 for uniform complete-bipartite) is a strong
 * signal that rankwidth will beat treewidth regardless of treewidth width. */
#define BRANCH_RANKWIDTH_LOW_RANK_BYPASS 3U
/* Minimum treewidth width at which the root fast path still considers bypassing
 * to rankwidth when prefix_cut_rank is small.  Below this the treewidth DP is
 * cheap enough that no bypass is warranted (e.g. path/star graphs with tw=1). */
#define BRANCH_FAST_PATH_RW_BYPASS_MIN_WIDTH 5U
#define BRANCH_SMALL_COMPONENT_CANONICAL_NVARS 5U
#define BRANCH_DELEGATION_DEPTH_TOP_K 6U

static bool build_active_residual_subinstance(const qsop_residual_t *residual, qsop_instance_t *sub,
                                              qsop_error_t *error);

static void free_subinstance(qsop_instance_t *sub) {
  if (sub == NULL) {
    return;
  }
  free(sub->unary);
  free(sub->edge_u);
  free(sub->edge_v);
  *sub = (qsop_instance_t){0};
}

static void add_saturating_u64(uint64_t *dst, uint64_t value) {
  if (UINT64_MAX - *dst < value) {
    *dst = UINT64_MAX;
  } else {
    *dst += value;
  }
}

static uint64_t saturating_mul_u64(uint64_t left, uint64_t right) {
  if (left != 0 && right > UINT64_MAX / left) {
    return UINT64_MAX;
  }
  return left * right;
}

static uint64_t binary_assignment_forecast(uint32_t nvars) {
  if (nvars >= 64U) {
    return UINT64_MAX;
  }
  return UINT64_C(1) << nvars;
}

static uint64_t treewidth_table_forecast(uint32_t width, uint32_t r) {
  const uint32_t bag_vars = width >= UINT32_MAX ? UINT32_MAX : width + 1U;
  return saturating_mul_u64(binary_assignment_forecast(bag_vars), r);
}

static uint64_t treewidth_single_mode_table_forecast(uint32_t width) {
  const uint32_t bag_vars = width >= UINT32_MAX ? UINT32_MAX : width + 1U;
  return binary_assignment_forecast(bag_vars);
}

static uint64_t treewidth_join_pair_forecast(uint32_t width, uint32_t nvars) {
  const uint32_t bag_vars = width >= UINT32_MAX ? UINT32_MAX : width + 1U;
  return saturating_mul_u64(binary_assignment_forecast(bag_vars), nvars);
}

static void max_u32(uint32_t *dst, uint32_t value) {
  if (value > *dst) {
    *dst = value;
  }
}

static void branch_trace_event(branch_search_stats_t *stats, const char *phase, uint64_t items) {
  qsop_trace_emit(stats->trace, phase, stats->depth, items, 0);
}

static void note_residual_shape(branch_search_stats_t *stats, const qsop_residual_t *residual) {
  max_u32(&stats->max_residual_vars, qsop_residual_active_vars(residual));
  max_u32(&stats->max_residual_edges, qsop_residual_active_edges(residual));
}

static bool note_component_shape(branch_search_stats_t *stats, const qsop_residual_t *residual,
                                 const uint32_t *component, uint32_t ncomponents,
                                 qsop_error_t *error) {
  note_residual_shape(stats, residual);
  max_u32(&stats->max_residual_components, ncomponents);
  if (ncomponents <= 1U) {
    max_u32(&stats->max_residual_largest_component, qsop_residual_active_vars(residual));
    return true;
  }

  uint32_t *sizes = calloc(ncomponents, sizeof(*sizes));
  if (sizes == NULL) {
    set_error(error, "out of memory while recording residual component shape");
    return false;
  }
  for (uint32_t v = 0; v < qsop_residual_nvars(residual); v++) {
    if (!qsop_residual_var_active(residual, v)) {
      continue;
    }
    if (component[v] >= ncomponents) {
      free(sizes);
      set_error(error, "internal error: residual component index is out of range");
      return false;
    }
    sizes[component[v]]++;
  }
  for (uint32_t c = 0; c < ncomponents; c++) {
    max_u32(&stats->max_residual_largest_component, sizes[c]);
  }
  free(sizes);
  return true;
}

static void note_width_probe(branch_search_stats_t *stats, const qsop_stats_t *sub_stats) {
  if (!sub_stats->width_diagnostics_available) {
    return;
  }
  max_u32(&stats->max_residual_min_fill_width, sub_stats->min_fill_width);
  max_u32(&stats->max_residual_prefix_cut_rank, sub_stats->prefix_cut_rank);
}

static void note_treewidth_skip(branch_search_stats_t *stats, const char *phase, uint64_t items) {
  stats->branch_treewidth_skips++;
  branch_trace_event(stats, phase, items);
}

static void note_rankwidth_skip(branch_search_stats_t *stats, const char *phase, uint64_t items) {
  stats->branch_rankwidth_skips++;
  branch_trace_event(stats, phase, items);
}

static uint64_t assignment_count(uint32_t nvars) {
  if (nvars >= 63U) {
    return UINT64_MAX;
  }
  return UINT64_C(1) << nvars;
}

static bool branch_count_add(const branch_search_stats_t *stats, uint64_t *dst, uint64_t value,
                             qsop_error_t *error) {
  if (stats->count_modulus != 0) {
    *dst = qsop_mod_add_u64(*dst, value % stats->count_modulus, stats->count_modulus);
    return true;
  }
  return qsop_count_add(dst, value, error);
}

static bool branch_count_mul(const branch_search_stats_t *stats, uint64_t left, uint64_t right,
                             uint64_t *out, qsop_error_t *error) {
  if (stats->count_modulus != 0) {
    *out = qsop_mod_mul_u64(left, right, stats->count_modulus);
    return true;
  }
  return qsop_count_mul(left, right, out, error);
}

static bool add_counts(uint32_t r, uint64_t *dst, const uint64_t *src,
                       const branch_search_stats_t *stats, qsop_error_t *error) {
  for (uint32_t residue = 0; residue < r; residue++) {
    if (!branch_count_add(stats, &dst[residue], src[residue], error)) {
      return false;
    }
  }
  return true;
}

static bool branch_counts_shift_add(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift,
                                    const branch_search_stats_t *stats, qsop_error_t *error) {
  if (r == 0 || dst == NULL || src == NULL) {
    set_error(error, "internal error: invalid branch residue shift-add argument");
    return false;
  }

  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t target = residue + delta;
    if (target >= r) {
      target -= r;
    }
    if (!branch_count_add(stats, &dst[target], src[residue], error)) {
      return false;
    }
  }
  return true;
}

static bool branch_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                                   const uint64_t *right, const branch_search_stats_t *stats,
                                   qsop_error_t *error) {
  if (r == 0 || dst == NULL || left == NULL || right == NULL) {
    set_error(error, "internal error: invalid branch residue convolution argument");
    return false;
  }

  qsop_counts_clear(r, dst);
  for (uint32_t a = 0; a < r; a++) {
    if (left[a] == 0) {
      continue;
    }
    for (uint32_t b = 0; b < r; b++) {
      if (right[b] == 0) {
        continue;
      }
      uint32_t target = a + b;
      if (target >= r) {
        target -= r;
      }
      uint64_t product = 0;
      if (!branch_count_mul(stats, left[a], right[b], &product, error) ||
          !branch_count_add(stats, &dst[target], product, error)) {
        return false;
      }
    }
  }
  return true;
}

static bool branch_counts_to_fourier(uint32_t r, const uint64_t *counts, const uint64_t *powers,
                                     uint64_t prime, uint64_t *modes, qsop_error_t *error) {
  if (r == 0 || counts == NULL || powers == NULL || modes == NULL) {
    set_error(error, "internal error: invalid branch Fourier transform argument");
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    uint64_t value = 0;
    for (uint32_t residue = 0; residue < r; residue++) {
      const uint64_t count = counts[residue] % prime;
      if (count == 0) {
        continue;
      }
      const uint64_t weight = powers[(size_t)mode * r + residue];
      value = qsop_mod_add_u64(value, qsop_mod_mul_u64(count, weight, prime), prime);
    }
    modes[mode] = value;
  }
  return true;
}

static bool branch_fourier_multiply(uint32_t r, uint64_t *acc, const uint64_t *part, uint64_t prime,
                                    qsop_error_t *error) {
  if (r == 0 || acc == NULL || part == NULL) {
    set_error(error, "internal error: invalid branch Fourier multiply argument");
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    acc[mode] = qsop_mod_mul_u64(acc[mode], part[mode], prime);
  }
  return true;
}

static void residual_cache_key_free(residual_cache_key_t *key) {
  if (key == NULL) {
    return;
  }

  free(key->active_var);
  free(key->active_edge);
  free(key->unary);
  free(key->edge_u);
  free(key->edge_v);
  *key = (residual_cache_key_t){0};
}

static bool residual_cache_key_create(const qsop_residual_t *residual, residual_cache_key_t *key,
                                      qsop_error_t *error) {
  if (key == NULL) {
    set_error(error, "internal error: null residual cache key output");
    return false;
  }
  *key = (residual_cache_key_t){0};

  const uint32_t nvars = qsop_residual_nvars(residual);
  const uint32_t nedges = qsop_residual_nedges(residual);
  key->fingerprint = qsop_residual_fingerprint(residual);
  key->r = qsop_residual_modulus(residual);
  key->nvars = nvars;
  key->nedges = nedges;
  key->constant = qsop_residual_constant(residual);
  key->active_vars = qsop_residual_active_vars(residual);
  key->active_edges = qsop_residual_active_edges(residual);

  key->active_var = malloc(nvars == 0 ? 1U : nvars);
  key->active_edge = malloc(nedges == 0 ? 1U : nedges);
  key->unary = malloc((nvars == 0 ? 1U : nvars) * sizeof(*key->unary));
  key->edge_u = malloc((nedges == 0 ? 1U : nedges) * sizeof(*key->edge_u));
  key->edge_v = malloc((nedges == 0 ? 1U : nedges) * sizeof(*key->edge_v));
  if (key->active_var == NULL || key->active_edge == NULL || key->unary == NULL ||
      key->edge_u == NULL || key->edge_v == NULL) {
    residual_cache_key_free(key);
    set_error(error, "out of memory while allocating residual cache key");
    return false;
  }

  for (uint32_t v = 0; v < nvars; v++) {
    const bool active = qsop_residual_var_active(residual, v);
    key->active_var[v] = active ? 1U : 0U;
    key->unary[v] = active ? qsop_residual_unary(residual, v) : 0U;
  }
  for (uint32_t e = 0; e < nedges; e++) {
    key->active_edge[e] = qsop_residual_edge_active(residual, e) ? 1U : 0U;
    key->edge_u[e] = qsop_residual_edge_u(residual, e);
    key->edge_v[e] = qsop_residual_edge_v(residual, e);
  }

  return true;
}

static uint64_t residual_cache_key_fingerprint(const residual_cache_key_t *key) {
  uint64_t fingerprint = UINT64_C(1469598103934665603);
  fingerprint ^= key->canonical ? 1U : 0U;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= key->r;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= key->nvars;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= key->nedges;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= key->constant;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= key->active_vars;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= key->active_edges;
  fingerprint *= UINT64_C(1099511628211);
  for (uint32_t v = 0; v < key->nvars; v++) {
    fingerprint ^= key->active_var[v];
    fingerprint *= UINT64_C(1099511628211);
    fingerprint ^= key->unary[v];
    fingerprint *= UINT64_C(1099511628211);
  }
  for (uint32_t e = 0; e < key->nedges; e++) {
    fingerprint ^= key->active_edge[e];
    fingerprint *= UINT64_C(1099511628211);
    fingerprint ^= key->edge_u[e];
    fingerprint *= UINT64_C(1099511628211);
    fingerprint ^= key->edge_v[e];
    fingerprint *= UINT64_C(1099511628211);
  }
  return fingerprint;
}

static bool residual_cache_key_create_from_instance(const qsop_instance_t *sub, uint64_t constant,
                                                    residual_cache_key_t *key,
                                                    qsop_error_t *error) {
  if (key == NULL) {
    set_error(error, "internal error: null residual cache key output");
    return false;
  }
  *key = (residual_cache_key_t){0};

  key->canonical = true;
  key->r = sub->r;
  key->nvars = sub->nvars;
  key->nedges = sub->nedges;
  key->constant = constant;
  key->active_vars = sub->nvars;
  key->active_edges = sub->nedges;

  key->active_var = malloc(sub->nvars == 0 ? 1U : sub->nvars);
  key->active_edge = malloc(sub->nedges == 0 ? 1U : sub->nedges);
  key->unary = malloc((sub->nvars == 0 ? 1U : sub->nvars) * sizeof(*key->unary));
  key->edge_u = malloc((sub->nedges == 0 ? 1U : sub->nedges) * sizeof(*key->edge_u));
  key->edge_v = malloc((sub->nedges == 0 ? 1U : sub->nedges) * sizeof(*key->edge_v));
  if (key->active_var == NULL || key->active_edge == NULL || key->unary == NULL ||
      key->edge_u == NULL || key->edge_v == NULL) {
    residual_cache_key_free(key);
    set_error(error, "out of memory while allocating canonical residual cache key");
    return false;
  }

  memset(key->active_var, 1, (size_t)sub->nvars * sizeof(*key->active_var));
  memset(key->active_edge, 1, (size_t)sub->nedges * sizeof(*key->active_edge));
  memcpy(key->unary, sub->unary, (size_t)sub->nvars * sizeof(*key->unary));
  memcpy(key->edge_u, sub->edge_u, (size_t)sub->nedges * sizeof(*key->edge_u));
  memcpy(key->edge_v, sub->edge_v, (size_t)sub->nedges * sizeof(*key->edge_v));
  key->fingerprint = residual_cache_key_fingerprint(key);
  return true;
}

static bool residual_cache_key_create_canonical_small(const qsop_residual_t *residual,
                                                      residual_cache_key_t *key,
                                                      qsop_error_t *error) {
  qsop_instance_t sub = {0};
  if (!build_active_residual_subinstance(residual, &sub, error)) {
    return false;
  }
  const bool ok =
      residual_cache_key_create_from_instance(&sub, qsop_residual_constant(residual), key, error);
  free_subinstance(&sub);
  return ok;
}

static bool residual_cache_key_create_for_store(const qsop_residual_t *residual,
                                                residual_cache_key_t *key, qsop_error_t *error) {
  if (qsop_residual_active_vars(residual) <= BRANCH_SMALL_COMPONENT_CANONICAL_NVARS) {
    return residual_cache_key_create_canonical_small(residual, key, error);
  }
  return residual_cache_key_create(residual, key, error);
}

static bool residual_cache_key_matches_key(const residual_cache_key_t *lhs,
                                           const residual_cache_key_t *rhs) {
  if (lhs->fingerprint != rhs->fingerprint || lhs->canonical != rhs->canonical ||
      lhs->r != rhs->r || lhs->nvars != rhs->nvars || lhs->nedges != rhs->nedges ||
      lhs->constant != rhs->constant || lhs->active_vars != rhs->active_vars ||
      lhs->active_edges != rhs->active_edges) {
    return false;
  }

  for (uint32_t v = 0; v < lhs->nvars; v++) {
    if (lhs->active_var[v] != rhs->active_var[v] || lhs->unary[v] != rhs->unary[v]) {
      return false;
    }
  }
  for (uint32_t e = 0; e < lhs->nedges; e++) {
    if (lhs->active_edge[e] != rhs->active_edge[e] || lhs->edge_u[e] != rhs->edge_u[e] ||
        lhs->edge_v[e] != rhs->edge_v[e]) {
      return false;
    }
  }
  return true;
}

static bool residual_cache_key_matches_residual(const residual_cache_key_t *key,
                                                const qsop_residual_t *residual) {
  if (key->canonical) {
    return false;
  }
  if (key->fingerprint != qsop_residual_fingerprint(residual) ||
      key->r != qsop_residual_modulus(residual) || key->nvars != qsop_residual_nvars(residual) ||
      key->nedges != qsop_residual_nedges(residual) ||
      key->constant != qsop_residual_constant(residual) ||
      key->active_vars != qsop_residual_active_vars(residual) ||
      key->active_edges != qsop_residual_active_edges(residual)) {
    return false;
  }

  for (uint32_t e = 0; e < key->nedges; e++) {
    if (key->edge_u[e] != qsop_residual_edge_u(residual, e) ||
        key->edge_v[e] != qsop_residual_edge_v(residual, e)) {
      return false;
    }
  }

  for (uint32_t v = 0; v < key->nvars; v++) {
    const bool active = qsop_residual_var_active(residual, v);
    if (key->active_var[v] != (active ? 1U : 0U)) {
      return false;
    }
    if (active && key->unary[v] != qsop_residual_unary(residual, v)) {
      return false;
    }
  }

  for (uint32_t e = 0; e < key->nedges; e++) {
    if (key->active_edge[e] != (qsop_residual_edge_active(residual, e) ? 1U : 0U)) {
      return false;
    }
  }

  return true;
}

static bool residual_cache_find(const residual_cache_t *cache, const qsop_residual_t *residual,
                                const residual_cache_entry_t **out, bool *out_canonical_lookup,
                                qsop_error_t *error) {
  *out = NULL;
  if (out_canonical_lookup != NULL) {
    *out_canonical_lookup = false;
  }
  if (cache->bucket_count == 0) {
    return true;
  }

  if (qsop_residual_active_vars(residual) <= BRANCH_SMALL_COMPONENT_CANONICAL_NVARS) {
    if (out_canonical_lookup != NULL) {
      *out_canonical_lookup = true;
    }
    residual_cache_key_t lookup = {0};
    if (!residual_cache_key_create_canonical_small(residual, &lookup, error)) {
      return false;
    }

    const size_t bucket = (size_t)(lookup.fingerprint % cache->bucket_count);
    for (size_t i = cache->buckets[bucket]; i != SIZE_MAX; i = cache->entries[i].next) {
      const residual_cache_entry_t *entry = &cache->entries[i];
      if (residual_cache_key_matches_key(&entry->key, &lookup)) {
        *out = entry;
        break;
      }
    }
    residual_cache_key_free(&lookup);
    return true;
  }

  const uint64_t fingerprint = qsop_residual_fingerprint(residual);
  const size_t bucket = (size_t)(fingerprint % cache->bucket_count);
  for (size_t i = cache->buckets[bucket]; i != SIZE_MAX; i = cache->entries[i].next) {
    const residual_cache_entry_t *entry = &cache->entries[i];
    if (entry->key.fingerprint == fingerprint &&
        residual_cache_key_matches_residual(&entry->key, residual)) {
      *out = entry;
      return true;
    }
  }
  return true;
}

static bool residual_cache_reserve(residual_cache_t *cache, size_t needed, qsop_error_t *error) {
  if (needed <= cache->cap) {
    return true;
  }

  size_t new_cap = cache->cap == 0 ? 32U : cache->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "residual cache is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / sizeof(*cache->entries)) {
    set_error(error, "residual cache is too large");
    return false;
  }

  residual_cache_entry_t *new_entries = realloc(cache->entries, new_cap * sizeof(*cache->entries));
  if (new_entries == NULL) {
    set_error(error, "out of memory while growing residual cache");
    return false;
  }

  cache->entries = new_entries;
  cache->cap = new_cap;
  return true;
}

static bool residual_cache_rehash(residual_cache_t *cache, size_t bucket_count,
                                  qsop_error_t *error) {
  if (bucket_count == 0 || bucket_count > SIZE_MAX / sizeof(*cache->buckets)) {
    set_error(error, "residual cache is too large");
    return false;
  }

  size_t *buckets = malloc(bucket_count * sizeof(*buckets));
  if (buckets == NULL) {
    set_error(error, "out of memory while allocating residual cache buckets");
    return false;
  }
  for (size_t i = 0; i < bucket_count; i++) {
    buckets[i] = SIZE_MAX;
  }

  for (size_t i = 0; i < cache->len; i++) {
    const size_t bucket = (size_t)(cache->entries[i].key.fingerprint % bucket_count);
    cache->entries[i].next = buckets[bucket];
    buckets[bucket] = i;
  }

  free(cache->buckets);
  cache->buckets = buckets;
  cache->bucket_count = bucket_count;
  return true;
}

static bool residual_cache_store(residual_cache_t *cache, const qsop_residual_t *residual,
                                 const uint64_t *counts, uint64_t search_nodes,
                                 bool *out_canonical_store, qsop_error_t *error) {
  residual_cache_entry_t entry = {0};
  entry.next = SIZE_MAX;
  entry.search_nodes = search_nodes;
  if (out_canonical_store != NULL) {
    *out_canonical_store = false;
  }
  if (!residual_cache_key_create_for_store(residual, &entry.key, error)) {
    return false;
  }
  if (out_canonical_store != NULL) {
    *out_canonical_store = entry.key.canonical;
  }

  if (entry.key.r > UINT32_MAX) {
    residual_cache_key_free(&entry.key);
    set_error(error, "count-table branch residual cache requires R <= UINT32_MAX");
    return false;
  }
  if (!qsop_counts_alloc((uint32_t)entry.key.r, &entry.counts, error)) {
    residual_cache_key_free(&entry.key);
    return false;
  }
  memcpy(entry.counts, counts, (size_t)(uint32_t)entry.key.r * sizeof(*entry.counts));

  if (!residual_cache_reserve(cache, cache->len + 1U, error)) {
    residual_cache_key_free(&entry.key);
    free(entry.counts);
    return false;
  }
  if (cache->bucket_count == 0) {
    if (!residual_cache_rehash(cache, 64U, error)) {
      residual_cache_key_free(&entry.key);
      free(entry.counts);
      return false;
    }
  } else if (cache->bucket_count <= SIZE_MAX / 2U && cache->len + 1U > cache->bucket_count * 2U &&
             !residual_cache_rehash(cache, cache->bucket_count * 2U, error)) {
    residual_cache_key_free(&entry.key);
    free(entry.counts);
    return false;
  }

  const size_t bucket = (size_t)(entry.key.fingerprint % cache->bucket_count);
  entry.next = cache->buckets[bucket];
  cache->entries[cache->len] = entry;
  cache->buckets[bucket] = cache->len;
  cache->len++;
  return true;
}

static void residual_cache_free(residual_cache_t *cache) {
  if (cache == NULL) {
    return;
  }

  for (size_t i = 0; i < cache->len; i++) {
    residual_cache_key_free(&cache->entries[i].key);
    free(cache->entries[i].counts);
  }
  free(cache->entries);
  free(cache->buckets);
  *cache = (residual_cache_t){0};
}

static uint64_t residual_cache_canonical_entries(const residual_cache_t *cache) {
  uint64_t entries = 0;
  for (size_t i = 0; i < cache->len; i++) {
    if (cache->entries[i].key.canonical) {
      entries++;
    }
  }
  return entries;
}

static uint64_t residual_cache_key_bytes(const residual_cache_t *cache) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < cache->len; i++) {
    const residual_cache_key_t *key = &cache->entries[i].key;
    add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->active_var)));
    add_saturating_u64(&bytes,
                       saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->active_edge)));
    add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->unary)));
    add_saturating_u64(&bytes, saturating_mul_u64(saturating_mul_u64((uint64_t)key->nedges, 3U),
                                                  sizeof(*key->edge_u)));
  }
  return bytes;
}

static uint64_t residual_cache_count_bytes(const residual_cache_t *cache) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < cache->len; i++) {
    add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)cache->entries[i].key.r,
                                                  sizeof(*cache->entries[i].counts)));
  }
  return bytes;
}

static uint64_t residual_cache_estimated_bytes(const residual_cache_t *cache) {
  uint64_t bytes = saturating_mul_u64((uint64_t)cache->cap, sizeof(*cache->entries));
  add_saturating_u64(&bytes,
                     saturating_mul_u64((uint64_t)cache->bucket_count, sizeof(*cache->buckets)));
  add_saturating_u64(&bytes, residual_cache_key_bytes(cache));
  add_saturating_u64(&bytes, residual_cache_count_bytes(cache));
  return bytes;
}

static bool build_residual_subinstance(const qsop_residual_t *residual, const uint32_t *component,
                                       uint32_t wanted, qsop_instance_t *sub, qsop_error_t *error) {
  const uint32_t source_vars = qsop_residual_nvars(residual);
  const uint32_t source_edges = qsop_residual_nedges(residual);
  uint32_t *map = malloc((source_vars == 0 ? 1U : source_vars) * sizeof(*map));
  if (map == NULL) {
    set_error(error, "out of memory while building residual component subinstance");
    return false;
  }
  for (uint32_t v = 0; v < source_vars; v++) {
    map[v] = UINT32_MAX;
  }

  uint32_t nvars = 0;
  for (uint32_t v = 0; v < source_vars; v++) {
    if (component[v] == wanted) {
      map[v] = nvars++;
    }
  }

  const uint64_t r = qsop_residual_modulus(residual);
  uint32_t nedges = 0;
  for (uint32_t e = 0; e < source_edges; e++) {
    const uint32_t u = qsop_residual_edge_u(residual, e);
    if (qsop_residual_edge_active(residual, e) && component[u] == wanted) {
      nedges++;
    }
  }

  *sub = (qsop_instance_t){
      .r = r,
      .nvars = nvars,
      .norm_h = 0,
      .constant = 0,
      .nedges = nedges,
  };
  sub->unary = calloc(nvars == 0 ? 1U : nvars, sizeof(*sub->unary));
  sub->edge_u = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_u));
  sub->edge_v = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_v));
  if (sub->unary == NULL || sub->edge_u == NULL || sub->edge_v == NULL) {
    free(map);
    free_subinstance(sub);
    set_error(error, "out of memory while allocating residual component subinstance");
    return false;
  }

  for (uint32_t v = 0; v < source_vars; v++) {
    if (component[v] == wanted) {
      sub->unary[map[v]] = qsop_residual_unary(residual, v);
    }
  }

  uint32_t out_edge = 0;
  for (uint32_t e = 0; e < source_edges; e++) {
    const uint32_t u = qsop_residual_edge_u(residual, e);
    if (!qsop_residual_edge_active(residual, e) || component[u] != wanted) {
      continue;
    }
    const uint32_t v = qsop_residual_edge_v(residual, e);
    sub->edge_u[out_edge] = map[u];
    sub->edge_v[out_edge] = map[v];
    out_edge++;
  }
  if (!qsop_canonicalize_small_component(sub, BRANCH_SMALL_COMPONENT_CANONICAL_NVARS, error)) {
    free(map);
    free_subinstance(sub);
    return false;
  }

  free(map);
  return true;
}

static bool build_active_residual_subinstance(const qsop_residual_t *residual, qsop_instance_t *sub,
                                              qsop_error_t *error) {
  const uint32_t nvars = qsop_residual_nvars(residual);
  uint32_t *component = malloc((nvars == 0 ? 1U : nvars) * sizeof(*component));
  if (component == NULL) {
    set_error(error, "out of memory while building active residual subinstance");
    return false;
  }
  for (uint32_t v = 0; v < nvars; v++) {
    component[v] = qsop_residual_var_active(residual, v) ? 0U : UINT32_MAX;
  }
  const bool ok = build_residual_subinstance(residual, component, 0, sub, error);
  free(component);
  return ok;
}

static void merge_delegated_stats(branch_search_stats_t *stats, const qsop_solve_stats_t *delegated,
                                  uint32_t delegated_nvars) {
  add_saturating_u64(&stats->leaves, assignment_count(delegated_nvars));
  add_saturating_u64(&stats->treewidth_delegations, delegated->treewidth_delegations);
  add_saturating_u64(&stats->rankwidth_delegations, delegated->rankwidth_delegations);
  add_saturating_u64(&stats->table_entries, delegated->table_entries);
  add_saturating_u64(&stats->signature_entries, delegated->signature_entries);
  add_saturating_u64(&stats->join_pairs, delegated->join_pairs);
  add_saturating_u64(&stats->join_signature_pairs, delegated->join_signature_pairs);
  add_saturating_u64(&stats->branch_fallthroughs, delegated->branch_fallthroughs);
  add_saturating_u64(&stats->branch_treewidth_skips, delegated->branch_treewidth_skips);
  add_saturating_u64(&stats->branch_rankwidth_skips, delegated->branch_rankwidth_skips);
  if (delegated->max_table_entries > stats->max_table_entries) {
    stats->max_table_entries = delegated->max_table_entries;
  }
  if (delegated->max_signature_entries > stats->max_signature_entries) {
    stats->max_signature_entries = delegated->max_signature_entries;
  }
  if (delegated->decomposition_width > stats->decomposition_width) {
    stats->decomposition_width = delegated->decomposition_width;
  }
  if (delegated->rankwidth_cutrank_width > stats->rankwidth_cutrank_width) {
    stats->rankwidth_cutrank_width = delegated->rankwidth_cutrank_width;
  }
  if (delegated->rankwidth_table_forecast > stats->rankwidth_table_forecast) {
    stats->rankwidth_table_forecast = delegated->rankwidth_table_forecast;
  }
  if (delegated->rankwidth_join_pair_forecast > stats->rankwidth_join_pair_forecast) {
    stats->rankwidth_join_pair_forecast = delegated->rankwidth_join_pair_forecast;
  }
  max_u32(&stats->max_residual_vars, delegated->max_residual_vars);
  max_u32(&stats->max_residual_edges, delegated->max_residual_edges);
  max_u32(&stats->max_residual_components, delegated->max_residual_components);
  max_u32(&stats->max_residual_largest_component, delegated->max_residual_largest_component);
  max_u32(&stats->max_residual_min_fill_width, delegated->max_residual_min_fill_width);
  max_u32(&stats->max_residual_prefix_cut_rank, delegated->max_residual_prefix_cut_rank);
}

/* Shared by both the count-table and single-Fourier decision paths: the two differ only in
 * whether the treewidth table forecast carries the modulus factor r, which the caller already
 * folded into `treewidth_table_forecast_value`. */
static bool rankwidth_should_override_treewidth(uint32_t treewidth_width, uint32_t decision_width,
                                                uint64_t treewidth_table_forecast_value) {
  return decision_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH &&
         binary_assignment_forecast(decision_width) < treewidth_table_forecast_value &&
         treewidth_width > decision_width + BRANCH_RANKWIDTH_TREEWIDTH_MARGIN;
}

/* Shared cost model for both the count-table and single-Fourier decision paths: returns true when
 * the estimated ns for rankwidth beats treewidth by the policy speedup margin. The two modes
 * differ only in the forecast VALUES the caller passes (the count-table path folds the modulus r
 * into the table forecasts; single-Fourier does not); the signature estimate (2^cutrank) and the
 * formula are identical. With no treewidth competitor, rankwidth is favored. */
static bool branch_cost_model_favors_rankwidth(const qsop_branch_policy_t *pol,
                                               bool treewidth_available, uint64_t rw_table,
                                               uint64_t rw_join, uint64_t rw_sig, uint64_t tw_table,
                                               uint64_t tw_join) {
  if (!treewidth_available) {
    return true;
  }
  const uint64_t rw_est = saturating_mul_u64(pol->C_rw_table, rw_table) +
                          saturating_mul_u64(pol->C_rw_join, rw_join) +
                          saturating_mul_u64(pol->C_rw_sig, rw_sig) + pol->rw_fixed_overhead_ns +
                          pol->rw_memory_penalty_ns;
  const uint64_t tw_est = saturating_mul_u64(pol->C_tw_table, tw_table) +
                          saturating_mul_u64(pol->C_tw_join, tw_join) + pol->tw_fixed_overhead_ns;
  return tw_est != 0 && (double)rw_est * pol->rw_min_speedup < (double)tw_est;
}

/* Early "treewidth is narrow enough, prefer it" veto shared by both decision paths: treewidth is
 * within the delegate band and the cut-rank proxy is not small enough (<= BRANCH_RANKWIDTH_LOW_RANK
 * _BYPASS) to signal a big rankwidth win. */
static bool branch_treewidth_preferred(bool treewidth_available, uint32_t prefix_cut_rank,
                                       uint32_t treewidth_width) {
  return treewidth_available && prefix_cut_rank > BRANCH_RANKWIDTH_LOW_RANK_BYPASS &&
         treewidth_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH + BRANCH_RANKWIDTH_TREEWIDTH_MARGIN;
}

/* Predicted cost of *deciding* whether rankwidth wins: generate a rank decomposition, then measure
 * the cut rank at each of its ~2*nvars nodes, each a GF(2) rank over nvars-bit rows. That is
 * O(nvars^2 * words) bitset work, superlinear in the component size and with a large constant. */
static uint64_t rankwidth_probe_estimate_ns(const qsop_branch_policy_t *pol, uint32_t nvars) {
  const uint64_t words = ((uint64_t)nvars + 63U) / 64U;
  uint64_t units = saturating_mul_u64((uint64_t)nvars, (uint64_t)nvars);
  units = saturating_mul_u64(units, words == 0 ? 1U : words);
  return saturating_mul_u64(units, pol->C_rw_probe);
}

/* Never spend more deciding than the treewidth table alone will cost.
 *
 * The table forecast is a hard lower bound on the treewidth solve, and rankwidth's whole benefit is
 * making that table smaller -- so it also bounds what a rankwidth win could possibly recover. A
 * probe that costs more than the entire prize is never worth running, however the forecasts compare
 * afterwards. This is what the model was missing: it estimated the two *solves* and then paid an
 * unbudgeted amount computing the rankwidth estimate.
 *
 * Only applies when treewidth is actually usable. When it is not, rankwidth is the only backend
 * left and the probe has to be attempted whatever it costs. */
static bool branch_rankwidth_probe_too_costly(const qsop_branch_policy_t *pol,
                                              bool treewidth_usable, uint32_t nvars,
                                              uint64_t treewidth_table_entries) {
  if (!treewidth_usable) {
    return false;
  }
  return rankwidth_probe_estimate_ns(pol, nvars) >
         saturating_mul_u64(treewidth_table_entries, pol->C_tw_table);
}

/* Shared "treewidth is obviously cheap, don't bother probing rankwidth" pre-probe check
 * (single-Fourier table forecast, no r factor). */
static bool branch_treewidth_is_cheap(const qsop_branch_policy_t *pol, bool treewidth_available,
                                      uint32_t treewidth_width, uint32_t nvars,
                                      uint32_t prefix_cut_rank) {
  const bool low_rank_bypass = treewidth_available && prefix_cut_rank <= pol->rw_low_rank_bypass;
  return treewidth_available && !low_rank_bypass &&
         (treewidth_width <= pol->rw_min_treewidth_width ||
          treewidth_single_mode_table_forecast(treewidth_width) <= pol->rw_min_treewidth_forecast ||
          (nvars < pol->rw_min_residual_vars && treewidth_width <= 5U));
}

/* Public CLI helper for the single-Fourier auto path: true when the shared pre-probe vetoes make
 * treewidth the clear choice (obviously-cheap treewidth, or the narrow-treewidth veto), so the
 * direct whole-instance treewidth path can be taken without missing a rankwidth win. When false,
 * the caller should fall into the branch recursion, where rankwidth is actually probed and the
 * shared cost model decides. Mirrors the recursion's pre-probe skip conditions exactly. */
bool qsop_branch_single_treewidth_clearly_preferred(uint32_t treewidth_width,
                                                    uint32_t prefix_cut_rank, uint32_t nvars,
                                                    const qsop_branch_policy_t *policy) {
  const qsop_branch_policy_t pol = branch_policy_normalize(policy);
  /* Treewidth trivially cheap: for these sizes rankwidth's fixed overhead alone exceeds the
   * treewidth DP's estimated cost, so the recursion's cost model would always pick treewidth --
   * take the direct path. Unlike branch_treewidth_is_cheap this ignores the low-rank bypass, which
   * only matters when rankwidth could plausibly win (it cannot here). */
  const bool trivially_cheap =
      treewidth_width <= pol.rw_min_treewidth_width ||
      treewidth_single_mode_table_forecast(treewidth_width) <= pol.rw_min_treewidth_forecast ||
      (nvars < pol.rw_min_residual_vars && treewidth_width <= 5U);
  /* Callers only reach here with treewidth inside its single-Fourier cap, so treewidth is usable
   * and the probe-cost veto applies: on a large component, deciding costs more than solving. */
  const bool probe_too_costly = branch_rankwidth_probe_too_costly(
      &pol, true, nvars, treewidth_single_mode_table_forecast(treewidth_width));
  return trivially_cheap || probe_too_costly ||
         branch_treewidth_preferred(true, prefix_cut_rank, treewidth_width);
}

/* Maximum cutrank width tried during calibration runs. Wider sub-problems are
   skipped to bound calibration cost even when policy vetoqs are bypassed. */
#define BRANCH_CALIBRATION_MAX_WIDTH 20U

/* precomputed_order: if non-NULL, used instead of running min-fill inside the from-treewidth
 * generator (D2 optimization: share one min-fill run with the treewidth solver path). */
static bool branch_try_rankwidth_delegate(qsop_instance_t *sub, uint64_t *counts,
                                          uint32_t treewidth_width, uint32_t prefix_cut_rank,
                                          bool treewidth_available, uint32_t constant_shift,
                                          const uint32_t *precomputed_order,
                                          branch_search_stats_t *stats,
                                          branch_rw_decision_data_t *rw_data, bool *out_delegated,
                                          qsop_error_t *error) {
  *out_delegated = false;
  /* NONE policy: skip rankwidth entirely without any attempt. */
  if (stats->rw_source == QSOP_BRANCH_RW_SOURCE_NONE) {
    if (rw_data != NULL) {
      *rw_data = (branch_rw_decision_data_t){.attempted = false};
    }
    return true;
  }
  if (rw_data != NULL) {
    *rw_data = (branch_rw_decision_data_t){.attempted = true};
  }
  /* In calibration mode, bypass policy vetoqs so both backends get timed. */
  const bool calibrating = (stats->sink != NULL && stats->sink->calibrate_backends);
  /* True when a veto fired in calibration mode: rankwidth is timed but its counts discarded. */
  bool calibration_timing_only = false;
  uint64_t treewidth_table = 0;
  uint64_t treewidth_join_pairs = 0;
  if (treewidth_available) {
    treewidth_table = treewidth_table_forecast(treewidth_width, (uint32_t)sub->r);
    treewidth_join_pairs = treewidth_join_pair_forecast(treewidth_width, sub->nvars);
    branch_trace_event(stats, "branch.treewidth_table_forecast", treewidth_table);
    branch_trace_event(stats, "branch.treewidth_join_pair_forecast", treewidth_join_pairs);
  }

  /* Policy fields are pre-normalized in branch_policy_normalize(); read directly. */
  const qsop_branch_policy_t *pol = &stats->policy;
  const bool policy_low_rank_bypass =
      treewidth_available && prefix_cut_rank <= pol->rw_low_rank_bypass;

  /* A3: Early cheap-treewidth veto — skip the expensive rankwidth probe when treewidth
   * is obviously cheap.  Bypassed when prefix_cut_rank is very small (low-rank bypass). */
  if (!policy_low_rank_bypass && !calibrating && treewidth_available) {
    if (treewidth_width <= pol->rw_min_treewidth_width) {
      note_rankwidth_skip(stats, "branch.rankwidth_skip_treewidth_cheap", treewidth_width);
      if (rw_data != NULL) {
        rw_data->veto_reason = "treewidth-cheap";
        rw_data->attempted = false;
      }
      return true;
    }
    if (treewidth_table <= pol->rw_min_treewidth_forecast) {
      note_rankwidth_skip(stats, "branch.rankwidth_skip_treewidth_forecast_cheap", treewidth_table);
      if (rw_data != NULL) {
        rw_data->veto_reason = "treewidth-forecast-cheap";
        rw_data->attempted = false;
      }
      return true;
    }
    const uint32_t n_active = sub->nvars;
    if (n_active < pol->rw_min_residual_vars && treewidth_width <= 5U) {
      note_rankwidth_skip(stats, "branch.rankwidth_skip_small_residual_treewidth_cheap", n_active);
      if (rw_data != NULL) {
        rw_data->veto_reason = "small-residual-treewidth-cheap";
        rw_data->attempted = false;
      }
      return true;
    }
  }

  /* Early veto: if treewidth is narrow (≤ max+margin), prefer treewidth.
   * Exception: bypass when prefix_cut_rank is very small — a strong signal
   * that rankwidth will compress the problem far more than treewidth can. */
  if (branch_treewidth_preferred(treewidth_available, prefix_cut_rank, treewidth_width)) {
    note_rankwidth_skip(stats, "branch.rankwidth_skip_treewidth_preferred", treewidth_width);
    if (rw_data != NULL) {
      rw_data->veto_reason = "rw_treewidth_preferred";
    }
    if (!calibrating) {
      return true;
    }
    /* Calibration: fall through to time rankwidth for comparison. */
    calibration_timing_only = true;
  }

  /* Probing costs more than the treewidth table it could shrink. */
  if (!calibrating &&
      branch_rankwidth_probe_too_costly(
          pol, treewidth_available && treewidth_width <= BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH,
          sub->nvars, treewidth_table)) {
    note_rankwidth_skip(stats, "branch.rankwidth_skip_probe_cost", sub->nvars);
    if (rw_data != NULL) {
      rw_data->veto_reason = "rw_probe_cost_exceeds_treewidth_table";
      rw_data->attempted = false;
    }
    return true;
  }
  if (treewidth_available && prefix_cut_rank > BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH &&
      prefix_cut_rank + BRANCH_RANKWIDTH_TREEWIDTH_MARGIN >= treewidth_width) {
    note_rankwidth_skip(stats, "branch.rankwidth_skip_prefix_proxy", prefix_cut_rank);
    if (rw_data != NULL) {
      rw_data->veto_reason = "rw_prefix_proxy_rejected";
    }
    if (!calibrating) {
      return true;
    }
    /* Calibration: fall through to time rankwidth for comparison. */
    calibration_timing_only = true;
  }

  /* Select decomposition generator based on rw_source policy. */
  qsop_rankwidth_generator_t primary_gen =
      (stats->rw_source == QSOP_BRANCH_RW_SOURCE_FROM_TREEWIDTH ||
       stats->rw_source == QSOP_BRANCH_RW_SOURCE_AUTO ||
       stats->rw_source == QSOP_BRANCH_RW_SOURCE_BOTH)
          ? QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH
          : QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT;

  qsop_rankwidth_decomposition_t *decomposition = NULL;
  const uint64_t generate_start_ns = qsop_trace_now_ns();
  const uint64_t generate_start = qsop_trace_begin(stats->trace);
  const bool use_precomputed =
      precomputed_order != NULL && primary_gen == QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH;
  const bool gen_ok =
      use_precomputed
          ? qsop_rankwidth_decomposition_from_order(sub, precomputed_order, &decomposition, error)
          : qsop_rankwidth_decomposition_generate(sub, primary_gen, &decomposition, error);
  if (!gen_ok) {
    return false;
  }

  uint32_t cutrank_width = 0;
  if (!qsop_rankwidth_decomposition_width(sub, decomposition, &cutrank_width, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  qsop_trace_emit_elapsed(stats->trace, "branch.rankwidth_probe", stats->depth, cutrank_width,
                          generate_start);
  branch_trace_event(stats, "branch.rankwidth_cutrank_probe", cutrank_width);
  max_u32(&stats->rankwidth_cutrank_width, cutrank_width);
  uint64_t rankwidth_table_forecast =
      saturating_mul_u64(binary_assignment_forecast(cutrank_width), sub->r);
  uint64_t rankwidth_join_pair_forecast = 0;
  if (!qsop_rankwidth_decomposition_forecast(sub, decomposition, &rankwidth_table_forecast,
                                             &rankwidth_join_pair_forecast, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  const uint64_t generation_ns = qsop_trace_elapsed_ns(generate_start_ns);
  branch_trace_event(stats, "branch.rankwidth_table_forecast", rankwidth_table_forecast);
  branch_trace_event(stats, "branch.rankwidth_join_pair_forecast", rankwidth_join_pair_forecast);

  /* BOTH policy: also try the native generator and keep whichever forecasts better. */
  if (stats->rw_source == QSOP_BRANCH_RW_SOURCE_BOTH &&
      primary_gen == QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH) {
    qsop_rankwidth_decomposition_t *native_dec = NULL;
    if (qsop_rankwidth_decomposition_generate(sub, QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
                                              &native_dec, error)) {
      uint64_t native_table = saturating_mul_u64(binary_assignment_forecast(cutrank_width), sub->r);
      uint64_t native_join = 0;
      if (qsop_rankwidth_decomposition_forecast(sub, native_dec, &native_table, &native_join,
                                                NULL)) {
        if (native_table < rankwidth_table_forecast ||
            (native_table == rankwidth_table_forecast &&
             native_join < rankwidth_join_pair_forecast)) {
          qsop_rankwidth_decomposition_free(decomposition);
          decomposition = native_dec;
          native_dec = NULL;
          rankwidth_table_forecast = native_table;
          rankwidth_join_pair_forecast = native_join;
          branch_trace_event(stats, "branch.rankwidth_source_native_preferred", native_table);
        }
      }
      qsop_rankwidth_decomposition_free(native_dec);
    } else {
      /* Native generation failed; clear error and proceed with primary. */
      if (error != NULL) {
        error->message[0] = '\0';
      }
    }
  }

  if (rankwidth_table_forecast > stats->rankwidth_table_forecast) {
    stats->rankwidth_table_forecast = rankwidth_table_forecast;
  }
  if (rankwidth_join_pair_forecast > stats->rankwidth_join_pair_forecast) {
    stats->rankwidth_join_pair_forecast = rankwidth_join_pair_forecast;
  }

  if (rw_data != NULL) {
    rw_data->generation_ms = branch_ns_to_ms(generation_ns);
    rw_data->cutrank_width = cutrank_width;
    rw_data->forecast_entries = rankwidth_table_forecast;
    rw_data->forecast_join_pairs = rankwidth_join_pair_forecast;
  }

  const bool rankwidth_table_forecast_wins =
      !treewidth_available || rankwidth_table_forecast < treewidth_table;
  const bool rankwidth_join_forecast_wins =
      !treewidth_available || rankwidth_join_pair_forecast <= treewidth_join_pairs;

  /* A4: Cost-model veto — reject rankwidth unless estimated ns win is decisive. */
  bool cost_model_rejects = false;
  if (treewidth_available && !calibrating) {
    /* Signature count estimate: 2^cutrank_width (table forecast carries an extra r factor). */
    const uint64_t sig_est = sub->r > 0 ? (rankwidth_table_forecast / sub->r)
                                        : binary_assignment_forecast(cutrank_width);
    cost_model_rejects = !branch_cost_model_favors_rankwidth(pol, true, rankwidth_table_forecast,
                                                             rankwidth_join_pair_forecast, sig_est,
                                                             treewidth_table, treewidth_join_pairs);
  }

  const bool use_rankwidth =
      !cost_model_rejects && rankwidth_table_forecast_wins && rankwidth_join_forecast_wins &&
      (!treewidth_available || treewidth_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH ||
       rankwidth_should_override_treewidth(treewidth_width, cutrank_width, treewidth_table));
  if (!use_rankwidth || cutrank_width > BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
    const char *skip_phase = "branch.rankwidth_skip_policy";
    const char *veto = "rw_policy_rejected";
    if (cutrank_width > BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
      skip_phase = "branch.rankwidth_skip_width";
      veto = "rw_width_above_cap";
    } else if (cost_model_rejects) {
      skip_phase = "branch.rankwidth_skip_cost_model";
      veto = "rw_cost_model_rejected";
    } else if (!rankwidth_table_forecast_wins) {
      skip_phase = "branch.rankwidth_skip_table_forecast";
      veto = "rw_predicted_slower_table";
    } else if (!rankwidth_join_forecast_wins) {
      skip_phase = "branch.rankwidth_skip_join_pair_forecast";
      veto = "rw_predicted_slower_join";
    }
    note_rankwidth_skip(stats, skip_phase,
                        rankwidth_join_forecast_wins ? cutrank_width
                                                     : rankwidth_join_pair_forecast);
    if (rw_data != NULL) {
      rw_data->veto_reason = veto;
    }
    if (!calibrating || cutrank_width > BRANCH_CALIBRATION_MAX_WIDTH) {
      qsop_rankwidth_decomposition_free(decomposition);
      return true;
    }
    /* Calibration: fall through to time rankwidth without adopting its counts. */
    calibration_timing_only = true;
  }

  uint64_t *part_counts = NULL;
  qsop_result_t *part_result = NULL;
  qsop_solve_stats_t delegated = {0};
  const uint64_t solve_start_ns = qsop_trace_now_ns();
  const uint64_t solve_start = qsop_trace_begin(stats->trace);
  const bool use_fourier = stats->mode == QSOP_SOLVE_MODE_FOURIER && stats->count_modulus == 0;
  if (calibration_timing_only && use_fourier) {
    /* Cannot time Fourier mode into a scratch buffer; skip calibration timing. */
    qsop_rankwidth_decomposition_free(decomposition);
    return true;
  }
  const bool ok =
      use_fourier
          ? qsop_solve_rankwidth_mode_trace_stats(sub, decomposition, sub->nvars,
                                                  QSOP_RANKWIDTH_SOLVE_FOURIER, &part_result,
                                                  &delegated, stats->trace, error)
          : (qsop_counts_alloc((uint32_t)sub->r, &part_counts, error) &&
             qsop_solve_rankwidth_count_table_mod_stats(sub, decomposition, stats->count_modulus,
                                                        part_counts, &delegated, stats->trace,
                                                        error));
  qsop_trace_emit_elapsed(stats->trace, "branch.rankwidth_delegate", stats->depth, sub->nvars,
                          solve_start);
  qsop_rankwidth_decomposition_free(decomposition);
  if (!ok) {
    free(part_counts);
    qsop_result_free(part_result);
    return false;
  }
  if (rw_data != NULL) {
    rw_data->actual_ms = branch_ns_to_ms(qsop_trace_elapsed_ns(solve_start_ns));
  }
  if (calibration_timing_only) {
    /* Timed rankwidth for calibration; discard results, let caller proceed with treewidth. */
    free(part_counts);
    qsop_result_free(part_result);
    return true;
  }

  const uint64_t *delegate_counts = use_fourier ? part_result->counts : part_counts;
  if (!branch_counts_shift_add((uint32_t)sub->r, counts, delegate_counts, constant_shift, stats,
                               error)) {
    free(part_counts);
    qsop_result_free(part_result);
    return false;
  }
  delegated.rankwidth_delegations = 1;
  merge_delegated_stats(stats, &delegated, sub->nvars);
  free(part_counts);
  qsop_result_free(part_result);
  *out_delegated = true;
  return true;
}

static bool branch_try_dp_delegate(qsop_residual_t *residual, uint64_t *counts,
                                   branch_search_stats_t *stats, bool *out_delegated,
                                   qsop_error_t *error) {
  *out_delegated = false;
  const uint32_t active_vars = qsop_residual_active_vars(residual);
  if (active_vars < BRANCH_TREEWIDTH_DELEGATE_MIN_VARS) {
    return true;
  }

  const bool recording = (stats->sink != NULL && stats->sink->file != NULL);
  const bool calibrate = recording && stats->sink->calibrate_backends;
  const uint32_t active_edges = qsop_residual_active_edges(residual);
  const uint32_t modulus_r = (uint32_t)qsop_residual_modulus(residual);

  qsop_instance_t sub = {0};
  qsop_stats_t sub_stats = {0};
  if (!build_active_residual_subinstance(residual, &sub, error)) {
    return false;
  }
  /* D2+D3: precompute the elimination order once; share between rankwidth and treewidth paths.
   * D3: check the adjacency-keyed order cache first — sibling residuals (x=0 vs x=1) have
   * identical adjacency and can reuse the same min-fill order without recomputing it.
   *
   * When min_fill_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH, treewidth will never be used,
   * so we capture the order from within the stats pass (no extra min-fill needed later).
   * When min_fill_width <= cap, we still want MIN_FILL_MAX_DEGREE order for treewidth quality,
   * so we use qsop_treewidth_order_alloc as before.
   * For nvars > 63, compute_width_diagnostics uses the large path and cannot capture the order. */
  const bool rw_uses_from_treewidth = (stats->rw_source == QSOP_BRANCH_RW_SOURCE_FROM_TREEWIDTH ||
                                       stats->rw_source == QSOP_BRANCH_RW_SOURCE_AUTO ||
                                       stats->rw_source == QSOP_BRANCH_RW_SOURCE_BOTH);
  /* Allocate a buffer for the stats order before calling compute_stats_with_order.
   * Only useful for wide instances (treewidth won't be tried) with nvars <= 63. */
  uint32_t *order_from_stats = NULL;
  const bool want_stats_order = rw_uses_from_treewidth && sub.nvars > 0 && sub.nvars <= 63U;
  if (want_stats_order) {
    order_from_stats = calloc(sub.nvars, sizeof(*order_from_stats));
    if (order_from_stats == NULL) {
      free_subinstance(&sub);
      set_error(error, "out of memory for stats order buffer");
      return false;
    }
  }
  const uint64_t probe_start_ns = recording ? qsop_trace_now_ns() : 0;
  const uint64_t stats_start = qsop_trace_begin(stats->trace);
  if (!qsop_compute_stats_with_order(&sub, &sub_stats, order_from_stats, error)) {
    free(order_from_stats);
    free_subinstance(&sub);
    return false;
  }
  note_width_probe(stats, &sub_stats);
  qsop_trace_emit_elapsed(stats->trace, "branch.width_probe", stats->depth,
                          sub_stats.min_fill_width, stats_start);
  const double probe_ms = recording ? branch_ns_to_ms(qsop_trace_elapsed_ns(probe_start_ns)) : 0.0;

  /* Decide whether to use the stats-captured order or the MIN_FILL_MAX_DEGREE order.
   * Use stats order only when treewidth won't be attempted (width > cap): this avoids
   * an extra min-fill inside make_from_treewidth_decomposition while preserving the
   * higher-quality MAX_DEGREE tiebreaker for narrow instances that DO use treewidth. */
  const bool use_stats_order_for_rankwidth =
      order_from_stats != NULL && sub_stats.width_diagnostics_available &&
      sub_stats.min_fill_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH;

  uint32_t *shared_order = NULL;
  uint32_t shared_order_width = 0;
  double shared_order_probe_ms = 0.0;
  uint64_t shared_order_adj_fp = 0;
  if (rw_uses_from_treewidth && sub_stats.width_diagnostics_available &&
      (use_stats_order_for_rankwidth ||
       sub_stats.min_fill_width <= BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH)) {
    const uint64_t order_start_ns_pre = recording ? qsop_trace_now_ns() : 0;
    const uint64_t order_start_pre = qsop_trace_begin(stats->trace);
    shared_order_adj_fp = branch_order_adj_fp(&sub);
    shared_order = branch_order_cache_lookup(&stats->order_cache, &sub, shared_order_adj_fp,
                                             &shared_order_width);
    if (shared_order != NULL) {
      free(order_from_stats);
      order_from_stats = NULL;
    } else if (use_stats_order_for_rankwidth) {
      /* Wide instance: use order captured during stats (same heuristic as make_from_treewidth). */
      shared_order = order_from_stats;
      order_from_stats = NULL;
      shared_order_width = sub_stats.min_fill_width;
      branch_order_cache_insert(&stats->order_cache, &sub, shared_order_adj_fp, shared_order,
                                shared_order_width);
    } else {
      /* Narrow instance: use MIN_FILL_MAX_DEGREE for best treewidth bag quality. */
      free(order_from_stats);
      order_from_stats = NULL;
      if (!qsop_treewidth_order_alloc(&sub, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &shared_order,
                                      &shared_order_width, error)) {
        free_subinstance(&sub);
        return false;
      }
      branch_order_cache_insert(&stats->order_cache, &sub, shared_order_adj_fp, shared_order,
                                shared_order_width);
    }
    qsop_trace_emit_elapsed(stats->trace, "branch.treewidth_order_probe", stats->depth,
                            shared_order_width, order_start_pre);
    shared_order_probe_ms =
        recording ? branch_ns_to_ms(qsop_trace_elapsed_ns(order_start_ns_pre)) : 0.0;
  } else {
    free(order_from_stats);
    order_from_stats = NULL;
  }

  bool delegated = false;
  branch_rw_decision_data_t rw_data = {0};
  branch_rw_decision_data_t *rw_ptr = recording ? &rw_data : NULL;
  if (!branch_try_rankwidth_delegate(
          &sub, counts, sub_stats.min_fill_width, sub_stats.prefix_cut_rank,
          sub_stats.width_diagnostics_available, (uint32_t)qsop_residual_constant(residual),
          shared_order, stats, rw_ptr, &delegated, error)) {
    free(shared_order);
    free_subinstance(&sub);
    return false;
  }
  if (delegated) {
    /* Calibration: also run treewidth for timing when rankwidth won. */
    double tw_cal_ms = 0.0;
    bool tw_cal_set = false;
    uint32_t tw_cal_width = shared_order_width;
    const bool rw_won_fourier = stats->mode == QSOP_SOLVE_MODE_FOURIER && stats->count_modulus == 0;
    if (!rw_won_fourier && calibrate && sub_stats.width_diagnostics_available &&
        sub_stats.min_fill_width <= BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
      /* Reuse shared_order if precomputed; otherwise compute fresh. */
      uint32_t *cal_order = shared_order;
      bool cal_order_owned = false;
      if (cal_order == NULL) {
        qsop_error_t cal_err = {0};
        if (!qsop_treewidth_order_alloc(&sub, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &cal_order,
                                        &tw_cal_width, &cal_err)) {
          cal_order = NULL;
        } else {
          cal_order_owned = true;
        }
      }
      if (cal_order != NULL && tw_cal_width <= BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
        uint64_t *cal_counts = NULL;
        qsop_solve_stats_t cal_stats = {0};
        qsop_error_t cal_err = {0};
        const uint64_t t0 = qsop_trace_now_ns();
        if (qsop_counts_alloc((uint32_t)sub.r, &cal_counts, &cal_err) &&
            qsop_solve_treewidth_precomputed_order_count_mod_stats(
                &sub, BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS, cal_order, tw_cal_width,
                stats->count_modulus, cal_counts, &cal_stats, NULL, &cal_err)) {
          tw_cal_ms = branch_ns_to_ms(qsop_trace_elapsed_ns(t0));
          tw_cal_set = true;
        }
        free(cal_counts);
      } else {
        tw_cal_width = 0;
      }
      if (cal_order_owned)
        free(cal_order);
    }
    if (recording) {
      const uint64_t tw_fe =
          tw_cal_set ? treewidth_table_forecast(tw_cal_width, (uint32_t)sub.r) : 0;
      const uint64_t tw_jp = tw_cal_set ? treewidth_join_pair_forecast(tw_cal_width, sub.nvars) : 0;
      branch_emit_jsonl_record(stats->sink, active_vars, active_edges, modulus_r, "rankwidth", NULL,
                               probe_ms, sub_stats.width_diagnostics_available,
                               sub_stats.min_fill_width, tw_cal_set, tw_fe, tw_jp, tw_cal_set,
                               tw_cal_ms, &rw_data);
    }
    free(shared_order);
    free_subinstance(&sub);
    *out_delegated = true;
    return true;
  }

  if (!sub_stats.width_diagnostics_available ||
      sub_stats.min_fill_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    const bool unavail = !sub_stats.width_diagnostics_available;
    const char *tw_veto = unavail ? "tw_width_unavailable" : "tw_width_above_cap";
    note_treewidth_skip(
        stats, unavail ? "branch.treewidth_skip_unavailable" : "branch.treewidth_skip_width",
        unavail ? 0 : sub_stats.min_fill_width);
    if (recording) {
      branch_emit_jsonl_record(stats->sink, active_vars, active_edges, modulus_r, "branch", tw_veto,
                               probe_ms, sub_stats.width_diagnostics_available,
                               sub_stats.min_fill_width, false, 0, 0, false, 0.0, &rw_data);
    }
    free(shared_order);
    free_subinstance(&sub);
    return true;
  }

  /* Obtain the treewidth elimination order — reuse shared_order if precomputed (D2),
   * else try the adjacency order cache (D3), else compute fresh. */
  uint32_t *order = shared_order;
  uint32_t order_width = shared_order_width;
  double order_probe_ms = shared_order_probe_ms;
  if (order == NULL) {
    const uint64_t order_start_ns = recording ? qsop_trace_now_ns() : 0;
    const uint64_t order_start = qsop_trace_begin(stats->trace);
    const uint64_t tw_adj_fp = branch_order_adj_fp(&sub);
    order = branch_order_cache_lookup(&stats->order_cache, &sub, tw_adj_fp, &order_width);
    if (order == NULL) {
      if (!qsop_treewidth_order_alloc(&sub, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &order,
                                      &order_width, error)) {
        free_subinstance(&sub);
        return false;
      }
      branch_order_cache_insert(&stats->order_cache, &sub, tw_adj_fp, order, order_width);
    }
    qsop_trace_emit_elapsed(stats->trace, "branch.treewidth_order_probe", stats->depth, order_width,
                            order_start);
    order_probe_ms = recording ? branch_ns_to_ms(qsop_trace_elapsed_ns(order_start_ns)) : 0.0;
  }
  if (order_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    note_treewidth_skip(stats, "branch.treewidth_skip_order_width", order_width);
    if (recording) {
      branch_emit_jsonl_record(stats->sink, active_vars, active_edges, modulus_r, "branch",
                               "tw_order_width_above_cap", probe_ms + order_probe_ms, true,
                               order_width, false, 0, 0, false, 0.0, &rw_data);
    }
    free(order);
    free_subinstance(&sub);
    return true;
  }

  const uint64_t tw_forecast_entries = treewidth_table_forecast(order_width, (uint32_t)sub.r);
  const uint64_t tw_forecast_join_pairs = treewidth_join_pair_forecast(order_width, sub.nvars);

  uint64_t *part_counts = NULL;
  qsop_result_t *part_result = NULL;
  qsop_solve_stats_t delegated_stats = {0};
  const uint64_t solve_start_ns = recording ? qsop_trace_now_ns() : 0;
  const uint64_t solve_start = qsop_trace_begin(stats->trace);
  const bool use_fourier = stats->mode == QSOP_SOLVE_MODE_FOURIER && stats->count_modulus == 0;
  const bool ok =
      use_fourier
          ? qsop_solve_treewidth_precomputed_order_mode_trace_stats(
                &sub, BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS, order, order_width,
                QSOP_SOLVE_MODE_FOURIER, &part_result, &delegated_stats, stats->trace, error)
          : (qsop_counts_alloc((uint32_t)sub.r, &part_counts, error) &&
             qsop_solve_treewidth_precomputed_order_count_mod_stats(
                 &sub, BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS, order, order_width,
                 stats->count_modulus, part_counts, &delegated_stats, stats->trace, error));
  qsop_trace_emit_elapsed(stats->trace, "branch.treewidth_delegate", stats->depth, sub.nvars,
                          solve_start);
  const double tw_actual_ms =
      recording ? branch_ns_to_ms(qsop_trace_elapsed_ns(solve_start_ns)) : 0.0;
  free(order);
  if (!ok) {
    free_subinstance(&sub);
    free(part_counts);
    qsop_result_free(part_result);
    return false;
  }

  /* Calibration: also run rankwidth for timing when treewidth won. */
  if (!use_fourier && calibrate) {
    qsop_rankwidth_decomposition_t *cal_decomp = NULL;
    qsop_error_t cal_err = {0};
    const uint64_t rw_gen_start = qsop_trace_now_ns();
    if (qsop_rankwidth_decomposition_generate(&sub, QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
                                              &cal_decomp, &cal_err)) {
      uint32_t cal_width = 0;
      if (qsop_rankwidth_decomposition_width(&sub, cal_decomp, &cal_width, &cal_err)) {
        rw_data.attempted = true;
        rw_data.generation_ms = branch_ns_to_ms(qsop_trace_elapsed_ns(rw_gen_start));
        rw_data.cutrank_width = cal_width;
        if (cal_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
          uint64_t cal_fe = saturating_mul_u64(binary_assignment_forecast(cal_width), sub.r);
          uint64_t cal_jp = 0;
          qsop_error_t fore_err = {0};
          qsop_rankwidth_decomposition_forecast(&sub, cal_decomp, &cal_fe, &cal_jp, &fore_err);
          rw_data.forecast_entries = cal_fe;
          rw_data.forecast_join_pairs = cal_jp;
          uint64_t *cal_counts = NULL;
          qsop_solve_stats_t cal_stats = {0};
          const uint64_t t0 = qsop_trace_now_ns();
          if (qsop_counts_alloc((uint32_t)sub.r, &cal_counts, &cal_err) &&
              qsop_solve_rankwidth_count_table_mod_stats(&sub, cal_decomp, stats->count_modulus,
                                                         cal_counts, &cal_stats, NULL, &cal_err)) {
            rw_data.actual_ms = branch_ns_to_ms(qsop_trace_elapsed_ns(t0));
          }
          free(cal_counts);
        }
      }
      qsop_rankwidth_decomposition_free(cal_decomp);
    }
  }

  if (recording) {
    branch_emit_jsonl_record(stats->sink, active_vars, active_edges, modulus_r, "treewidth", NULL,
                             probe_ms + order_probe_ms, true, order_width, true,
                             tw_forecast_entries, tw_forecast_join_pairs, true, tw_actual_ms,
                             &rw_data);
  }

  const uint64_t *delegate_counts = use_fourier ? part_result->counts : part_counts;
  if (!branch_counts_shift_add((uint32_t)sub.r, counts, delegate_counts,
                               (uint32_t)qsop_residual_constant(residual), stats, error)) {
    free_subinstance(&sub);
    free(part_counts);
    qsop_result_free(part_result);
    return false;
  }
  delegated_stats.treewidth_delegations = 1;
  merge_delegated_stats(stats, &delegated_stats, sub.nvars);
  free_subinstance(&sub);
  free(part_counts);
  qsop_result_free(part_result);
  *out_delegated = true;
  return true;
}

static bool branch_sum_rec(qsop_residual_t *residual, uint64_t *counts,
                           branch_search_stats_t *stats, qsop_error_t *error);

static void branch_search_free(branch_search_stats_t *search) {
  if (search == NULL) {
    return;
  }
  branch_order_cache_free_entries(&search->order_cache);
  residual_cache_free(&search->cache);
  free(search->work);
  free(search->tmp);
  search->work = NULL;
  search->tmp = NULL;
}

static bool branch_solve_counts_once(const qsop_instance_t *qsop, uint64_t count_modulus,
                                     uint32_t max_vars, qsop_branch_heuristic_t heuristic,
                                     qsop_branch_rw_source_t rw_source,
                                     const qsop_branch_policy_t *policy, qsop_solve_mode_t mode,
                                     uint64_t *counts, qsop_solve_stats_t *stats,
                                     qsop_solve_trace_t *trace, qsop_backend_stats_sink_t *sink,
                                     qsop_error_t *error) {
  qsop_residual_t *residual = NULL;
  branch_search_stats_t search = {
      .trace = trace,
      .sink = sink,
      .heuristic = heuristic,
      .rw_source = rw_source,
      .policy = branch_policy_normalize(policy),
      .mode = mode,
      .count_modulus = count_modulus,
      .max_vars = max_vars,
  };

  if (!qsop_residual_create(qsop, &residual, error) ||
      !qsop_counts_alloc((uint32_t)qsop->r, &search.work, error) ||
      !qsop_counts_alloc((uint32_t)qsop->r, &search.tmp, error)) {
    qsop_residual_free(residual);
    branch_search_free(&search);
    return false;
  }

  if (!branch_sum_rec(residual, counts, &search, error)) {
    qsop_residual_free(residual);
    branch_search_free(&search);
    return false;
  }

  if (stats != NULL) {
    stats->search_nodes = search.nodes;
    stats->leaf_assignments = search.leaves;
    stats->cache_hits = search.cache_hits;
    stats->cache_misses = search.cache_misses;
    stats->cache_avoided_nodes = search.cache_avoided_nodes;
    stats->cache_canonical_hits = search.cache_canonical_hits;
    stats->cache_canonical_lookups = search.cache_canonical_lookups;
    stats->cache_canonical_stores = search.cache_canonical_stores;
    stats->cache_entries = (uint64_t)search.cache.len;
    stats->cache_canonical_entries = residual_cache_canonical_entries(&search.cache);
    stats->cache_stored_residue_slots = saturating_mul_u64((uint64_t)search.cache.len, qsop->r);
    stats->cache_key_bytes = residual_cache_key_bytes(&search.cache);
    stats->cache_count_bytes = residual_cache_count_bytes(&search.cache);
    stats->cache_estimated_bytes = residual_cache_estimated_bytes(&search.cache);
    stats->table_entries = search.table_entries;
    stats->max_table_entries = search.max_table_entries;
    stats->signature_entries = search.signature_entries;
    stats->max_signature_entries = search.max_signature_entries;
    stats->join_pairs = search.join_pairs;
    stats->join_signature_pairs = search.join_signature_pairs;
    stats->rankwidth_table_forecast = search.rankwidth_table_forecast;
    stats->rankwidth_join_pair_forecast = search.rankwidth_join_pair_forecast;
    stats->treewidth_delegations = search.treewidth_delegations;
    stats->rankwidth_delegations = search.rankwidth_delegations;
    stats->branch_fallthroughs = search.branch_fallthroughs;
    stats->branch_treewidth_skips = search.branch_treewidth_skips;
    stats->branch_rankwidth_skips = search.branch_rankwidth_skips;
    stats->max_residual_vars = search.max_residual_vars;
    stats->max_residual_edges = search.max_residual_edges;
    stats->max_residual_components = search.max_residual_components;
    stats->max_residual_largest_component = search.max_residual_largest_component;
    stats->max_residual_min_fill_width = search.max_residual_min_fill_width;
    stats->max_residual_prefix_cut_rank = search.max_residual_prefix_cut_rank;
    stats->decomposition_width = search.decomposition_width;
    stats->rankwidth_cutrank_width = search.rankwidth_cutrank_width;
  }

  qsop_residual_free(residual);
  branch_search_free(&search);
  return true;
}

static bool branch_solve_component_counts_shared(const qsop_instance_t *sub, uint64_t *counts,
                                                 branch_search_stats_t *stats,
                                                 qsop_error_t *error) {
  qsop_residual_t *residual = NULL;
  if (!qsop_residual_create(sub, &residual, error)) {
    return false;
  }

  stats->depth++;
  const bool ok = branch_sum_rec(residual, counts, stats, error);
  stats->depth--;
  qsop_residual_free(residual);
  return ok;
}

static bool branch_sum_components(qsop_residual_t *residual, uint64_t *counts,
                                  branch_search_stats_t *stats, bool *out_split,
                                  qsop_error_t *error) {
  *out_split = false;
  const uint32_t nvars = qsop_residual_nvars(residual);
  const uint32_t r = (uint32_t)qsop_residual_modulus(residual);
  const bool use_fourier = stats->mode == QSOP_SOLVE_MODE_FOURIER && stats->count_modulus == 0;
  uint32_t *component = malloc((nvars == 0 ? 1U : nvars) * sizeof(*component));
  uint64_t *acc = NULL;
  uint64_t *tmp = NULL;
  uint64_t prime = 0;
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;
  if (component == NULL || !qsop_counts_alloc(r, &acc, error) ||
      !qsop_counts_alloc(r, &tmp, error)) {
    free(component);
    free(acc);
    free(tmp);
    set_error(error, "out of memory while splitting residual components");
    return false;
  }

  uint32_t ncomponents = 0;
  const uint64_t split_start = qsop_trace_begin(stats->trace);
  if (!qsop_residual_active_components(residual, component, &ncomponents, error)) {
    free(component);
    free(acc);
    free(tmp);
    return false;
  }
  if (!note_component_shape(stats, residual, component, ncomponents, error)) {
    free(component);
    free(acc);
    free(tmp);
    return false;
  }
  qsop_trace_emit_elapsed(stats->trace, "branch.component_split", stats->depth, ncomponents,
                          split_start);
  if (ncomponents <= 1U) {
    free(component);
    free(acc);
    free(tmp);
    return true;
  }

  if (use_fourier) {
    if (!qsop_fourier_find_ntt_prime(r, qsop_residual_active_vars(residual), &prime, error) ||
        !qsop_fourier_find_order_root(prime, r, &root, error)) {
      free(component);
      free(acc);
      free(tmp);
      return false;
    }
    inv_root = qsop_mod_pow_u64(root, prime - 2U, prime);
    if (!qsop_fourier_make_root_powers(r, root, prime, &powers, error) ||
        !qsop_fourier_make_root_powers(r, inv_root, prime, &inv_powers, error)) {
      free(component);
      free(acc);
      free(tmp);
      free(powers);
      free(inv_powers);
      return false;
    }
    for (uint32_t mode = 0; mode < r; mode++) {
      acc[mode] = 1;
    }
  } else {
    acc[0] = 1;
  }
  for (uint32_t c = 0; c < ncomponents; c++) {
    qsop_instance_t sub = {0};
    uint64_t *part_counts = NULL;
    if (!qsop_counts_alloc(r, &part_counts, error) ||
        !build_residual_subinstance(residual, component, c, &sub, error) ||
        !branch_solve_component_counts_shared(&sub, part_counts, stats, error)) {
      free_subinstance(&sub);
      free(part_counts);
      free(component);
      free(acc);
      free(tmp);
      free(powers);
      free(inv_powers);
      return false;
    }

    const uint64_t combine_start = qsop_trace_begin(stats->trace);
    const bool combined =
        use_fourier ? (branch_counts_to_fourier(r, part_counts, powers, prime, tmp, error) &&
                       branch_fourier_multiply(r, acc, tmp, prime, error))
                    : branch_counts_convolve(r, tmp, acc, part_counts, stats, error);
    if (!combined) {
      free_subinstance(&sub);
      free(part_counts);
      free(component);
      free(acc);
      free(tmp);
      free(powers);
      free(inv_powers);
      return false;
    }
    qsop_trace_emit_elapsed(stats->trace,
                            use_fourier ? "branch.fourier_multiply" : "branch.convolution",
                            stats->depth, r, combine_start);

    if (!use_fourier) {
      memcpy(acc, tmp, (size_t)r * sizeof(*acc));
    }
    free_subinstance(&sub);
    free(part_counts);
  }

  const bool finalized =
      use_fourier ? qsop_fourier_inverse_counts(r, acc, (uint32_t)qsop_residual_constant(residual),
                                                powers, inv_powers, prime, counts, error)
                  : branch_counts_shift_add(
                        r, counts, acc, (uint32_t)qsop_residual_constant(residual), stats, error);
  if (!finalized) {
    free(component);
    free(acc);
    free(tmp);
    free(powers);
    free(inv_powers);
    return false;
  }
  free(component);
  free(acc);
  free(tmp);
  free(powers);
  free(inv_powers);
  *out_split = true;
  return true;
}

typedef struct branch_candidate {
  uint32_t var;
  uint32_t components;
  uint32_t largest_component;
  uint32_t degree;
  uint64_t fill_edges;
  uint32_t child_width;
  uint32_t cut_rank;
  bool has_unary;
} branch_candidate_t;

static bool split_candidate_better(const branch_candidate_t *candidate,
                                   const branch_candidate_t *best) {
  /* One-variable balance gains can add component subsolves without reducing current corpus search.
   */
  const uint32_t min_balance_gain = 2;
  const bool materially_better_balance =
      best->largest_component >= min_balance_gain &&
      candidate->largest_component <= best->largest_component - min_balance_gain;
  return candidate->components > best->components ||
         (candidate->components == best->components && materially_better_balance) ||
         (candidate->components == best->components &&
          candidate->largest_component == best->largest_component &&
          candidate->degree > best->degree) ||
         (candidate->components == best->components &&
          candidate->largest_component == best->largest_component &&
          candidate->degree == best->degree && candidate->has_unary && !best->has_unary);
}

static bool branch_candidate_better(qsop_branch_heuristic_t heuristic,
                                    const branch_candidate_t *candidate,
                                    const branch_candidate_t *best) {
  switch (heuristic) {
  case QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH:
    return candidate->child_width < best->child_width ||
           (candidate->child_width == best->child_width && split_candidate_better(candidate, best));
  case QSOP_BRANCH_HEURISTIC_SPLIT:
    return split_candidate_better(candidate, best);
  case QSOP_BRANCH_HEURISTIC_TREEWIDTH:
    return candidate->fill_edges < best->fill_edges ||
           (candidate->fill_edges == best->fill_edges && split_candidate_better(candidate, best));
  case QSOP_BRANCH_HEURISTIC_CUTRANK_PROXY:
    return candidate->cut_rank < best->cut_rank ||
           (candidate->cut_rank == best->cut_rank && split_candidate_better(candidate, best));
  }
  return split_candidate_better(candidate, best);
}

static void delegation_top_insert(branch_candidate_t *top, uint32_t *ntop,
                                  const branch_candidate_t *candidate) {
  uint32_t pos = *ntop;
  if (pos < BRANCH_DELEGATION_DEPTH_TOP_K) {
    top[pos] = *candidate;
    (*ntop)++;
  } else if (candidate->fill_edges > top[pos - 1U].fill_edges ||
             (candidate->fill_edges == top[pos - 1U].fill_edges &&
              !split_candidate_better(candidate, &top[pos - 1U]))) {
    return;
  } else {
    top[pos - 1U] = *candidate;
  }

  pos = *ntop;
  while (pos > 1U) {
    branch_candidate_t *right = &top[pos - 1U];
    branch_candidate_t *left = &top[pos - 2U];
    const bool right_better =
        right->fill_edges < left->fill_edges ||
        (right->fill_edges == left->fill_edges && split_candidate_better(right, left));
    if (!right_better) {
      break;
    }
    const branch_candidate_t tmp = *left;
    *left = *right;
    *right = tmp;
    pos--;
  }
}

static bool choose_branch_var(const qsop_residual_t *residual, qsop_branch_heuristic_t heuristic,
                              uint32_t *out, qsop_error_t *error) {
  const uint32_t nvars = qsop_residual_nvars(residual);
  bool found = false;
  branch_candidate_t best = {0};
  branch_candidate_t top[BRANCH_DELEGATION_DEPTH_TOP_K] = {0};
  uint32_t ntop = 0;
  const bool delegation_depth =
      heuristic == QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH &&
      qsop_residual_active_vars(residual) <= BRANCH_ROOT_TREEWIDTH_WIDE_MAX_VARS;
  const bool need_fill_proxy = heuristic == QSOP_BRANCH_HEURISTIC_TREEWIDTH ||
                               heuristic == QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH;
  const qsop_branch_heuristic_t comparison_heuristic =
      heuristic == QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH && !delegation_depth
          ? QSOP_BRANCH_HEURISTIC_TREEWIDTH
          : heuristic;

  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_residual_var_active(residual, v)) {
      const uint32_t degree = qsop_residual_active_degree(residual, v);
      if (degree == 0) {
        continue;
      }
      uint32_t components = 0;
      uint32_t largest_component = 0;
      if (!qsop_residual_split_without_var(residual, v, &components, &largest_component, error)) {
        return false;
      }
      branch_candidate_t candidate = {
          .var = v,
          .components = components,
          .largest_component = largest_component,
          .degree = degree,
          .has_unary = qsop_residual_unary(residual, v) != 0,
      };
      if (need_fill_proxy &&
          !qsop_residual_fill_edges_without_var(residual, v, &candidate.fill_edges, error)) {
        return false;
      }
      if (heuristic == QSOP_BRANCH_HEURISTIC_CUTRANK_PROXY &&
          !qsop_residual_neighbor_cut_rank(residual, v, &candidate.cut_rank, error)) {
        return false;
      }
      if (delegation_depth) {
        delegation_top_insert(top, &ntop, &candidate);
      } else if (!found || branch_candidate_better(comparison_heuristic, &candidate, &best)) {
        found = true;
        best = candidate;
      }
    }
  }

  if (delegation_depth) {
    for (uint32_t i = 0; i < ntop; i++) {
      if (!qsop_residual_min_fill_width_without_var(residual, top[i].var, &top[i].child_width,
                                                    error)) {
        return false;
      }
      if (!found || branch_candidate_better(heuristic, &top[i], &best)) {
        found = true;
        best = top[i];
      }
    }
  }

  if (!found) {
    set_error(error, "residual active-var count disagrees with active flags");
    return false;
  }

  *out = best.var;
  return true;
}

static bool edge_free_sum(const qsop_residual_t *residual, uint64_t *counts,
                          branch_search_stats_t *stats, qsop_error_t *error) {
  const uint64_t edge_free_start = qsop_trace_begin(stats->trace);
  const uint32_t r = (uint32_t)qsop_residual_modulus(residual);
  uint64_t *current = stats->work;
  uint64_t *next = stats->tmp;
  qsop_counts_clear(r, current);
  current[(uint32_t)qsop_residual_constant(residual)] = 1;

  for (uint32_t v = 0; v < qsop_residual_nvars(residual); v++) {
    if (!qsop_residual_var_active(residual, v)) {
      continue;
    }

    qsop_counts_clear(r, next);
    const uint32_t unary = (uint32_t)qsop_residual_unary(residual, v);
    for (uint32_t residue = 0; residue < r; residue++) {
      const uint64_t count = current[residue];
      if (count == 0) {
        continue;
      }
      if (!branch_count_add(stats, &next[residue], count, error) ||
          !branch_count_add(stats, &next[((uint64_t)residue + unary) % r], count, error)) {
        return false;
      }
    }

    uint64_t *swap = current;
    current = next;
    next = swap;
  }

  for (uint32_t residue = 0; residue < r; residue++) {
    if (!branch_count_add(stats, &counts[residue], current[residue], error)) {
      return false;
    }
  }
  const uint64_t leaves = assignment_count(qsop_residual_active_vars(residual));
  add_saturating_u64(&stats->leaves, leaves);
  qsop_trace_emit_elapsed(stats->trace, "branch.edge_free_leaf", stats->depth, leaves,
                          edge_free_start);
  return true;
}

static bool branch_sum_uncached(qsop_residual_t *residual, uint64_t *counts,
                                branch_search_stats_t *stats, qsop_error_t *error) {
  if (qsop_residual_active_vars(residual) == 0) {
    stats->leaves++;
    return branch_count_add(stats, &counts[(uint32_t)qsop_residual_constant(residual)], 1, error);
  }
  if (qsop_residual_active_edges(residual) == 0) {
    return edge_free_sum(residual, counts, stats, error);
  }

  bool did_split = false;
  if (!branch_sum_components(residual, counts, stats, &did_split, error)) {
    return false;
  }
  if (did_split) {
    return true;
  }

  bool delegated = false;
  if (!branch_try_dp_delegate(residual, counts, stats, &delegated, error)) {
    return false;
  }
  if (delegated) {
    return true;
  }

  /* Unlike single-fourier's branching fallback (gated separately by max_fallback_vars), this
   * recursion has no smaller cap of its own -- qsop_solve_branch's root nvars check is
   * deliberately loose now (BRANCH_ROOT_SANITY_MULTIPLIER), so a component that didn't split
   * small enough and isn't delegate-eligible must be refused here instead of falling into
   * unbounded exhaustive branching. */
  const uint32_t active_vars = qsop_residual_active_vars(residual);
  if (active_vars > stats->max_vars) {
    set_error(error,
              "residual branch solver refuses a %" PRIu32
              "-variable component; pass a larger --max-vars or use a future backend",
              active_vars);
    return false;
  }

  stats->branch_fallthroughs++;
  branch_trace_event(stats, "branch.fallthrough", qsop_residual_active_vars(residual));

  uint32_t v = 0;
  const uint64_t select_start = qsop_trace_begin(stats->trace);
  if (!choose_branch_var(residual, stats->heuristic, &v, error)) {
    return false;
  }
  qsop_trace_emit_elapsed(stats->trace, "branch.select_variable", stats->depth, v, select_start);

  for (uint8_t value = 0; value <= 1U; value++) {
    const size_t checkpoint = qsop_residual_checkpoint(residual);
    if (!qsop_residual_branch(residual, v, value, error)) {
      return false;
    }
    stats->depth++;
    const bool ok = branch_sum_rec(residual, counts, stats, error);
    stats->depth--;
    if (!ok) {
      return false;
    }
    if (!qsop_residual_undo(residual, checkpoint, error)) {
      return false;
    }
  }

  return true;
}

static bool branch_sum_rec(qsop_residual_t *residual, uint64_t *counts,
                           branch_search_stats_t *stats, qsop_error_t *error) {
  stats->nodes++;

  const uint64_t lookup_start = qsop_trace_begin(stats->trace);
  const residual_cache_entry_t *entry = NULL;
  bool canonical_lookup = false;
  if (!residual_cache_find(&stats->cache, residual, &entry, &canonical_lookup, error)) {
    return false;
  }
  if (canonical_lookup) {
    stats->cache_canonical_lookups++;
  }
  qsop_trace_emit_elapsed(
      stats->trace, canonical_lookup ? "branch.cache_canonical_lookup" : "branch.cache_lookup",
      stats->depth, stats->cache.len, lookup_start);
  if (entry != NULL) {
    stats->cache_hits++;
    if (entry->key.canonical) {
      stats->cache_canonical_hits++;
    }
    if (entry->search_nodes > 1U) {
      add_saturating_u64(&stats->cache_avoided_nodes, entry->search_nodes - 1U);
    }
    return add_counts((uint32_t)qsop_residual_modulus(residual), counts, entry->counts, stats,
                      error);
  }

  stats->cache_misses++;
  const uint64_t subtree_start_nodes = stats->nodes;
  uint64_t *computed = NULL;
  const uint32_t r = (uint32_t)qsop_residual_modulus(residual);
  if (!qsop_counts_alloc(r, &computed, error)) {
    return false;
  }
  if (!branch_sum_uncached(residual, computed, stats, error)) {
    free(computed);
    return false;
  }
  const uint64_t subtree_search_nodes = stats->nodes - subtree_start_nodes + 1U;
  const uint64_t store_start = qsop_trace_begin(stats->trace);
  bool canonical_store = false;
  if (!residual_cache_store(&stats->cache, residual, computed, subtree_search_nodes,
                            &canonical_store, error)) {
    free(computed);
    return false;
  }
  if (canonical_store) {
    stats->cache_canonical_stores++;
  }
  qsop_trace_emit_elapsed(stats->trace,
                          canonical_store ? "branch.cache_canonical_store" : "branch.cache_store",
                          stats->depth, stats->cache.len, store_start);

  if (!add_counts(r, counts, computed, stats, error)) {
    free(computed);
    return false;
  }
  free(computed);
  return true;
}

static bool solve_branch_crt(const qsop_instance_t *qsop, uint32_t max_vars,
                             qsop_branch_heuristic_t heuristic, qsop_branch_rw_source_t rw_source,
                             qsop_result_t **out, qsop_solve_stats_t *stats,
                             qsop_solve_trace_t *trace, qsop_backend_stats_sink_t *sink,
                             qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "branch CRT count table is too large");
    return false;
  }

  uint64_t *all_counts = calloc(nprimes * (size_t)qsop->r, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (all_counts == NULL || residues == NULL || result == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating branch CRT solve state");
    return false;
  }
  result->r = (uint32_t)qsop->r;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(qsop->r, sizeof(*result->count_strings));
  if (result->count_strings == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating branch CRT result strings");
    return false;
  }

  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    qsop_backend_stats_sink_t *sink_for_prime = p == 0 ? sink : NULL;
    if (!branch_solve_counts_once(qsop, primes[p], max_vars, heuristic, rw_source, NULL,
                                  QSOP_SOLVE_MODE_COUNT_TABLE, &all_counts[p * (size_t)qsop->r],
                                  stats_for_prime, trace_for_prime, sink_for_prime, error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t residue = 0; residue < qsop->r; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)qsop->r + residue];
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

static uint32_t root_find(uint32_t *parent, uint32_t value) {
  uint32_t root = value;
  while (parent[root] != root) {
    root = parent[root];
  }
  while (parent[value] != value) {
    const uint32_t next = parent[value];
    parent[value] = root;
    value = next;
  }
  return root;
}

static void root_union(uint32_t *parent, uint32_t left, uint32_t right) {
  const uint32_t left_root = root_find(parent, left);
  const uint32_t right_root = root_find(parent, right);
  if (left_root != right_root) {
    parent[right_root] = left_root;
  }
}

static bool support_component_count(const qsop_instance_t *qsop, uint32_t *out,
                                    qsop_error_t *error) {
  uint32_t *parent = malloc((qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*parent));
  if (parent == NULL) {
    set_error(error, "out of memory while counting support components");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    parent[v] = v;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    root_union(parent, qsop->edge_u[e], qsop->edge_v[e]);
  }

  uint32_t components = 0;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (root_find(parent, v) == v) {
      components++;
    }
  }
  free(parent);
  *out = components;
  return true;
}

static bool branch_try_root_treewidth_fast_path(const qsop_instance_t *qsop, qsop_result_t **out,
                                                qsop_solve_stats_t *stats,
                                                qsop_solve_trace_t *trace, qsop_solve_mode_t mode,
                                                qsop_backend_stats_sink_t *sink, bool *out_handled,
                                                qsop_error_t *error) {
  *out_handled = false;
  if (qsop->nvars < BRANCH_TREEWIDTH_DELEGATE_MIN_VARS || qsop->nedges == 0) {
    return true;
  }

  uint32_t components = 0;
  if (!support_component_count(qsop, &components, error)) {
    return false;
  }
  if (components != 1U) {
    return true;
  }

  uint32_t *order = NULL;
  uint32_t width = 0;
  const bool recording = (sink != NULL && sink->file != NULL);
  const uint64_t probe_start_ns = recording ? qsop_trace_now_ns() : 0;
  const uint64_t stats_start = qsop_trace_begin(trace);
  if (!qsop_treewidth_order_alloc(qsop, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &order, &width,
                                  error)) {
    return false;
  }
  qsop_trace_emit_elapsed(trace, "branch.root_width_probe", 0, width, stats_start);
  const double probe_ms = recording ? branch_ns_to_ms(qsop_trace_elapsed_ns(probe_start_ns)) : 0.0;
  const uint32_t max_width = qsop->nvars <= BRANCH_ROOT_TREEWIDTH_WIDE_MAX_VARS
                                 ? BRANCH_ROOT_TREEWIDTH_WIDE_MAX_WIDTH
                                 : BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH;
  const uint32_t max_bag_vars = qsop->nvars <= BRANCH_ROOT_TREEWIDTH_WIDE_MAX_VARS
                                    ? BRANCH_ROOT_TREEWIDTH_WIDE_MAX_BAG_VARS
                                    : BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS;
  if (width > max_width) {
    free(order);
    return true;
  }
  /* D5: treewidth is within delegation range but prefix_cut_rank signals rankwidth
   * may win massively (e.g. K_{a,b} uniform with tw = a >> cut_rank = 1).  Check
   * after the width gate so roots outside the admitted cap skip this stats call entirely. */
  qsop_stats_t root_stats = {0};
  qsop_error_t stats_err = {0};
  if (qsop_compute_stats(qsop, &root_stats, &stats_err) && root_stats.width_diagnostics_available &&
      root_stats.prefix_cut_rank <= BRANCH_RANKWIDTH_LOW_RANK_BYPASS &&
      root_stats.min_fill_width > BRANCH_FAST_PATH_RW_BYPASS_MIN_WIDTH) {
    free(order);
    return true; /* not handled: let main recursion try rankwidth */
  }
  qsop_trace_emit(trace, "branch.treewidth_table_forecast", 0,
                  treewidth_table_forecast(width, (uint32_t)qsop->r), 0);
  qsop_trace_emit(trace, "branch.treewidth_join_pair_forecast", 0,
                  treewidth_join_pair_forecast(width, qsop->nvars), 0);
  qsop_trace_emit(trace, "branch.rankwidth_skip_treewidth_preferred", 0, width, 0);

  qsop_result_t *result = NULL;
  qsop_solve_stats_t delegated = {0};
  const uint64_t solve_start_ns = recording ? qsop_trace_now_ns() : 0;
  const uint64_t solve_start = qsop_trace_begin(trace);
  if (!qsop_solve_treewidth_precomputed_order_mode_trace_stats(
          qsop, max_bag_vars, order, width, mode, &result, &delegated, trace, error)) {
    free(order);
    return false;
  }
  free(order);
  qsop_trace_emit_elapsed(trace, "branch.root_treewidth_delegate", 0, qsop->nvars, solve_start);
  const double actual_ms = recording ? branch_ns_to_ms(qsop_trace_elapsed_ns(solve_start_ns)) : 0.0;

  if (stats != NULL) {
    *stats = delegated;
    stats->search_nodes = 1;
    stats->cache_misses = 1;
    stats->leaf_assignments = assignment_count(qsop->nvars);
    stats->treewidth_delegations = 1;
    stats->rankwidth_delegations = 0;
    stats->branch_rankwidth_skips = 1;
    stats->max_residual_vars = qsop->nvars;
    stats->max_residual_edges = qsop->nedges;
    stats->max_residual_components = components;
    stats->max_residual_largest_component = qsop->nvars;
    stats->max_residual_min_fill_width = width;
  }

  /* Calibration: also time rankwidth on the root instance for comparison data. */
  branch_rw_decision_data_t rw_data = {0};
  if (recording && sink->calibrate_backends && mode != QSOP_SOLVE_MODE_FOURIER) {
    /* Get a CRT prime so count arithmetic does not overflow for large circuits. */
    uint64_t *cal_primes = NULL;
    size_t cal_nprimes = 0;
    qsop_error_t cal_err = {0};
    uint64_t cal_modulus = 0;
    if (qsop_crt_find_primes_for_nvars(qsop->nvars, &cal_primes, &cal_nprimes, &cal_err) &&
        cal_nprimes > 0) {
      cal_modulus = cal_primes[0];
    }
    free(cal_primes);
    if (cal_modulus != 0) {
      qsop_rankwidth_decomposition_t *cal_decomp = NULL;
      const uint64_t rw_gen_start = qsop_trace_now_ns();
      if (qsop_rankwidth_decomposition_generate(qsop, QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
                                                &cal_decomp, &cal_err)) {
        uint32_t cal_width = 0;
        if (qsop_rankwidth_decomposition_width(qsop, cal_decomp, &cal_width, &cal_err)) {
          rw_data.attempted = true;
          rw_data.generation_ms = branch_ns_to_ms(qsop_trace_elapsed_ns(rw_gen_start));
          rw_data.cutrank_width = cal_width;
          if (cal_width <= BRANCH_CALIBRATION_MAX_WIDTH) {
            uint64_t cal_fe = saturating_mul_u64(binary_assignment_forecast(cal_width), qsop->r);
            uint64_t cal_jp = 0;
            qsop_rankwidth_decomposition_forecast(qsop, cal_decomp, &cal_fe, &cal_jp, &cal_err);
            rw_data.forecast_entries = cal_fe;
            rw_data.forecast_join_pairs = cal_jp;
            uint64_t *cal_counts = NULL;
            qsop_solve_stats_t cal_stats = {0};
            const uint64_t t0 = qsop_trace_now_ns();
            if (qsop_counts_alloc((uint32_t)qsop->r, &cal_counts, &cal_err) &&
                qsop_solve_rankwidth_count_table_mod_stats(
                    qsop, cal_decomp, cal_modulus, cal_counts, &cal_stats, NULL, &cal_err)) {
              rw_data.actual_ms = branch_ns_to_ms(qsop_trace_elapsed_ns(t0));
            }
            free(cal_counts);
          }
        }
        qsop_rankwidth_decomposition_free(cal_decomp);
      }
    }
  }

  if (recording) {
    const uint64_t tw_fe = treewidth_table_forecast(width, (uint32_t)qsop->r);
    const uint64_t tw_jp = treewidth_join_pair_forecast(width, qsop->nvars);
    branch_emit_jsonl_record(sink, qsop->nvars, qsop->nedges, (uint32_t)qsop->r, "treewidth",
                             "rw_treewidth_preferred", probe_ms, true, width, true, tw_fe, tw_jp,
                             true, actual_ms, rw_data.attempted ? &rw_data : NULL);
  }

  *out = result;
  *out_handled = true;
  return true;
}

bool qsop_solve_branch(const qsop_instance_t *qsop, uint32_t max_vars,
                       const qsop_branch_solve_options_t *options, qsop_result_t **out,
                       qsop_solve_stats_t *stats, qsop_error_t *error) {
  const qsop_branch_solve_options_t o =
      options != NULL ? *options : (qsop_branch_solve_options_t){0};
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->r > UINT32_MAX) {
    set_error(error, "count-table/all-modes branch backend requires R <= UINT32_MAX; use "
                     "--solve-mode single-fourier");
    return false;
  }
  if (o.mode != QSOP_SOLVE_MODE_COUNT_TABLE && o.mode != QSOP_SOLVE_MODE_FOURIER) {
    set_error(error, "internal error: unsupported residual branch solve mode");
    return false;
  }
  bool root_handled = false;
  const bool tried_root_treewidth_before_sanity =
      max_vars > 0U && qsop->nvars <= BRANCH_ROOT_TREEWIDTH_WIDE_MAX_VARS;
  if (tried_root_treewidth_before_sanity) {
    if (!branch_try_root_treewidth_fast_path(qsop, out, stats, o.trace, o.mode, o.sink,
                                             &root_handled, error)) {
      return false;
    }
    if (root_handled) {
      return true;
    }
  }
  /* Deliberately loose (see BRANCH_ROOT_SANITY_MULTIPLIER's comment): rejecting on the whole
   * instance's raw nvars here, before ever attempting a component split, would wrongly refuse a
   * large instance that's actually a disjoint union of many small, easily delegatable
   * components. The real accept/reject decision for a component that doesn't split small enough
   * is the per-component max_vars check added to branch_sum_uncached's fallthrough-to-branching
   * step, since (unlike single-fourier) this recursion's branching fallback has no separate
   * max_fallback_vars-style cap of its own. */
  const uint64_t root_sanity_limit = (uint64_t)max_vars * BRANCH_ROOT_SANITY_MULTIPLIER;
  if ((uint64_t)qsop->nvars > root_sanity_limit) {
    set_error(error,
              "residual branch solver refuses %" PRIu32
              " variables outright (exceeds %ux --max-vars); pass a larger --max-vars or use a "
              "future backend",
              qsop->nvars, BRANCH_ROOT_SANITY_MULTIPLIER);
    return false;
  }
  if (!tried_root_treewidth_before_sanity) {
    if (!branch_try_root_treewidth_fast_path(qsop, out, stats, o.trace, o.mode, o.sink,
                                             &root_handled, error)) {
      return false;
    }
    if (root_handled) {
      return true;
    }
  }
  if (qsop->nvars >= 64U) {
    return solve_branch_crt(qsop, max_vars, o.heuristic, o.rw_source, out, stats, o.trace, o.sink,
                            error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    set_error(error, "out of memory while allocating result");
    return false;
  }
  result->r = (uint32_t)qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc((uint32_t)qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    return false;
  }

  if (!branch_solve_counts_once(qsop, 0, max_vars, o.heuristic, o.rw_source, &o.policy, o.mode,
                                result->counts, stats, o.trace, o.sink, error)) {
    qsop_result_free(result);
    return false;
  }

  *out = result;
  return true;
}

/* ---------------------------------------------------------------------------
 * Single Fourier mode: delegate-first residual branching.
 *
 * The single-mode branch backend uses widened qsop_residual_t state, so it can split active
 * residual components, delegate cheap components to treewidth/rankwidth, and fall back to
 * scalar residual branching without allocating O(R) residue tables. The explicit delegate-only
 * policy still uses this same recursion, but stops with a clear "no delegate available" error
 * before the residual branching step.
 * --------------------------------------------------------------------------- */

static void branch_root_of_unity(uint64_t r, uint32_t target_mode, uint64_t k, long double *re,
                                 long double *im) {
  /* Duplicated per this file's convention -- tw_root_of_unity/rw_root_of_unity are
   * independently defined in treewidth.c/rankwidth.c too, rather than shared. */
  static const long double branch_two_pi =
      6.283185307179586476925286766559005768394338798750211641949889L;
  const long double angle =
      branch_two_pi * (long double)target_mode * (long double)k / (long double)r;
  *re = cosl(angle);
  *im = sinl(angle);
}

/* re/im are long double, not double: components delegated to treewidth/rankwidth
 * come back as qsop_amplitude_t (long double), with raw (pre-normalization)
 * magnitudes on the order of 2**(norm_h/2) -- routinely far beyond a double's
 * ~1.8e308 range on deep circuits (norm_h > ~2048 is enough). Narrowing here
 * would silently overflow to inf/nan before any combination even happens; see
 * branch_try_single_mode_delegate below, the one place a delegated amplitude
 * enters this type. */
typedef struct branch_c64 {
  long double re;
  long double im;
} branch_c64_t;

static inline branch_c64_t c64_zero(void) {
  return (branch_c64_t){0.0L, 0.0L};
}

static inline branch_c64_t c64_one(void) {
  return (branch_c64_t){1.0L, 0.0L};
}

static inline branch_c64_t c64_add(branch_c64_t a, branch_c64_t b) {
  return (branch_c64_t){a.re + b.re, a.im + b.im};
}

static inline branch_c64_t c64_mul(branch_c64_t a, branch_c64_t b) {
  return (branch_c64_t){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

static inline void c64_accum_error(uint64_t ops, long double *err) {
  if (err != NULL) {
    *err += (long double)ops * 8.0L * LDBL_EPSILON;
  }
}

typedef struct branch_phase_cache {
  uint64_t r;
  uint32_t target_mode;
  uint64_t *keys;
  double *re;
  double *im;
  uint8_t *used;
  size_t cap;
  size_t len;
} branch_phase_cache_t;

typedef struct branch_amp_cache_entry {
  residual_cache_key_t key;
  branch_c64_t amp;
  long double numeric_error_bound;
  uint64_t search_nodes;
  size_t next;
} branch_amp_cache_entry_t;

typedef struct branch_amp_cache {
  branch_amp_cache_entry_t *entries;
  size_t *buckets;
  size_t len;
  size_t cap;
  size_t bucket_count;
  uint64_t estimated_bytes;
  uint64_t budget_bytes;
  uint32_t min_vars;
} branch_amp_cache_t;

typedef struct branch_single_mode_state {
  uint64_t r;
  uint32_t target_mode;
  uint32_t max_vars;
  qsop_branch_rw_source_t rw_source;
  qsop_branch_policy_t policy;
  qsop_branch_heuristic_t heuristic;
  qsop_branch_single_fallback_t fallback;
  qsop_branch_single_precision_t precision;
  qsop_branch_single_kernel_t kernel;
  const qsop_simd_vtable_t *simd;
  uint64_t max_search_nodes;
  uint32_t max_fallback_vars;
  uint32_t max_recursion_depth;
  /* Resolved once at init: qsop_residual_propagate is exact only for an even modulus and an odd
   * target mode (an even mode kills the sign edges outright and the rule changes shape). */
  bool propagate;

  branch_phase_cache_t phase_cache;
  branch_amp_cache_t amp_cache;

  uint64_t nodes;
  uint64_t leaves;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t cache_avoided_nodes;
  uint64_t cache_stores;
  uint64_t branch_fallthroughs;
  uint64_t propagations;
  uint64_t zero_prunes;
  uint32_t depth;
  long double numeric_error_bound;

  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
} branch_single_mode_state_t;

static uint64_t branch_hash_u64(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27U)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31U);
}

static bool branch_phase_cache_init(branch_phase_cache_t *cache, uint64_t r, uint32_t target_mode,
                                    uint32_t lg_cap, qsop_error_t *error) {
  *cache = (branch_phase_cache_t){.r = r, .target_mode = target_mode};
  if (lg_cap == 0) {
    lg_cap = BRANCH_SINGLE_DEFAULT_PHASE_CACHE_LG_CAP;
  }
  if (lg_cap > BRANCH_SINGLE_MAX_PHASE_CACHE_LG_CAP) {
    set_error(error, "branch single-Fourier phase cache is too large");
    return false;
  }
  cache->cap = (size_t)1U << lg_cap;
  cache->keys = calloc(cache->cap, sizeof(*cache->keys));
  cache->re = calloc(cache->cap, sizeof(*cache->re));
  cache->im = calloc(cache->cap, sizeof(*cache->im));
  cache->used = calloc(cache->cap, sizeof(*cache->used));
  if (cache->keys == NULL || cache->re == NULL || cache->im == NULL || cache->used == NULL) {
    free(cache->keys);
    free(cache->re);
    free(cache->im);
    free(cache->used);
    *cache = (branch_phase_cache_t){0};
    set_error(error, "out of memory while allocating branch single-Fourier phase cache");
    return false;
  }
  return true;
}

static void branch_phase_cache_free(branch_phase_cache_t *cache) {
  if (cache == NULL) {
    return;
  }
  free(cache->keys);
  free(cache->re);
  free(cache->im);
  free(cache->used);
  *cache = (branch_phase_cache_t){0};
}

static uint64_t mul_mod_u64_u32(uint64_t a, uint32_t b, uint64_t mod) {
  if (mod == 0) {
    return 0;
  }
#if defined(__SIZEOF_INT128__)
  return (uint64_t)(((__uint128_t)(a % mod) * (__uint128_t)b) % (__uint128_t)mod);
#else
  const long double product = fmodl((long double)(a % mod) * (long double)b, (long double)mod);
  return (uint64_t)product;
#endif
}

static branch_c64_t branch_quarter_phase(uint32_t target_mode, uint32_t multiplier) {
  switch ((target_mode * multiplier) & 3U) {
  case 0:
    return (branch_c64_t){1.0, 0.0};
  case 1:
    return (branch_c64_t){0.0, 1.0};
  case 2:
    return (branch_c64_t){-1.0, 0.0};
  default:
    return (branch_c64_t){0.0, -1.0};
  }
}

static branch_c64_t branch_phase_compute(uint64_t r, uint32_t target_mode, uint64_t residue) {
  residue %= r;
  if (residue == 0) {
    return c64_one();
  }
  if ((r & 1U) == 0 && residue == r / 2U) {
    return (target_mode & 1U) != 0 ? (branch_c64_t){-1.0, 0.0} : c64_one();
  }
  if ((r & 3U) == 0) {
    if (residue == r / 4U) {
      return branch_quarter_phase(target_mode, 1U);
    }
    if (residue == (3U * (r / 4U))) {
      return branch_quarter_phase(target_mode, 3U);
    }
  }

  static const double two_pi = 6.2831853071795864769252867665590057683943387987502;
  const uint64_t k = mul_mod_u64_u32(residue, target_mode, r);
  const double angle = two_pi * (double)k / (double)r;
  return (branch_c64_t){cos(angle), sin(angle)};
}

static branch_c64_t branch_phase_lookup(branch_phase_cache_t *cache, uint64_t residue) {
  if (cache == NULL || cache->r == 0) {
    return c64_one();
  }
  residue %= cache->r;
  if (residue == 0 || (((cache->r & 1U) == 0) && residue == cache->r / 2U) ||
      (((cache->r & 3U) == 0) && (residue == cache->r / 4U || residue == 3U * (cache->r / 4U)))) {
    return branch_phase_compute(cache->r, cache->target_mode, residue);
  }
  if (cache->cap == 0) {
    return branch_phase_compute(cache->r, cache->target_mode, residue);
  }
  size_t idx = (size_t)(branch_hash_u64(residue) & (uint64_t)(cache->cap - 1U));
  while (cache->used[idx] != 0) {
    if (cache->keys[idx] == residue) {
      return (branch_c64_t){cache->re[idx], cache->im[idx]};
    }
    idx = (idx + 1U) & (cache->cap - 1U);
  }
  const branch_c64_t value = branch_phase_compute(cache->r, cache->target_mode, residue);
  if (cache->len * 10U >= cache->cap * 7U) {
    return value;
  }
  cache->used[idx] = 1U;
  cache->keys[idx] = residue;
  /* The cache itself stores double-precision phases (magnitude always 1, so this
   * narrowing never risks overflow) -- only the amplitude-carrying branch_c64_t
   * values need long double range. */
  cache->re[idx] = (double)value.re;
  cache->im[idx] = (double)value.im;
  cache->len++;
  return value;
}

static uint64_t residual_key_estimated_bytes(const residual_cache_key_t *key) {
  uint64_t bytes = sizeof(*key);
  add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->active_var)));
  add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->active_edge)));
  add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->unary)));
  add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->edge_u)));
  add_saturating_u64(&bytes, saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->edge_v)));
  return bytes;
}

static void branch_amp_cache_free(branch_amp_cache_t *cache) {
  if (cache == NULL) {
    return;
  }
  for (size_t i = 0; i < cache->len; i++) {
    residual_cache_key_free(&cache->entries[i].key);
  }
  free(cache->entries);
  free(cache->buckets);
  *cache = (branch_amp_cache_t){0};
}

static bool branch_amp_cache_reserve(branch_amp_cache_t *cache, size_t needed,
                                     qsop_error_t *error) {
  if (needed <= cache->cap) {
    return true;
  }
  size_t new_cap = cache->cap == 0 ? 32U : cache->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "branch single-Fourier amplitude cache is too large");
      return false;
    }
    new_cap *= 2U;
  }
  branch_amp_cache_entry_t *entries = realloc(cache->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    set_error(error, "out of memory while growing branch single-Fourier amplitude cache");
    return false;
  }
  cache->entries = entries;
  cache->cap = new_cap;
  return true;
}

static bool branch_amp_cache_rehash(branch_amp_cache_t *cache, size_t bucket_count,
                                    qsop_error_t *error) {
  size_t *buckets = malloc(bucket_count * sizeof(*buckets));
  if (buckets == NULL) {
    set_error(error, "out of memory while allocating branch single-Fourier amplitude cache");
    return false;
  }
  for (size_t i = 0; i < bucket_count; i++) {
    buckets[i] = SIZE_MAX;
  }
  for (size_t i = 0; i < cache->len; i++) {
    const size_t bucket = (size_t)(cache->entries[i].key.fingerprint % bucket_count);
    cache->entries[i].next = buckets[bucket];
    buckets[bucket] = i;
  }
  free(cache->buckets);
  cache->buckets = buckets;
  cache->bucket_count = bucket_count;
  return true;
}

static bool branch_amp_cache_find(const branch_amp_cache_t *cache, const qsop_residual_t *residual,
                                  const branch_amp_cache_entry_t **out) {
  *out = NULL;
  if (cache->bucket_count == 0 || qsop_residual_active_vars(residual) < cache->min_vars) {
    return true;
  }
  const uint64_t fingerprint = qsop_residual_fingerprint(residual);
  const size_t bucket = (size_t)(fingerprint % cache->bucket_count);
  for (size_t i = cache->buckets[bucket]; i != SIZE_MAX; i = cache->entries[i].next) {
    const branch_amp_cache_entry_t *entry = &cache->entries[i];
    if (entry->key.fingerprint == fingerprint &&
        residual_cache_key_matches_residual(&entry->key, residual)) {
      *out = entry;
      return true;
    }
  }
  return true;
}

static bool branch_amp_cache_store(branch_amp_cache_t *cache, const qsop_residual_t *residual,
                                   branch_c64_t amp, long double numeric_error_bound,
                                   uint64_t search_nodes, qsop_error_t *error) {
  if (qsop_residual_active_vars(residual) < cache->min_vars) {
    return true;
  }
  branch_amp_cache_entry_t entry = {
      .amp = amp,
      .numeric_error_bound = numeric_error_bound,
      .search_nodes = search_nodes,
      .next = SIZE_MAX,
  };
  if (!residual_cache_key_create(residual, &entry.key, error)) {
    return false;
  }
  const uint64_t entry_bytes = sizeof(entry) + residual_key_estimated_bytes(&entry.key);
  if (cache->budget_bytes != 0 && (entry_bytes > cache->budget_bytes ||
                                   cache->estimated_bytes > cache->budget_bytes - entry_bytes)) {
    residual_cache_key_free(&entry.key);
    return true;
  }
  if (!branch_amp_cache_reserve(cache, cache->len + 1U, error)) {
    residual_cache_key_free(&entry.key);
    return false;
  }
  if (cache->bucket_count == 0) {
    if (!branch_amp_cache_rehash(cache, 64U, error)) {
      residual_cache_key_free(&entry.key);
      return false;
    }
  } else if (cache->bucket_count <= SIZE_MAX / 2U && cache->len + 1U > cache->bucket_count * 2U &&
             !branch_amp_cache_rehash(cache, cache->bucket_count * 2U, error)) {
    residual_cache_key_free(&entry.key);
    return false;
  }
  const size_t bucket = (size_t)(entry.key.fingerprint % cache->bucket_count);
  entry.next = cache->buckets[bucket];
  cache->entries[cache->len] = entry;
  cache->buckets[bucket] = cache->len;
  cache->len++;
  add_saturating_u64(&cache->estimated_bytes, entry_bytes);
  return true;
}

/* Accumulate one delegated component's stats into the running total, mirroring the spirit of
 * merge_delegated_stats above but typed for plain qsop_solve_stats_t on both sides (this path
 * has no branch_search_stats_t -- no residual search, no leaf/fallthrough counters to merge). */
static void merge_single_mode_stats(qsop_solve_stats_t *stats,
                                    const qsop_solve_stats_t *delegated) {
  if (stats == NULL || delegated == NULL) {
    return;
  }
  stats->treewidth_delegations += delegated->treewidth_delegations;
  stats->rankwidth_delegations += delegated->rankwidth_delegations;
  stats->table_entries += delegated->table_entries;
  stats->signature_entries += delegated->signature_entries;
  stats->join_pairs += delegated->join_pairs;
  stats->join_signature_pairs += delegated->join_signature_pairs;
  if (delegated->max_table_entries > stats->max_table_entries) {
    stats->max_table_entries = delegated->max_table_entries;
  }
  if (delegated->max_signature_entries > stats->max_signature_entries) {
    stats->max_signature_entries = delegated->max_signature_entries;
  }
  if (delegated->decomposition_width > stats->decomposition_width) {
    stats->decomposition_width = delegated->decomposition_width;
  }
  if (delegated->rankwidth_cutrank_width > stats->rankwidth_cutrank_width) {
    stats->rankwidth_cutrank_width = delegated->rankwidth_cutrank_width;
  }
}

/* Per-component delegate-or-error decision. Duplicates (does not share) the veto/cost-model
 * *ordering* already in branch_try_rankwidth_delegate above -- same constants, same policy
 * fields -- but against qsop_instance_t/qsop_amplitude_t instead of
 * qsop_residual_t/uint64_t[r], and without the calibration-only machinery (JSONL sink veto-
 * reason logging, "calibrating" timing bypass) since --branch-calibrate-backends is rejected
 * together with --solve-mode single-fourier at the CLI layer.
 *
 * NOTE for future maintainers: if branch_try_rankwidth_delegate's cost model is ever retuned,
 * update this function too.  The treewidth cap is intentionally wider here than in count-table
 * mode because single-Fourier table size is independent of the modulus. */
/* Lazily computes a treewidth elimination order the first time it's actually needed.
 * *order starts out either NULL or the stats-captured order (only valid for nvars <= 63,
 * see the comment in branch_single_mode_delegate_component below); when NULL, this runs
 * qsop_treewidth_order_alloc, which has no such ceiling (its adjacency representation
 * always uses the multi-word bitset form). No-op (and free of an extra min-fill run) once
 * *order is already set. */
static bool branch_single_mode_ensure_order(const qsop_instance_t *sub, uint32_t **order,
                                            uint32_t *order_width, bool *order_owned,
                                            qsop_error_t *error) {
  if (*order != NULL) {
    return true;
  }
  uint32_t *fresh = NULL;
  uint32_t width = 0;
  if (!qsop_treewidth_order_alloc(sub, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &fresh, &width,
                                  error)) {
    return false;
  }
  *order = fresh;
  *order_width = width;
  *order_owned = true;
  return true;
}

static bool branch_single_mode_delegate_component(
    const qsop_instance_t *sub, uint32_t max_vars, uint32_t target_mode,
    const qsop_branch_single_mode_options_t *options, bool fail_on_refusal, bool *out_delegated,
    qsop_amplitude_t *out, qsop_solve_stats_t *io_stats, qsop_error_t *error) {
  if (out_delegated != NULL) {
    *out_delegated = false;
  }
  *out = (qsop_amplitude_t){0};

  if (sub->nvars == 0) {
    branch_root_of_unity(sub->r, target_mode, sub->constant % sub->r, &out->re, &out->im);
    if (out_delegated != NULL) {
      *out_delegated = true;
    }
    return true;
  }
  if (sub->nvars > max_vars) {
    set_error(error,
              "branch single-fourier solver refuses a %" PRIu32
              "-variable component; pass a larger --max-vars",
              sub->nvars);
    return false;
  }

  const qsop_branch_policy_t policy = branch_policy_normalize(&options->policy);
  const qsop_branch_rw_source_t rw_source = options->rw_source;

  /* qsop_compute_stats_with_order only populates `order` for nvars <= 63 -- its width-
   * diagnostics path for larger instances (compute_large_width_diagnostics) computes
   * min_fill_width/prefix_cut_rank correctly but never touches the order buffer at all.
   * Mirrors the identical "want_stats_order ... sub.nvars <= 63U" guard in
   * branch_try_dp_delegate above. Passing a buffer unconditionally here previously left it
   * as all-zero garbage for larger components, crashing the treewidth delegate with
   * "found no factor for variable" -- caught via the gauntlet solve-readiness re-probe. */
  const bool want_stats_order = sub->nvars <= 63U;
  uint32_t *stats_order = want_stats_order ? calloc(sub->nvars, sizeof(*stats_order)) : NULL;
  if (want_stats_order && stats_order == NULL) {
    set_error(error, "out of memory while allocating branch single-fourier stats order buffer");
    return false;
  }
  qsop_stats_t sub_stats = {0};
  if (!qsop_compute_stats_with_order(sub, &sub_stats, stats_order, error)) {
    free(stats_order);
    return false;
  }
  const bool treewidth_available = sub_stats.width_diagnostics_available;
  const uint32_t treewidth_width = sub_stats.min_fill_width;
  const uint32_t prefix_cut_rank = sub_stats.prefix_cut_rank;

  /* order/order_width: lazily computed (see branch_single_mode_ensure_order) the first time
   * a treewidth order is actually needed, reusing stats_order when available. */
  uint32_t *order = stats_order;
  uint32_t order_width = treewidth_width;
  bool order_owned = false;

  bool use_rankwidth = false;
  qsop_rankwidth_decomposition_t *decomposition = NULL;
  uint32_t cutrank_width = 0;
  bool setup_ok = true;

  if (rw_source != QSOP_BRANCH_RW_SOURCE_NONE) {
    const bool cheap_treewidth = branch_treewidth_is_cheap(
        &policy, treewidth_available, treewidth_width, sub->nvars, prefix_cut_rank);
    const bool prefer_treewidth =
        branch_treewidth_preferred(treewidth_available, prefix_cut_rank, treewidth_width);
    const bool probe_too_costly = branch_rankwidth_probe_too_costly(
        &policy,
        treewidth_available && treewidth_width <= BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH,
        sub->nvars, treewidth_single_mode_table_forecast(treewidth_width));
    if (probe_too_costly && io_stats != NULL) {
      io_stats->branch_rankwidth_skips++;
    }

    if (!cheap_treewidth && !prefer_treewidth && !probe_too_costly) {
      const qsop_rankwidth_generator_t generator = (rw_source == QSOP_BRANCH_RW_SOURCE_NATIVE)
                                                       ? QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT
                                                       : QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH;
      const bool use_from_treewidth =
          generator == QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH && treewidth_available;
      if (use_from_treewidth &&
          !branch_single_mode_ensure_order(sub, &order, &order_width, &order_owned, error)) {
        setup_ok = false;
      } else {
        const bool gen_ok =
            use_from_treewidth
                ? qsop_rankwidth_decomposition_from_order(sub, order, &decomposition, error)
                : qsop_rankwidth_decomposition_generate(sub, QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
                                                        &decomposition, error);
        if (!gen_ok) {
          setup_ok = false;
        } else {
          uint64_t rankwidth_table = 0;
          uint64_t rankwidth_join = 0;
          if (!qsop_rankwidth_decomposition_width(sub, decomposition, &cutrank_width, error) ||
              !qsop_rankwidth_decomposition_forecast(sub, decomposition, &rankwidth_table,
                                                     &rankwidth_join, error)) {
            setup_ok = false;
          } else if (cutrank_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
            const uint64_t treewidth_table =
                treewidth_available ? treewidth_single_mode_table_forecast(treewidth_width) : 0;
            const uint64_t treewidth_join =
                treewidth_available ? treewidth_join_pair_forecast(treewidth_width, sub->nvars) : 0;
            rankwidth_table = binary_assignment_forecast(cutrank_width);
            /* sig_est = 2^cutrank = rankwidth_table (single-Fourier forecasts omit the r factor).
             */
            const bool cost_model_favors_rw = branch_cost_model_favors_rankwidth(
                &policy, treewidth_available, rankwidth_table, rankwidth_join, rankwidth_table,
                treewidth_table, treewidth_join);
            use_rankwidth = cost_model_favors_rw &&
                            (!treewidth_available || rankwidth_table < treewidth_table) &&
                            (!treewidth_available || rankwidth_join <= treewidth_join) &&
                            (!treewidth_available ||
                             treewidth_width > BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH ||
                             rankwidth_should_override_treewidth(treewidth_width, cutrank_width,
                                                                 treewidth_table));
          }
        }
      }
    }
  }

  if (setup_ok && !use_rankwidth && treewidth_available &&
      treewidth_width <= BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    setup_ok = branch_single_mode_ensure_order(sub, &order, &order_width, &order_owned, error);
  }

  bool ok;
  qsop_solve_stats_t delegated = {0};
  if (!setup_ok) {
    ok = false;
  } else if (use_rankwidth) {
    if (options->precision == QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE) {
      ok = qsop_solve_rankwidth_single_mode(sub, decomposition, max_vars, target_mode, out,
                                            &delegated, options->trace, error);
    } else {
      ok = qsop_solve_rankwidth_single_mode_f64(sub, decomposition, max_vars, target_mode,
                                                options->simd, out, &delegated, options->trace,
                                                error);
    }
    delegated.rankwidth_delegations++;
  } else if (treewidth_available && treewidth_width <= BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH &&
             order_width <= BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    if (options->precision == QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE) {
      ok = qsop_solve_treewidth_precomputed_order_single_mode(
          sub, BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_BAG_VARS, order, order_width, target_mode, out,
          &delegated, options->trace, error);
    } else {
      ok = qsop_solve_treewidth_precomputed_order_single_mode_f64(
          sub, BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_BAG_VARS, order, order_width, target_mode,
          options->simd, out, &delegated, options->trace, error);
    }
    delegated.treewidth_delegations++;
  } else if (decomposition != NULL && cutrank_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
    /* Treewidth unavailable/too wide, but rankwidth is viable even though the cost model
     * (computed above only when !cheap_treewidth && !prefer_treewidth) didn't prefer it. */
    if (options->precision == QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE) {
      ok = qsop_solve_rankwidth_single_mode(sub, decomposition, max_vars, target_mode, out,
                                            &delegated, options->trace, error);
    } else {
      ok = qsop_solve_rankwidth_single_mode_f64(sub, decomposition, max_vars, target_mode,
                                                options->simd, out, &delegated, options->trace,
                                                error);
    }
    delegated.rankwidth_delegations++;
  } else {
    if (fail_on_refusal) {
      set_error(error,
                "branch single-fourier: connected component (%" PRIu32
                " vars) has treewidth %" PRIu32 " and rankwidth cutrank %" PRIu32
                "; neither is within its delegate cap (%u / %u) -- no delegate available",
                sub->nvars, treewidth_width, cutrank_width,
                BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH, BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH);
      ok = false;
    } else {
      ok = true;
    }
  }

  qsop_rankwidth_decomposition_free(decomposition);
  if (order_owned) {
    free(order);
  } else {
    free(stats_order);
  }
  if (ok) {
    if (delegated.treewidth_delegations != 0 || delegated.rankwidth_delegations != 0) {
      if (out_delegated != NULL) {
        *out_delegated = true;
      }
      merge_single_mode_stats(io_stats, &delegated);
    }
  }
  return ok;
}

static bool branch_sum_rec_single_mode(qsop_residual_t *residual, branch_single_mode_state_t *state,
                                       branch_c64_t *out, qsop_error_t *error);

static uint64_t branch_cache_budget_bytes(uint64_t mib) {
  return saturating_mul_u64(mib, UINT64_C(1024) * UINT64_C(1024));
}

static bool branch_single_mode_state_init(branch_single_mode_state_t *state,
                                          const qsop_instance_t *qsop, uint32_t max_vars,
                                          uint32_t target_mode,
                                          const qsop_branch_single_mode_options_t *options,
                                          qsop_solve_stats_t *stats, qsop_error_t *error) {
  const qsop_branch_single_mode_options_t o =
      options != NULL ? *options : (qsop_branch_single_mode_options_t){0};
  *state = (branch_single_mode_state_t){
      .r = qsop->r,
      .target_mode = target_mode,
      .max_vars = max_vars,
      .rw_source = o.rw_source,
      .policy = o.policy,
      .heuristic = o.heuristic,
      .fallback = o.fallback,
      .precision = o.precision,
      .kernel = o.kernel,
      .simd = o.simd,
      .max_search_nodes =
          o.max_search_nodes != 0 ? o.max_search_nodes : BRANCH_SINGLE_DEFAULT_MAX_SEARCH_NODES,
      .max_fallback_vars =
          o.max_fallback_vars != 0 ? o.max_fallback_vars : BRANCH_SINGLE_DEFAULT_MAX_FALLBACK_VARS,
      .max_recursion_depth = o.max_recursion_depth != 0 ? o.max_recursion_depth : qsop->nvars,
      /* The rule rests on omega^(r/2) = -1 raised to an odd power; an even target mode turns
       * (-1)^(t*(s+S)) into 1 and the constraint disappears, so refuse rather than mis-fold. */
      .propagate = o.propagate != QSOP_BRANCH_SINGLE_PROPAGATE_OFF && qsop->r >= 2U &&
                   (qsop->r % 2U) == 0U && (target_mode % 2U) == 1U,
      .stats = stats,
      .trace = o.trace,
  };
  const uint64_t cache_budget_mib =
      o.cache_budget_mib != 0 ? o.cache_budget_mib : BRANCH_SINGLE_DEFAULT_CACHE_BUDGET_MIB;
  state->amp_cache = (branch_amp_cache_t){
      .budget_bytes = branch_cache_budget_bytes(cache_budget_mib),
      .min_vars = o.cache_min_vars != 0 ? o.cache_min_vars : BRANCH_SINGLE_DEFAULT_CACHE_MIN_VARS,
  };
  if (!branch_phase_cache_init(&state->phase_cache, qsop->r, target_mode, o.phase_cache_lg_cap,
                               error)) {
    return false;
  }
  return true;
}

static void branch_single_mode_state_free(branch_single_mode_state_t *state) {
  if (state == NULL) {
    return;
  }
  branch_phase_cache_free(&state->phase_cache);
  branch_amp_cache_free(&state->amp_cache);
}

static void branch_single_mode_merge_final_stats(branch_single_mode_state_t *state) {
  qsop_solve_stats_t *stats = state->stats;
  if (stats == NULL) {
    return;
  }
  stats->search_nodes = state->nodes;
  stats->leaf_assignments = state->leaves;
  stats->cache_hits = state->cache_hits;
  stats->cache_misses = state->cache_misses;
  stats->cache_avoided_nodes = state->cache_avoided_nodes;
  stats->cache_entries = (uint64_t)state->amp_cache.len;
  stats->cache_key_bytes = state->amp_cache.estimated_bytes;
  stats->cache_estimated_bytes = state->amp_cache.estimated_bytes;
  stats->branch_fallthroughs = state->branch_fallthroughs;
  stats->branch_propagations = state->propagations;
  stats->branch_zero_prunes = state->zero_prunes;
}

static void branch_single_note_residual_shape(branch_single_mode_state_t *state,
                                              const qsop_residual_t *residual) {
  if (state->stats == NULL) {
    return;
  }
  max_u32(&state->stats->max_residual_vars, qsop_residual_active_vars(residual));
  max_u32(&state->stats->max_residual_edges, qsop_residual_active_edges(residual));
}

static bool branch_edge_free_single_mode(const qsop_residual_t *residual,
                                         branch_single_mode_state_t *state, branch_c64_t *out,
                                         qsop_error_t *error) {
  (void)error;
  const uint64_t edge_free_start = qsop_trace_begin(state->trace);
  branch_c64_t z = branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual));
  const uint64_t r = qsop_residual_modulus(residual);
  for (uint32_t v = 0; v < qsop_residual_nvars(residual); v++) {
    if (!qsop_residual_var_active(residual, v)) {
      continue;
    }
    const uint64_t unary = qsop_residual_unary(residual, v) % r;
    branch_c64_t factor = {0.0, 0.0};
    if (unary == 0) {
      factor = (branch_c64_t){2.0, 0.0};
    } else if ((r & 1U) == 0 && unary == r / 2U && (state->target_mode & 1U) != 0) {
      *out = c64_zero();
      add_saturating_u64(&state->leaves, assignment_count(qsop_residual_active_vars(residual)));
      qsop_trace_emit_elapsed(state->trace, "branch.single.edge_free", state->depth,
                              qsop_residual_active_vars(residual), edge_free_start);
      return true;
    } else {
      const branch_c64_t phase = branch_phase_lookup(&state->phase_cache, unary);
      factor = (branch_c64_t){1.0 + phase.re, phase.im};
    }
    z = c64_mul(z, factor);
    c64_accum_error(1, &state->numeric_error_bound);
  }
  *out = z;
  add_saturating_u64(&state->leaves, assignment_count(qsop_residual_active_vars(residual)));
  qsop_trace_emit_elapsed(state->trace, "branch.single.edge_free", state->depth,
                          qsop_residual_active_vars(residual), edge_free_start);
  return true;
}

static bool branch_solve_component_single_mode(const qsop_instance_t *sub,
                                               branch_single_mode_state_t *state, branch_c64_t *out,
                                               qsop_error_t *error) {
  qsop_residual_t *component_residual = NULL;
  if (!qsop_residual_create(sub, &component_residual, error)) {
    return false;
  }
  state->depth++;
  const bool ok = branch_sum_rec_single_mode(component_residual, state, out, error);
  state->depth--;
  qsop_residual_free(component_residual);
  return ok;
}

static bool branch_sum_components_single_mode(qsop_residual_t *residual,
                                              branch_single_mode_state_t *state, bool *out_split,
                                              branch_c64_t *out, qsop_error_t *error) {
  *out_split = false;
  const uint32_t nvars = qsop_residual_nvars(residual);
  uint32_t *component = malloc((nvars == 0 ? 1U : nvars) * sizeof(*component));
  if (component == NULL) {
    set_error(error, "out of memory while splitting branch single-Fourier residual components");
    return false;
  }

  uint32_t ncomponents = 0;
  const uint64_t split_start = qsop_trace_begin(state->trace);
  if (!qsop_residual_active_components(residual, component, &ncomponents, error)) {
    free(component);
    return false;
  }
  if (state->stats != NULL) {
    max_u32(&state->stats->max_residual_components, ncomponents);
  }
  qsop_trace_emit_elapsed(state->trace, "branch.single.component_split", state->depth, ncomponents,
                          split_start);
  if (ncomponents <= 1U) {
    free(component);
    return true;
  }

  branch_c64_t acc = branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual));
  for (uint32_t c = 0; c < ncomponents; c++) {
    qsop_instance_t sub = {0};
    branch_c64_t part = c64_zero();
    if (!build_residual_subinstance(residual, component, c, &sub, error) ||
        !branch_solve_component_single_mode(&sub, state, &part, error)) {
      free_subinstance(&sub);
      free(component);
      return false;
    }
    acc = c64_mul(acc, part);
    c64_accum_error(1, &state->numeric_error_bound);
    free_subinstance(&sub);
  }
  free(component);
  *out = acc;
  *out_split = true;
  return true;
}

static bool branch_try_single_mode_delegate(qsop_residual_t *residual,
                                            branch_single_mode_state_t *state, bool *out_delegated,
                                            branch_c64_t *out, qsop_error_t *error) {
  *out_delegated = false;
  qsop_instance_t sub = {0};
  qsop_amplitude_t delegated_amp = {0};
  if (!build_active_residual_subinstance(residual, &sub, error)) {
    return false;
  }
  const qsop_branch_single_mode_options_t delegate_options = {
      .rw_source = state->rw_source,
      .policy = state->policy,
      .heuristic = state->heuristic,
      .fallback = state->fallback,
      .precision = state->precision,
      .kernel = state->kernel,
      .simd = state->simd,
      .trace = state->trace,
  };
  const bool ok = branch_single_mode_delegate_component(
      &sub, state->max_vars, state->target_mode, &delegate_options,
      state->fallback == QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY, out_delegated, &delegated_amp,
      state->stats, error);
  free_subinstance(&sub);
  if (!ok || !*out_delegated) {
    return ok;
  }

  branch_c64_t amp = {delegated_amp.re, delegated_amp.im};
  const branch_c64_t constant =
      branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual));
  *out = c64_mul(constant, amp);
  state->numeric_error_bound += delegated_amp.numeric_error_bound;
  c64_accum_error(2, &state->numeric_error_bound);
  return true;
}

static bool branch_single_mode_cache_lookup(branch_single_mode_state_t *state,
                                            const qsop_residual_t *residual, branch_c64_t *out) {
  if (qsop_residual_active_vars(residual) < state->amp_cache.min_vars) {
    return false;
  }
  const branch_amp_cache_entry_t *entry = NULL;
  branch_amp_cache_find(&state->amp_cache, residual, &entry);
  if (entry == NULL) {
    state->cache_misses++;
    return false;
  }
  state->cache_hits++;
  if (entry->search_nodes > 1U) {
    add_saturating_u64(&state->cache_avoided_nodes, entry->search_nodes - 1U);
  }
  state->numeric_error_bound += entry->numeric_error_bound;
  *out = entry->amp;
  return true;
}

/* The amplitude of `residual` exactly as it stands: propagation, node and depth accounting all
 * belong to branch_sum_rec_single_mode below, which wraps this. */
static bool branch_sum_rec_single_mode_node(qsop_residual_t *residual,
                                            branch_single_mode_state_t *state, branch_c64_t *out,
                                            qsop_error_t *error) {
  branch_single_note_residual_shape(state, residual);

  if (branch_single_mode_cache_lookup(state, residual, out)) {
    qsop_trace_emit(state->trace, "branch.single.cache_hit", state->depth,
                    qsop_residual_active_vars(residual), 0);
    return true;
  }

  const uint64_t subtree_start_nodes = state->nodes;
  const long double subtree_start_error = state->numeric_error_bound;
  bool ok = true;
  if (qsop_residual_active_vars(residual) == 0) {
    state->leaves++;
    *out = branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual));
  } else if (qsop_residual_active_edges(residual) == 0) {
    ok = branch_edge_free_single_mode(residual, state, out, error);
  } else {
    bool did_split = false;
    ok = branch_sum_components_single_mode(residual, state, &did_split, out, error);
    if (ok && !did_split) {
      bool delegated = false;
      ok = branch_try_single_mode_delegate(residual, state, &delegated, out, error);
      if (ok && !delegated) {
        if (qsop_residual_active_vars(residual) > state->max_fallback_vars) {
          set_error(error,
                    "branch single-Fourier fallback refused component with %" PRIu32
                    " active vars: residual fallback cap %" PRIu32
                    " exceeded; try --branch-single-max-fallback-vars=%" PRIu32
                    " or --branch-single-fourier-fallback=delegate-only",
                    qsop_residual_active_vars(residual), state->max_fallback_vars,
                    qsop_residual_active_vars(residual));
          ok = false;
        } else if (state->precision == QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE) {
          set_error(error, "branch single-Fourier residual fallback does not implement "
                           "--branch-single-precision long-double yet");
          ok = false;
        } else {
          state->branch_fallthroughs++;
          qsop_trace_emit(state->trace, "branch.single.fallback_node", state->depth,
                          qsop_residual_active_vars(residual), 0);
          uint32_t v = 0;
          const uint64_t select_start = qsop_trace_begin(state->trace);
          if (!choose_branch_var(residual, state->heuristic, &v, error)) {
            ok = false;
          } else {
            qsop_trace_emit_elapsed(state->trace, "branch.single.select_variable", state->depth, v,
                                    select_start);
            branch_c64_t amp0 = c64_zero();
            branch_c64_t amp1 = c64_zero();
            for (uint8_t value = 0; ok && value <= 1U; value++) {
              const size_t checkpoint = qsop_residual_checkpoint(residual);
              if (!qsop_residual_branch(residual, v, value, error)) {
                ok = false;
                break;
              }
              state->depth++;
              branch_c64_t branch_amp = c64_zero();
              ok = branch_sum_rec_single_mode(residual, state, &branch_amp, error);
              state->depth--;
              const bool undo_ok = qsop_residual_undo(residual, checkpoint, error);
              if (!ok || !undo_ok) {
                ok = false;
                break;
              }
              if (value == 0) {
                amp0 = branch_amp;
              } else {
                amp1 = branch_amp;
              }
            }
            if (ok) {
              *out = c64_add(amp0, amp1);
              c64_accum_error(1, &state->numeric_error_bound);
            }
          }
        }
      }
    }
  }
  if (!ok) {
    return false;
  }

  const uint64_t subtree_nodes = state->nodes - subtree_start_nodes + 1U;
  const long double subtree_error = state->numeric_error_bound - subtree_start_error;
  if (!branch_amp_cache_store(&state->amp_cache, residual, *out, subtree_error, subtree_nodes,
                              error)) {
    return false;
  }
  if (qsop_residual_active_vars(residual) >= state->amp_cache.min_vars) {
    state->cache_stores++;
    qsop_trace_emit(state->trace, "branch.single.cache_store", state->depth,
                    (uint64_t)state->amp_cache.len, 0);
  }
  return true;
}

/* Unit propagation, in the SAT sense, for sum-over-paths: before doing anything expensive at a
 * node, sum out every variable that the Hadamard rule can eliminate outright, cascading through the
 * pins it creates. Two payoffs. The residual shrinks, so the delegate probe below may now find a
 * component narrow enough to hand off. And the cascade can derive an isolated variable with unary
 * r/2 -- factor 1 + omega^(r/2) = 0 -- which is a conflict: the amplitude of this whole subtree is
 * exactly zero and none of it needs exploring.
 *
 * Each elimination doubles the amplitude, so the subtree's value is scaled by 2^doublings on the
 * way out; the cached value belongs to the propagated residual, which is what the recursion below
 * and the cache both see. Undoing the checkpoint restores the caller's residual, so this composes
 * with the trail exactly like an ordinary branch. */
static bool branch_sum_rec_single_mode(qsop_residual_t *residual, branch_single_mode_state_t *state,
                                       branch_c64_t *out, qsop_error_t *error) {
  if (state->max_recursion_depth != 0 && state->depth > state->max_recursion_depth) {
    set_error(error, "branch single-Fourier recursion-depth cap exceeded");
    return false;
  }
  state->nodes++;
  if (state->max_search_nodes != 0 && state->nodes > state->max_search_nodes) {
    set_error(error,
              "branch single-Fourier search-node cap exceeded (%" PRIu64
              "); try --branch-single-max-search-nodes with a larger value",
              state->max_search_nodes);
    return false;
  }

  if (!state->propagate) {
    return branch_sum_rec_single_mode_node(residual, state, out, error);
  }

  const size_t checkpoint = qsop_residual_checkpoint(residual);
  uint32_t doublings = 0;
  bool zero = false;
  const uint64_t propagate_start = qsop_trace_begin(state->trace);
  if (!qsop_residual_propagate(residual, &doublings, &zero, error)) {
    return false;
  }
  qsop_trace_emit_elapsed(state->trace, "branch.single.propagate", state->depth, doublings,
                          propagate_start);
  state->propagations += doublings;

  if (zero) {
    state->zero_prunes++;
    qsop_trace_emit(state->trace, "branch.single.zero_prune", state->depth,
                    qsop_residual_active_vars(residual), 0);
    *out = c64_zero();
    return qsop_residual_undo(residual, checkpoint, error);
  }

  branch_c64_t amp = c64_zero();
  if (!branch_sum_rec_single_mode_node(residual, state, &amp, error)) {
    return false;
  }
  if (!qsop_residual_undo(residual, checkpoint, error)) {
    return false;
  }
  if (doublings != 0) {
    const long double factor = ldexpl(1.0L, (int)doublings);
    amp.re *= factor;
    amp.im *= factor;
  }
  *out = amp;
  return true;
}

static bool branch_solve_single_mode_residual(const qsop_instance_t *qsop, uint32_t max_vars,
                                              uint32_t target_mode,
                                              const qsop_branch_single_mode_options_t *options,
                                              qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                              qsop_error_t *error) {
  branch_single_mode_state_t state = {0};
  qsop_residual_t *residual = NULL;
  if (!branch_single_mode_state_init(&state, qsop, max_vars, target_mode, options, stats, error) ||
      !qsop_residual_create(qsop, &residual, error)) {
    branch_single_mode_state_free(&state);
    qsop_residual_free(residual);
    return false;
  }

  branch_c64_t amp = c64_zero();
  const bool ok = branch_sum_rec_single_mode(residual, &state, &amp, error);
  if (ok) {
    out->re = (long double)amp.re;
    out->im = (long double)amp.im;
    out->numeric_error_bound = state.numeric_error_bound;
    branch_single_mode_merge_final_stats(&state);
  }
  qsop_residual_free(residual);
  branch_single_mode_state_free(&state);
  return ok;
}

bool qsop_solve_branch_single_mode(const qsop_instance_t *qsop, uint32_t max_vars,
                                   uint32_t target_mode,
                                   const qsop_branch_single_mode_options_t *options,
                                   qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                   qsop_error_t *error) {
  const qsop_branch_single_mode_options_t o =
      options != NULL ? *options : (qsop_branch_single_mode_options_t){0};
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->r == 0) {
    set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }
  /* Deliberately no `qsop->r > UINT32_MAX` guard here -- that's the entire point of this
   * entry point (see the file-level comment above). */
  /* This is deliberately a much looser bound than max_vars: rejecting on the whole instance's
   * raw nvars here, before ever attempting a component split, would wrongly refuse a large
   * instance that's actually a disjoint union of many small, easily delegatable components
   * (e.g. a batch of independent sub-circuits). The component split itself is a cheap O(nvars)
   * graph traversal, so only guard here against a truly pathological input; the real
   * accept/reject decision for a component that doesn't split small enough is the per-component
   * max_vars check inside branch_single_mode_delegate_component, which already fires before the
   * expensive width diagnostic. */
  const uint64_t root_sanity_limit = (uint64_t)max_vars * BRANCH_ROOT_SANITY_MULTIPLIER;
  if ((uint64_t)qsop->nvars > root_sanity_limit) {
    set_error(error,
              "branch single-fourier solver refuses %" PRIu32
              " variables outright (exceeds %ux --max-vars); pass a larger --max-vars",
              qsop->nvars, BRANCH_ROOT_SANITY_MULTIPLIER);
    return false;
  }

  return branch_solve_single_mode_residual(qsop, max_vars, target_mode, &o, out, stats, error);
}
