#include "branch_shadow.h"
#include "component_key.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/qsop_stats.h"
#include "dlx4sop/residual.h"
#include "dlx4sop/residue.h"
#include "trace.h"
#include "../core/qsop_internal.h"

#include <assert.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Private policy defaults (kept out of the public header).
 * --------------------------------------------------------------------------- */
#define BRANCH_POLICY_DEFAULT_RW_FIXED_OVERHEAD_NS 20000UL
#define BRANCH_POLICY_DEFAULT_TW_FIXED_OVERHEAD_NS 10000UL
#define BRANCH_POLICY_DEFAULT_C_RW_TABLE 80UL
#define BRANCH_POLICY_DEFAULT_C_RW_JOIN 40UL
#define BRANCH_POLICY_DEFAULT_C_RW_SIG 2000UL
/* ns per treewidth DP table entry *touched*, summed over every elimination step (see
 * qsop_stats_t.min_fill_dp_work). The unit changed with the unified model: it used to price the
 * peak table, 2^(width+1), with a separate per-nvars join term on top. Calibrated against the
 * fixtures whose right answer is known -- a 32-var path and a 6x6 grid must stay on treewidth,
 * K12,12 / K16,16 / K20,20 must go to rankwidth, and qwalk-noancilla_9 / grover-v-chain_13 must
 * not be probed; any value in [2,8] satisfies all of them, 20 does not. */
#define BRANCH_POLICY_DEFAULT_C_TW_TABLE 4UL
#define BRANCH_POLICY_DEFAULT_C_RW_PROBE 2UL
#define BRANCH_POLICY_DEFAULT_RW_MIN_SPEEDUP 1.1
#define BRANCH_SINGLE_DEFAULT_MAX_FALLBACK_VARS 64U
#define BRANCH_SINGLE_DEFAULT_MAX_SEARCH_NODES UINT64_C(10000000)
#define BRANCH_SINGLE_DEFAULT_CACHE_BUDGET_MIB 256U
#define BRANCH_SINGLE_DEFAULT_CACHE_MIN_VARS 12U
#define BRANCH_SINGLE_DEFAULT_PHASE_CACHE_LG_CAP 16U
#define BRANCH_SINGLE_MAX_PHASE_CACHE_LG_CAP 30U
#define BRANCH_SINGLE_DEFAULT_LOOKAHEAD_CANDIDATES 8U
#define BRANCH_SINGLE_DEFAULT_MAX_CONDITIONING_NODES UINT64_C(4096)
#define BRANCH_SINGLE_DEFAULT_DELEGATE_REPROBE_INTERVAL 2U
#define BRANCH_SINGLE_DEFAULT_MAX_STAGNANT_LEVELS 1U
/* AUTO shadow-shortlisting trigger: below this the plain unlock3/unlock4 heuristic is already
 * cheap and effective, so building a shadow graph is not worth its own O(nvars+nedges) scan. */
#define BRANCH_SHADOW_AUTO_TRIGGER_MIN_VARS 128U
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

/* The residual's additive constant is deliberately absent: see qsop_residual_fingerprint. Both
 * caches below therefore store the *constant-free* value of a residual, and both re-apply the
 * rotation on a hit -- branch_counts_shift_add for the count table, a unit-modulus phase for the
 * single-Fourier amplitude. That is worth up to an r-fold reduction in distinct entries. */
typedef struct residual_cache_key {
  uint64_t fingerprint;
  bool canonical;
  uint64_t r;
  uint32_t nvars;
  uint32_t nedges;
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
/* The single-Fourier treewidth delegate is admitted by *cost* (min-fill DP work), not raw
 * width -- see BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_DP_WORK below. This width value is only a
 * memory-safety ceiling: the DP peaks at a 2^width table, ~2 GB of long double complex at 26,
 * doubling per width. It was 25 (a pure width cap) before the cost gate; widening it by one to
 * 26 lets a cheap-but-wide component (e.g. qnn_24 at width 26, ~1.1e9 work) reach the DP while
 * the DP-work budget keeps a dense width-26 component (realamprandom_26, ~7e9 work) out. Width
 * 27 is deliberately *not* admitted: its 2^27 table plus the branch backend's own residual/
 * cache state OOMs a 12 GiB budget (qnn_25 dies there), so a one-width bump is the safe reach. */
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH 26U
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_BAG_VARS                                              \
  (BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH + 1U)
/* Cost gate: refuse the treewidth delegate when the min-fill DP work (sum over elimination
 * steps of 2^bagsize, qsop_stats_t.min_fill_dp_work) exceeds this. Admission then tracks the
 * real DP cost rather than the width alone, which is a poor proxy: qnn_24 (width 26) solves in
 * ~22 s while realamprandom_25 (width 25) times out. Calibrated on the gauntlet corpus so the
 * slowest solved case (grover-v-chain_15, ~2.85e9 work, 62 s) stays admitted while the dense
 * width-26 realamprandom_26 (~7.1e9) is refused. The budget sits above grover-v-chain_15 and
 * below realamprandom_26; the width and memory ceilings below keep out the wider qnn the budget
 * would otherwise admit.
 *
 * Raised 3.2e9 -> 4.0e9 to newly admit qnn_26 (width 26, ~3.76e9 work, ~53 s, ~7.5 GiB peak --
 * memory-safe under the 12 GiB budget below), while still refusing realamprandom_26 (~7.1e9,
 * times out). The gap [3.76e9, 7.1e9] is the calibration window. */
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_DP_WORK UINT64_C(4000000000)
#define BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH 12U
/* Memory-safety budget for the treewidth delegate, in MiB. The DP's *peak* live memory is
 * dominated by join intermediates, not the final 2^width table: a width-26 qnn component whose
 * final long-double-complex table is ~2 GiB was measured to peak at ~7.5 GiB (~3.6x). Forecast
 * it conservatively as 2^width * PEAK_BYTES_PER_ENTRY (128 B/entry reproduces the measured peak
 * with headroom: width 26 -> ~8.6 GiB admitted under 12 GiB, width 27 -> ~17 GiB refused) and
 * reject over-budget components *gracefully* as BRANCH_DELEGATE_MISS_MEMORY, rather than letting
 * the DP fail its own allocation mid-run (an error, not a refusal -- observed when the width cap
 * was raised without this guard). Default 12 GiB matches the gauntlet's per-solve RLIMIT_AS;
 * lower --branch-single-delegate-max-memory-mib to match a tighter process limit. */
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_PEAK_BYTES_PER_ENTRY UINT64_C(128)
#define BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_MEMORY_MIB UINT64_C(12288)
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

static uint64_t binary_assignment_forecast(uint32_t nvars) {
  if (nvars >= 64U) {
    return UINT64_MAX;
  }
  return UINT64_C(1) << nvars;
}

static uint64_t treewidth_table_forecast(uint32_t width, uint32_t r) {
  const uint32_t bag_vars = width >= UINT32_MAX ? UINT32_MAX : width + 1U;
  return qsop_saturating_mul_u64(binary_assignment_forecast(bag_vars), r);
}

static uint64_t treewidth_join_pair_forecast(uint32_t width, uint32_t nvars) {
  const uint32_t bag_vars = width >= UINT32_MAX ? UINT32_MAX : width + 1U;
  return qsop_saturating_mul_u64(binary_assignment_forecast(bag_vars), nvars);
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
    qsop_set_error(error, "out of memory while recording residual component shape");
    return false;
  }
  for (uint32_t v = 0; v < qsop_residual_nvars(residual); v++) {
    if (!qsop_residual_var_active(residual, v)) {
      continue;
    }
    if (component[v] >= ncomponents) {
      free(sizes);
      qsop_set_error(error, "internal error: residual component index is out of range");
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
    qsop_set_error(error, "internal error: invalid branch residue shift-add argument");
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

/* The inverse of branch_counts_shift_add's rotation, as a plain copy: dst[j] = src[(j+shift) % r].
 * Strips a residual's additive constant back out of a computed count vector, which is what the
 * cache stores. No addition, hence no overflow and no count_modulus to respect. */
static void branch_counts_rotate_out(uint32_t r, uint64_t *dst, const uint64_t *src,
                                     uint32_t shift) {
  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t source = residue + delta;
    if (source >= r) {
      source -= r;
    }
    dst[residue] = src[source];
  }
}

static bool branch_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                                   const uint64_t *right, const branch_search_stats_t *stats,
                                   qsop_error_t *error) {
  if (r == 0 || dst == NULL || left == NULL || right == NULL) {
    qsop_set_error(error, "internal error: invalid branch residue convolution argument");
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
    qsop_set_error(error, "internal error: invalid branch Fourier transform argument");
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
    qsop_set_error(error, "internal error: invalid branch Fourier multiply argument");
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
    qsop_set_error(error, "internal error: null residual cache key output");
    return false;
  }
  *key = (residual_cache_key_t){0};

  const uint32_t nvars = qsop_residual_nvars(residual);
  const uint32_t nedges = qsop_residual_nedges(residual);
  key->fingerprint = qsop_residual_fingerprint(residual);
  key->r = qsop_residual_modulus(residual);
  key->nvars = nvars;
  key->nedges = nedges;
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
    qsop_set_error(error, "out of memory while allocating residual cache key");
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

static bool residual_cache_key_create_from_instance(const qsop_instance_t *sub,
                                                    residual_cache_key_t *key,
                                                    qsop_error_t *error) {
  if (key == NULL) {
    qsop_set_error(error, "internal error: null residual cache key output");
    return false;
  }
  *key = (residual_cache_key_t){0};

  key->canonical = true;
  key->r = sub->r;
  key->nvars = sub->nvars;
  key->nedges = sub->nedges;
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
    qsop_set_error(error, "out of memory while allocating canonical residual cache key");
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
  const bool ok = residual_cache_key_create_from_instance(&sub, key, error);
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
      lhs->active_vars != rhs->active_vars || lhs->active_edges != rhs->active_edges) {
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
      qsop_set_error(error, "residual cache is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / sizeof(*cache->entries)) {
    qsop_set_error(error, "residual cache is too large");
    return false;
  }

  residual_cache_entry_t *new_entries = realloc(cache->entries, new_cap * sizeof(*cache->entries));
  if (new_entries == NULL) {
    qsop_set_error(error, "out of memory while growing residual cache");
    return false;
  }

  cache->entries = new_entries;
  cache->cap = new_cap;
  return true;
}

static bool residual_cache_rehash(residual_cache_t *cache, size_t bucket_count,
                                  qsop_error_t *error) {
  if (bucket_count == 0 || bucket_count > SIZE_MAX / sizeof(*cache->buckets)) {
    qsop_set_error(error, "residual cache is too large");
    return false;
  }

  size_t *buckets = malloc(bucket_count * sizeof(*buckets));
  if (buckets == NULL) {
    qsop_set_error(error, "out of memory while allocating residual cache buckets");
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
    qsop_set_error(error, "count-table branch residual cache requires R <= UINT32_MAX");
    return false;
  }
  if (!qsop_counts_alloc((uint32_t)entry.key.r, &entry.counts, error)) {
    residual_cache_key_free(&entry.key);
    return false;
  }
  /* `counts` is the residual's histogram *including* its additive constant; the key does not carry
   * that constant, so rotate it back out before storing. branch_sum_rec re-applies it on a hit. */
  branch_counts_rotate_out((uint32_t)entry.key.r, entry.counts, counts,
                           (uint32_t)(qsop_residual_constant(residual) % entry.key.r));

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
    qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->active_var)));
    qsop_add_saturating_u64(&bytes,
                       qsop_saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->active_edge)));
    qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->unary)));
    qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64(qsop_saturating_mul_u64((uint64_t)key->nedges, 3U),
                                                  sizeof(*key->edge_u)));
  }
  return bytes;
}

static uint64_t residual_cache_count_bytes(const residual_cache_t *cache) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < cache->len; i++) {
    qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)cache->entries[i].key.r,
                                                  sizeof(*cache->entries[i].counts)));
  }
  return bytes;
}

static uint64_t residual_cache_estimated_bytes(const residual_cache_t *cache) {
  uint64_t bytes = qsop_saturating_mul_u64((uint64_t)cache->cap, sizeof(*cache->entries));
  qsop_add_saturating_u64(&bytes,
                     qsop_saturating_mul_u64((uint64_t)cache->bucket_count, sizeof(*cache->buckets)));
  qsop_add_saturating_u64(&bytes, residual_cache_key_bytes(cache));
  qsop_add_saturating_u64(&bytes, residual_cache_count_bytes(cache));
  return bytes;
}

static bool build_residual_subinstance(const qsop_residual_t *residual, const uint32_t *component,
                                       uint32_t wanted, qsop_instance_t *sub, qsop_error_t *error) {
  const uint32_t source_vars = qsop_residual_nvars(residual);
  const uint32_t source_edges = qsop_residual_nedges(residual);
  uint32_t *map = malloc((source_vars == 0 ? 1U : source_vars) * sizeof(*map));
  if (map == NULL) {
    qsop_set_error(error, "out of memory while building residual component subinstance");
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
    qsop_set_error(error, "out of memory while allocating residual component subinstance");
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
    qsop_set_error(error, "out of memory while building active residual subinstance");
    return false;
  }
  for (uint32_t v = 0; v < nvars; v++) {
    component[v] = qsop_residual_var_active(residual, v) ? 0U : UINT32_MAX;
  }
  const bool ok = build_residual_subinstance(residual, component, 0, sub, error);
  free(component);
  return ok;
}

typedef struct branch_materialized_reduction {
  qsop_instance_t reduced;
  uint32_t doublings;
  uint32_t degree2_merges;
  bool changed;
  bool zero;
} branch_materialized_reduction_t;

/* Unlike build_active_residual_subinstance, this copy is an algebraic replacement for the
 * residual rather than a delegate input: it therefore carries the residual constant.  norm_h is
 * artificial bookkeeping used only to count how many raw-amplitude doublings the root
 * simplifier performs. */
static bool build_active_residual_for_simplify(const qsop_residual_t *residual,
                                               qsop_instance_t *out, qsop_error_t *error) {
  if (!build_active_residual_subinstance(residual, out, error)) {
    return false;
  }
  out->constant = qsop_residual_constant(residual);
  const uint64_t artificial_norm_h = qsop_saturating_mul_u64((uint64_t)out->nvars, 2U);
  if (artificial_norm_h == UINT64_MAX) {
    free_subinstance(out);
    qsop_set_error(error, "active residual is too large for artificial Hadamard normalization");
    return false;
  }
  out->norm_h = artificial_norm_h;
  return true;
}

static bool branch_materialize_reduction(const qsop_residual_t *residual, bool simplify,
                                         branch_materialized_reduction_t *out,
                                         qsop_solve_stats_t *stats, qsop_error_t *error) {
  *out = (branch_materialized_reduction_t){0};
  if (!build_active_residual_for_simplify(residual, &out->reduced, error)) {
    return false;
  }
  if (!simplify) {
    return true;
  }

  const uint64_t start_norm_h = out->reduced.norm_h;
  const uint64_t start_ns = qsop_trace_now_ns();
  qsop_hadamard_simplify_stats_t simplify_stats = {0};
  /* [HH]-only: this runs inside the single-Fourier recursion at an arbitrary odd target mode, where
   * the [omega] rule's phase fold is unsound (see omega_eligible), so every elimination spends an
   * even two doublings and norm_h_delta stays even. */
  if (!qsop_simplify_hadamard_hh_only_with_stats(&out->reduced, &simplify_stats)) {
    free_subinstance(&out->reduced);
    qsop_set_error(error, "out of memory during materialized Hadamard simplification");
    return false;
  }
  const uint64_t elapsed_ns = qsop_trace_elapsed_ns(start_ns);
  assert(start_norm_h >= out->reduced.norm_h);
  assert(((start_norm_h - out->reduced.norm_h) & 1U) == 0U);
  const uint64_t doublings = (start_norm_h - out->reduced.norm_h) / 2U;
  if (doublings > UINT32_MAX) {
    free_subinstance(&out->reduced);
    qsop_set_error(error, "materialized Hadamard doubling count exceeds uint32 range");
    return false;
  }
  out->doublings = (uint32_t)doublings;
  out->degree2_merges = simplify_stats.degree2_eliminations;
  out->zero = simplify_stats.zero_witness;
  out->changed = doublings != 0U || out->zero;
  assert(!out->changed || out->doublings > 0U || out->zero);
#ifndef NDEBUG
  if (out->zero) {
    assert(out->reduced.r >= 2U && (out->reduced.r & 1U) == 0U);
    assert(out->reduced.nvars == 1U && out->reduced.nedges == 0U);
    assert(out->reduced.constant == 0U && out->reduced.unary[0] == out->reduced.r / 2U);
  }
#endif
  if (stats != NULL) {
    const uint64_t eliminations = (uint64_t)simplify_stats.degree0_eliminations +
                                  simplify_stats.degree1_eliminations +
                                  simplify_stats.degree2_eliminations;
    stats->branch_materialized_calls++;
    qsop_add_saturating_u64(&stats->branch_materialized_eliminations, eliminations);
    qsop_add_saturating_u64(&stats->branch_materialized_degree2_merges,
                            simplify_stats.degree2_eliminations);
    qsop_add_saturating_u64(&stats->branch_materialized_reduction_ns, elapsed_ns);
  }
  return true;
}

static void merge_delegated_stats(branch_search_stats_t *stats, const qsop_solve_stats_t *delegated,
                                  uint32_t delegated_nvars) {
  qsop_add_saturating_u64(&stats->leaves, assignment_count(delegated_nvars));
  qsop_add_saturating_u64(&stats->treewidth_delegations, delegated->treewidth_delegations);
  qsop_add_saturating_u64(&stats->rankwidth_delegations, delegated->rankwidth_delegations);
  qsop_add_saturating_u64(&stats->table_entries, delegated->table_entries);
  qsop_add_saturating_u64(&stats->signature_entries, delegated->signature_entries);
  qsop_add_saturating_u64(&stats->join_pairs, delegated->join_pairs);
  qsop_add_saturating_u64(&stats->join_signature_pairs, delegated->join_signature_pairs);
  qsop_add_saturating_u64(&stats->branch_fallthroughs, delegated->branch_fallthroughs);
  qsop_add_saturating_u64(&stats->branch_treewidth_skips, delegated->branch_treewidth_skips);
  qsop_add_saturating_u64(&stats->branch_rankwidth_skips, delegated->branch_rankwidth_skips);
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

/* ---------------------------------------------------------------------------
 * Unified delegation cost model.
 *
 * One inequality, `rw_est * rw_min_speedup < tw_est`, evaluated twice: first with a *predicted*
 * rankwidth cost that includes the price of the probe, to decide whether probing is worth it at
 * all; then with the measured cost after the probe, where the probe is sunk. Everything the model
 * used to express as a separate veto -- treewidth obviously cheap, treewidth narrow enough to
 * prefer, low cut-rank bypass, prefix-proxy rejection, the two forecast comparisons, and the
 * probe-cost check -- falls out of those two evaluations.
 *
 * Two things had to be true for that to work.
 *
 * tw_est uses the *real* DP work, sum over elimination steps of 2^(bag size), not the
 * nvars * 2^(width+1) bound. That bound assumes every bag is as wide as the widest one and
 * overstates circuit graphs by 275x to 605x while being within 2x on the small dense graphs where
 * rankwidth wins -- exactly backwards for this decision.
 *
 * rw_est includes rankwidth_probe_estimate_ns before the probe. Deciding is not free: generating a
 * rank decomposition and measuring the cut rank at each of its ~2*nvars nodes is O(nvars^2 * words)
 * of bitset work. On a 14k-variable, width-16 instance that was over 100s spent to improve on a 3s
 * treewidth solve, and nothing in the model accounted for it.
 * --------------------------------------------------------------------------- */

/* Nanoseconds for the treewidth solve. UINT64_MAX when treewidth cannot run at all, which makes
 * rankwidth win every comparison below -- it is the only backend left. */
static uint64_t branch_treewidth_estimate_ns(const qsop_branch_policy_t *pol, bool usable,
                                             uint64_t tw_dp_work) {
  if (!usable) {
    return UINT64_MAX;
  }
  uint64_t est = qsop_saturating_mul_u64(pol->C_tw_table, tw_dp_work);
  qsop_add_saturating_u64(&est, pol->tw_fixed_overhead_ns);
  return est;
}

/* Nanoseconds for the rankwidth solve. probe_ns is the cost of the decomposition probe: charged
 * before the probe has run, zero afterwards. */
static uint64_t branch_rankwidth_estimate_ns(const qsop_branch_policy_t *pol, uint64_t rw_table,
                                             uint64_t rw_join, uint64_t rw_sig, uint64_t probe_ns) {
  uint64_t est = qsop_saturating_mul_u64(pol->C_rw_table, rw_table);
  qsop_add_saturating_u64(&est, qsop_saturating_mul_u64(pol->C_rw_join, rw_join));
  qsop_add_saturating_u64(&est, qsop_saturating_mul_u64(pol->C_rw_sig, rw_sig));
  qsop_add_saturating_u64(&est, pol->rw_fixed_overhead_ns);
  qsop_add_saturating_u64(&est, pol->rw_memory_penalty_ns);
  qsop_add_saturating_u64(&est, probe_ns);
  return est;
}

static bool branch_rankwidth_wins(const qsop_branch_policy_t *pol, uint64_t rw_est,
                                  uint64_t tw_est) {
  if (tw_est == UINT64_MAX) {
    return true;
  }
  return tw_est != 0 && (double)rw_est * pol->rw_min_speedup < (double)tw_est;
}

/* Predicted cost of *deciding* whether rankwidth wins: generate a rank decomposition, then measure
 * the cut rank at each of its ~2*nvars nodes, each a GF(2) rank over nvars-bit rows. That is
 * O(nvars^2 * words) bitset work, superlinear in the component size and with a large constant. */
static uint64_t rankwidth_probe_estimate_ns(const qsop_branch_policy_t *pol, uint32_t nvars) {
  const uint64_t words = ((uint64_t)nvars + 63U) / 64U;
  uint64_t units = qsop_saturating_mul_u64((uint64_t)nvars, (uint64_t)nvars);
  units = qsop_saturating_mul_u64(units, words == 0 ? 1U : words);
  return qsop_saturating_mul_u64(units, pol->C_rw_probe);
}

/* Is the probe worth running? Natural-order prefix cut-rank is not a lower bound on the width of a
 * generated decomposition: a different order can compress a large prefix rank all the way down to
 * one. Use the minimum feasible generated width -- zero only when the graph's prefix rank is zero,
 * otherwise one -- and omit the join term we cannot know yet. A probe that looks promising under
 * this deliberately best-case estimate is settled honestly by the measured second evaluation,
 * while one that cannot win even in the best case is pure loss. `table_r_factor` is r for the
 * count-table path (whose tables carry a residue axis) and 1 for single-Fourier. */
static bool branch_should_probe_rankwidth(const qsop_branch_policy_t *pol, uint64_t tw_est,
                                          uint32_t nvars, uint32_t prefix_cut_rank,
                                          uint64_t table_r_factor) {
  const uint32_t optimistic_width = prefix_cut_rank == 0 ? 0U : 1U;
  const uint64_t rw_sig = binary_assignment_forecast(optimistic_width);
  const uint64_t rw_table = qsop_saturating_mul_u64(rw_sig, table_r_factor);
  const uint64_t rw_est = branch_rankwidth_estimate_ns(pol, rw_table, 0, rw_sig,
                                                       rankwidth_probe_estimate_ns(pol, nvars));
  return branch_rankwidth_wins(pol, rw_est, tw_est);
}

/* Public CLI helper for the single-Fourier auto path: true when the unified pre-probe check says a
 * rankwidth probe cannot pay for itself, so the direct whole-instance treewidth path is safe to
 * take. Prefix cut-rank distinguishes only rank zero from a potentially compressible graph; it is
 * not used as an estimate of the generated width. When false, the caller falls into the branch
 * recursion, where rankwidth is probed and the same model decides. Mirrors
 * branch_should_probe_rankwidth exactly -- callers only reach here with treewidth inside its
 * single-Fourier cap, so treewidth is usable and its tables carry no residue axis (r factor 1). */
bool qsop_branch_single_treewidth_clearly_preferred(uint32_t prefix_cut_rank, uint32_t nvars,
                                                    uint64_t treewidth_dp_work,
                                                    const qsop_branch_policy_t *policy) {
  const qsop_branch_policy_t pol = branch_policy_normalize(policy);
  const uint64_t tw_est = branch_treewidth_estimate_ns(&pol, true, treewidth_dp_work);
  return !branch_should_probe_rankwidth(&pol, tw_est, nvars, prefix_cut_rank, 1U);
}

/* Maximum cutrank width tried during calibration runs. Wider sub-problems are
   skipped to bound calibration cost even when policy vetoqs are bypassed. */
#define BRANCH_CALIBRATION_MAX_WIDTH 20U

/* BOTH policy: also generate the native (min-fill-cut) decomposition and keep whichever forecasts
 * the smaller table (ties broken by fewer join pairs). A no-op unless rw_source is BOTH and the
 * primary decomposition came from the from-treewidth generator. On success it may replace
 * *decomposition and update the forecasts in place; native-generation failure is silently ignored
 * (the primary decomposition is kept, and the caller's error stays untouched). */
static void branch_rankwidth_maybe_prefer_native(
    qsop_instance_t *sub, qsop_rankwidth_generator_t primary_gen, uint32_t cutrank_width,
    branch_search_stats_t *stats, qsop_rankwidth_decomposition_t **decomposition,
    uint64_t *table_forecast, uint64_t *join_forecast) {
  if (stats->rw_source != QSOP_BRANCH_RW_SOURCE_BOTH ||
      primary_gen != QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH) {
    return;
  }
  qsop_rankwidth_decomposition_t *native_dec = NULL;
  qsop_error_t native_error = {0};
  if (!qsop_rankwidth_decomposition_generate(sub, QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT, &native_dec,
                                             &native_error)) {
    return;
  }
  uint64_t native_table = qsop_saturating_mul_u64(binary_assignment_forecast(cutrank_width), sub->r);
  uint64_t native_join = 0;
  if (qsop_rankwidth_decomposition_forecast(sub, native_dec, &native_table, &native_join, NULL) &&
      (native_table < *table_forecast ||
       (native_table == *table_forecast && native_join < *join_forecast))) {
    qsop_rankwidth_decomposition_free(*decomposition);
    *decomposition = native_dec;
    native_dec = NULL;
    *table_forecast = native_table;
    *join_forecast = native_join;
    branch_trace_event(stats, "branch.rankwidth_source_native_preferred", native_table);
  }
  qsop_rankwidth_decomposition_free(native_dec);
}

/* precomputed_order: if non-NULL, used instead of running min-fill inside the from-treewidth
 * generator (D2 optimization: share one min-fill run with the treewidth solver path). */
static bool branch_try_rankwidth_delegate(
    qsop_instance_t *sub, uint64_t *counts, uint32_t treewidth_width, uint32_t prefix_cut_rank,
    uint64_t treewidth_dp_work, bool treewidth_available, uint32_t constant_shift,
    const uint32_t *precomputed_order, branch_search_stats_t *stats,
    branch_rw_decision_data_t *rw_data, bool *out_delegated, qsop_error_t *error) {
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
  /* In calibration mode, bypass the cost inequality so both backends get timed. */
  const bool calibrating = (stats->sink != NULL && stats->sink->calibrate_backends);
  /* Whether rankwidth's counts will be adopted. Cleared when a veto fires in calibration mode:
   * rankwidth is still generated, solved and timed (both backends must be), but the result is
   * discarded and the caller proceeds with treewidth. */
  bool adopt = true;
  /* Diagnostic only: the peak-2^(width+1) table/join forecasts are emitted for the trace but no
   * longer feed the decision, which prices treewidth by its real min-fill DP work (tw_est below). */
  if (treewidth_available) {
    branch_trace_event(stats, "branch.treewidth_table_forecast",
                       treewidth_table_forecast(treewidth_width, (uint32_t)sub->r));
    branch_trace_event(stats, "branch.treewidth_join_pair_forecast",
                       treewidth_join_pair_forecast(treewidth_width, sub->nvars));
  }

  /* Policy fields are pre-normalized in branch_policy_normalize(); read directly. */
  const qsop_branch_policy_t *pol = &stats->policy;

  /* The treewidth solve this decision is trying to beat. Its tables carry a residue axis, hence
   * the r factor on the DP work. */
  const bool treewidth_usable =
      treewidth_available && treewidth_width <= BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH;
  const uint64_t tw_est = branch_treewidth_estimate_ns(
      pol, treewidth_usable, qsop_saturating_mul_u64(treewidth_dp_work, (uint64_t)sub->r));

  if (!branch_should_probe_rankwidth(pol, tw_est, sub->nvars, prefix_cut_rank, sub->r)) {
    note_rankwidth_skip(stats, "branch.rankwidth_skip_predicted_cost", sub->nvars);
    if (rw_data != NULL) {
      rw_data->veto_reason = "rw_predicted_cost";
      rw_data->attempted = calibrating;
    }
    if (!calibrating) {
      return true;
    }
    /* Calibration: probe and time rankwidth anyway, but do not adopt its result. */
    adopt = false;
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
      qsop_saturating_mul_u64(binary_assignment_forecast(cutrank_width), sub->r);
  uint64_t rankwidth_join_pair_forecast = 0;
  if (!qsop_rankwidth_decomposition_forecast(sub, decomposition, &rankwidth_table_forecast,
                                             &rankwidth_join_pair_forecast, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  const uint64_t generation_ns = qsop_trace_elapsed_ns(generate_start_ns);
  branch_trace_event(stats, "branch.rankwidth_table_forecast", rankwidth_table_forecast);
  branch_trace_event(stats, "branch.rankwidth_join_pair_forecast", rankwidth_join_pair_forecast);

  branch_rankwidth_maybe_prefer_native(sub, primary_gen, cutrank_width, stats, &decomposition,
                                       &rankwidth_table_forecast, &rankwidth_join_pair_forecast);

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

  /* Second and final evaluation of the cost model, probe now sunk (rw_probe drops to 0). Rankwidth
   * delegates iff it stays within its hard cut-rank memory cap AND wins the inequality; calibration
   * mode bypasses the inequality (both backends must be timed) but never the memory cap. */
  const uint64_t sig_est = binary_assignment_forecast(cutrank_width);
  const uint64_t rw_est = branch_rankwidth_estimate_ns(pol, rankwidth_table_forecast,
                                                       rankwidth_join_pair_forecast, sig_est, 0);
  const bool within_cutrank_cap = cutrank_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH;
  const bool rankwidth_wins =
      within_cutrank_cap && (calibrating || branch_rankwidth_wins(pol, rw_est, tw_est));

  if (!rankwidth_wins) {
    const bool over_cap = !within_cutrank_cap;
    note_rankwidth_skip(
        stats, over_cap ? "branch.rankwidth_skip_width" : "branch.rankwidth_skip_cost_model",
        cutrank_width);
    if (rw_data != NULL) {
      rw_data->veto_reason = over_cap ? "rw_width_above_cap" : "rw_cost_model_rejected";
    }
    if (!calibrating || cutrank_width > BRANCH_CALIBRATION_MAX_WIDTH) {
      qsop_rankwidth_decomposition_free(decomposition);
      return true;
    }
    /* Calibration: fall through to time rankwidth without adopting its counts. */
    adopt = false;
  }

  uint64_t *part_counts = NULL;
  qsop_result_t *part_result = NULL;
  qsop_solve_stats_t delegated = {0};
  const uint64_t solve_start_ns = qsop_trace_now_ns();
  const uint64_t solve_start = qsop_trace_begin(stats->trace);
  const bool use_fourier = stats->mode == QSOP_SOLVE_MODE_FOURIER && stats->count_modulus == 0;
  if (!adopt && use_fourier) {
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
  if (!adopt) {
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
      qsop_set_error(error, "out of memory for stats order buffer");
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
                                      &shared_order_width, NULL, error)) {
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
  if (!branch_try_rankwidth_delegate(&sub, counts, sub_stats.min_fill_width,
                                     sub_stats.prefix_cut_rank, sub_stats.min_fill_dp_work,
                                     sub_stats.width_diagnostics_available,
                                     (uint32_t)qsop_residual_constant(residual), shared_order,
                                     stats, rw_ptr, &delegated, error)) {
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
                                        &tw_cal_width, NULL, &cal_err)) {
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
                                      &order_width, NULL, error)) {
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
          uint64_t cal_fe = qsop_saturating_mul_u64(binary_assignment_forecast(cal_width), sub.r);
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
    stats->cache_stored_residue_slots = qsop_saturating_mul_u64((uint64_t)search.cache.len, qsop->r);
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
    qsop_set_error(error, "out of memory while splitting residual components");
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
    qsop_set_error(error, "residual active-var count disagrees with active flags");
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
  qsop_add_saturating_u64(&stats->leaves, leaves);
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
    qsop_set_error(error,
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
      qsop_add_saturating_u64(&stats->cache_avoided_nodes, entry->search_nodes - 1U);
    }
    const uint32_t hit_r = (uint32_t)qsop_residual_modulus(residual);
    return branch_counts_shift_add(hit_r, counts, entry->counts,
                                   (uint32_t)(qsop_residual_constant(residual) % hit_r), stats,
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
    qsop_set_error(error, "branch CRT count table is too large");
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
    qsop_set_error(error, "out of memory while allocating branch CRT solve state");
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
    qsop_set_error(error, "out of memory while allocating branch CRT result strings");
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
    qsop_set_error(error, "out of memory while counting support components");
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
                                                const qsop_branch_policy_t *policy,
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
                                  NULL, error)) {
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
  /* Treewidth is within delegation range, but rankwidth may still win massively (e.g. uniform
   * K_{a,b}, treewidth a, cut rank 1). Ask the same question the recursion asks -- is a rankwidth
   * probe worth its own cost? -- rather than a separate pair of thresholds. Checked after the width
   * gate so roots outside the admitted cap skip this stats call entirely. */
  qsop_stats_t root_stats = {0};
  qsop_error_t stats_err = {0};
  if (qsop_compute_stats(qsop, &root_stats, &stats_err) && root_stats.width_diagnostics_available) {
    const uint64_t tw_est = branch_treewidth_estimate_ns(
        policy, true, qsop_saturating_mul_u64(root_stats.min_fill_dp_work, qsop->r));
    if (branch_should_probe_rankwidth(policy, tw_est, qsop->nvars, root_stats.prefix_cut_rank,
                                      qsop->r)) {
      free(order);
      return true; /* not handled: let the main recursion probe rankwidth */
    }
  }
  qsop_trace_emit(trace, "branch.treewidth_table_forecast", 0,
                  treewidth_table_forecast(width, (uint32_t)qsop->r), 0);
  qsop_trace_emit(trace, "branch.treewidth_join_pair_forecast", 0,
                  treewidth_join_pair_forecast(width, qsop->nvars), 0);
  qsop_trace_emit(trace, "branch.rankwidth_skip_predicted_cost", 0, width, 0);

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
            uint64_t cal_fe = qsop_saturating_mul_u64(binary_assignment_forecast(cal_width), qsop->r);
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
    qsop_set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL) {
    qsop_set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->r > UINT32_MAX) {
    qsop_set_error(error, "count-table/all-modes branch backend requires R <= UINT32_MAX; use "
                     "--solve-mode single-fourier");
    return false;
  }
  const qsop_branch_policy_t policy = branch_policy_normalize(&o.policy);
  if (o.mode != QSOP_SOLVE_MODE_COUNT_TABLE && o.mode != QSOP_SOLVE_MODE_FOURIER) {
    qsop_set_error(error, "internal error: unsupported residual branch solve mode");
    return false;
  }
  bool root_handled = false;
  const bool tried_root_treewidth_before_sanity =
      max_vars > 0U && qsop->nvars <= BRANCH_ROOT_TREEWIDTH_WIDE_MAX_VARS;
  if (tried_root_treewidth_before_sanity) {
    if (!branch_try_root_treewidth_fast_path(qsop, out, stats, o.trace, o.mode, &policy, o.sink,
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
    if (stats != NULL) {
      stats->termination_reason = QSOP_SOLVE_TERMINATION_OTHER_ERROR;
      stats->failure_active_vars = qsop->nvars;
      stats->failure_active_edges = qsop->nedges;
    }
    qsop_set_error(error,
              "residual branch solver refuses %" PRIu32
              " variables outright (exceeds %ux --max-vars); pass a larger --max-vars or use a "
              "future backend",
              qsop->nvars, BRANCH_ROOT_SANITY_MULTIPLIER);
    return false;
  }
  if (!tried_root_treewidth_before_sanity) {
    if (!branch_try_root_treewidth_fast_path(qsop, out, stats, o.trace, o.mode, &policy, o.sink,
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
    qsop_set_error(error, "out of memory while allocating result");
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
/* The value is (re + i*im) * 2^exp. The recursion multiplies one amplitude per connected component
 * and one factor of 2 per propagated variable, so the product reaches 2^nvars and blows past even
 * long double's ~2^16384 on the larger gauntlet instances. Carrying the exponent separately, and
 * pulling the mantissa back to [1,2) after every operation, removes the ceiling. Scaling by a power
 * of two is exact, so no mantissa bit is lost doing it. */
typedef struct branch_c64 {
  long double re;
  long double im;
  int exp;
} branch_c64_t;

/* Long double keeps 64 mantissa bits, so a summand more than that many binary orders below the
 * other cannot change it. Aligning further would just flush it to zero anyway. */
#define BRANCH_C64_ALIGN_LIMIT 72

static inline branch_c64_t c64_zero(void) {
  return (branch_c64_t){0.0L, 0.0L, 0};
}

static inline branch_c64_t c64_one(void) {
  return (branch_c64_t){1.0L, 0.0L, 0};
}

static inline bool c64_is_zero(branch_c64_t v) {
  return v.re == 0.0L && v.im == 0.0L;
}

static inline branch_c64_t c64_normalize(branch_c64_t v) {
  const long double re = fabsl(v.re);
  const long double im = fabsl(v.im);
  const long double peak = re > im ? re : im;
  if (peak == 0.0L || !isfinite(peak)) {
    return v;
  }
  const int e = ilogbl(peak);
  if (e == 0) {
    return v;
  }
  const long double scale = ldexpl(1.0L, -e);
  return (branch_c64_t){v.re * scale, v.im * scale, v.exp + e};
}

static inline branch_c64_t c64_add(branch_c64_t a, branch_c64_t b) {
  if (c64_is_zero(a)) {
    return b;
  }
  if (c64_is_zero(b)) {
    return a;
  }
  if (a.exp - b.exp > BRANCH_C64_ALIGN_LIMIT) {
    return a;
  }
  if (b.exp - a.exp > BRANCH_C64_ALIGN_LIMIT) {
    return b;
  }
  const int e = a.exp > b.exp ? a.exp : b.exp;
  const long double sa = ldexpl(1.0L, a.exp - e);
  const long double sb = ldexpl(1.0L, b.exp - e);
  return c64_normalize((branch_c64_t){a.re * sa + b.re * sb, a.im * sa + b.im * sb, e});
}

static inline branch_c64_t c64_mul(branch_c64_t a, branch_c64_t b) {
  return c64_normalize(
      (branch_c64_t){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re, a.exp + b.exp});
}

/* For a unit-modulus v -- which every omega^k phase is -- the conjugate is the exact inverse, so
 * this divides a residual's constant phase back out without a single rounding step beyond the
 * multiply itself. */
static inline branch_c64_t c64_conj(branch_c64_t v) {
  return (branch_c64_t){v.re, -v.im, v.exp};
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

typedef enum branch_delegate_miss {
  BRANCH_DELEGATE_MISS_NONE,
  BRANCH_DELEGATE_MISS_TW_WIDTH,
  BRANCH_DELEGATE_MISS_TW_WORK,
  BRANCH_DELEGATE_MISS_RW_WIDTH,
  BRANCH_DELEGATE_MISS_COST,
  BRANCH_DELEGATE_MISS_MEMORY,
} branch_delegate_miss_t;

typedef struct branch_cutset_frame {
  uint32_t depth;
  uint32_t levels_since_delegate_probe;
  uint32_t vars_at_last_delegate_probe;
  uint32_t edges_at_last_delegate_probe;
  uint32_t stagnant_levels;
} branch_cutset_frame_t;

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
  uint32_t treewidth_delegate_max_width;
  uint32_t rankwidth_delegate_max_width;
  uint64_t treewidth_delegate_max_dp_work;
  uint64_t cutset_treewidth_delegate_max_dp_work; /* 0 = reuse treewidth_delegate_max_dp_work */
  uint64_t treewidth_delegate_max_memory_mib;     /* 0 = built-in 12 GiB budget */
  bool materialized_reduction;
  bool hadamard_reduction_exact;
  bool diagnose_conditioning;
  uint32_t max_cutset_depth;
  uint32_t lookahead_candidates;
  uint64_t max_conditioning_nodes;
  uint32_t delegate_reprobe_interval;
  uint32_t max_stagnant_levels;
  /* Resolved once at init: qsop_residual_propagate is exact only for an even modulus and an odd
   * target mode (an even mode kills the sign edges outright and the rule changes shape). */
  bool propagate;
  qsop_branch_shadow_mode_t shadow_mode;

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
  uint64_t conditioning_nodes;
  uint32_t depth;
  branch_delegate_miss_t last_delegate_miss;
  bool conditioning_diagnosed;
  long double numeric_error_bound;

  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
  qsop_backend_stats_sink_t *sink;
} branch_single_mode_state_t;

typedef struct branch_cutset_child {
  qsop_instance_t reduced;
  uint32_t doublings;
  uint32_t degree2_merges;
  uint32_t active_vars;
  uint32_t active_edges;
  uint32_t components;
  uint32_t largest_component;
  uint32_t extra_reductions;
  bool zero;
} branch_cutset_child_t;

typedef struct branch_cutset_candidate {
  uint32_t var;
  uint32_t unlock3;
  uint32_t unlock4;
  uint32_t degree;
  bool has_unary;
  branch_cutset_child_t child[2];
} branch_cutset_candidate_t;

static void branch_cutset_child_free(branch_cutset_child_t *child) {
  if (child == NULL) {
    return;
  }
  free_subinstance(&child->reduced);
  *child = (branch_cutset_child_t){0};
}

static void branch_cutset_candidate_free(branch_cutset_candidate_t *candidate) {
  if (candidate == NULL) {
    return;
  }
  branch_cutset_child_free(&candidate->child[0]);
  branch_cutset_child_free(&candidate->child[1]);
  *candidate = (branch_cutset_candidate_t){0};
}

static bool branch_shortlist_better(const branch_cutset_candidate_t *a,
                                    const branch_cutset_candidate_t *b) {
  if (a->unlock3 != b->unlock3) {
    return a->unlock3 > b->unlock3;
  }
  if (a->unlock4 != b->unlock4) {
    return a->unlock4 > b->unlock4;
  }
  if (a->degree != b->degree) {
    return a->degree > b->degree;
  }
  if (a->has_unary != b->has_unary) {
    return a->has_unary;
  }
  return a->var < b->var;
}

/* O(n+m): active degrees are maintained by the residual, and each active edge contributes at
 * most one unlock count to each endpoint. */
static bool branch_cutset_shortlist(const qsop_residual_t *residual, uint32_t limit,
                                    branch_cutset_candidate_t **out, uint32_t *out_len,
                                    qsop_error_t *error) {
  *out = NULL;
  *out_len = 0;
  const uint32_t nvars = qsop_residual_nvars(residual);
  if (limit == 0U || nvars == 0U) {
    return true;
  }
  uint32_t *unlock3 = calloc(nvars, sizeof(*unlock3));
  uint32_t *unlock4 = calloc(nvars, sizeof(*unlock4));
  branch_cutset_candidate_t *top = calloc(limit, sizeof(*top));
  if (unlock3 == NULL || unlock4 == NULL || top == NULL) {
    free(unlock3);
    free(unlock4);
    free(top);
    qsop_set_error(error, "out of memory while building cutset candidate shortlist");
    return false;
  }
  const uint64_t r = qsop_residual_modulus(residual);
  const uint64_t sign = (r & 1U) == 0U ? r / 2U : UINT64_MAX;
  for (uint32_t e = 0; e < qsop_residual_nedges(residual); e++) {
    if (!qsop_residual_edge_active(residual, e)) {
      continue;
    }
    const uint32_t u = qsop_residual_edge_u(residual, e);
    const uint32_t v = qsop_residual_edge_v(residual, e);
    const uint64_t uu = qsop_residual_unary(residual, u) % r;
    const uint64_t vu = qsop_residual_unary(residual, v) % r;
    const uint32_t du = qsop_residual_active_degree(residual, u);
    const uint32_t dv = qsop_residual_active_degree(residual, v);
    if ((vu == 0U || vu == sign) && dv == 3U) {
      unlock3[u]++;
    }
    if ((uu == 0U || uu == sign) && du == 3U) {
      unlock3[v]++;
    }
    if ((vu == 0U || vu == sign) && dv == 4U) {
      unlock4[u]++;
    }
    if ((uu == 0U || uu == sign) && du == 4U) {
      unlock4[v]++;
    }
  }

  uint32_t len = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if (!qsop_residual_var_active(residual, v) || qsop_residual_active_degree(residual, v) == 0U) {
      continue;
    }
    const branch_cutset_candidate_t candidate = {
        .var = v,
        .unlock3 = unlock3[v],
        .unlock4 = unlock4[v],
        .degree = qsop_residual_active_degree(residual, v),
        .has_unary = qsop_residual_unary(residual, v) != 0U,
    };
    uint32_t pos = len;
    if (len < limit) {
      len++;
    } else if (!branch_shortlist_better(&candidate, &top[len - 1U])) {
      continue;
    } else {
      pos = len - 1U;
    }
    top[pos] = candidate;
    while (pos > 0U && branch_shortlist_better(&top[pos], &top[pos - 1U])) {
      const branch_cutset_candidate_t tmp = top[pos - 1U];
      top[pos - 1U] = top[pos];
      top[pos] = tmp;
      pos--;
    }
  }
  free(unlock3);
  free(unlock4);
  if (len == 0U) {
    free(top);
    qsop_set_error(error, "residual active-var count disagrees with cutset shortlist");
    return false;
  }
  *out = top;
  *out_len = len;
  return true;
}

static bool branch_measure_instance_shape(const qsop_instance_t *inst, uint32_t *components,
                                          uint32_t *largest, qsop_error_t *error) {
  *components = 0;
  *largest = 0;
  qsop_residual_t *residual = NULL;
  if (!qsop_residual_create(inst, &residual, error)) {
    return false;
  }
  uint32_t *labels = malloc((inst->nvars == 0U ? 1U : inst->nvars) * sizeof(*labels));
  uint32_t *sizes = calloc(inst->nvars == 0U ? 1U : inst->nvars, sizeof(*sizes));
  bool ok = labels != NULL && sizes != NULL;
  if (!ok) {
    qsop_set_error(error, "out of memory while measuring reduced cutset child");
  } else {
    ok = qsop_residual_active_components(residual, labels, components, error);
  }
  if (ok) {
    for (uint32_t v = 0; v < inst->nvars; v++) {
      if (labels[v] < *components) {
        sizes[labels[v]]++;
        if (sizes[labels[v]] > *largest) {
          *largest = sizes[labels[v]];
        }
      }
    }
  }
  free(labels);
  free(sizes);
  qsop_residual_free(residual);
  return ok;
}

static bool branch_cutset_score_better(const branch_cutset_candidate_t *a,
                                       const branch_cutset_candidate_t *b) {
  const uint32_t a_zero = (uint32_t)a->child[0].zero + (uint32_t)a->child[1].zero;
  const uint32_t b_zero = (uint32_t)b->child[0].zero + (uint32_t)b->child[1].zero;
  if (a_zero != b_zero) {
    return a_zero > b_zero;
  }
#define WORST(field, x) ((x)->child[0].field > (x)->child[1].field ? (x)->child[0].field          \
                                                                    : (x)->child[1].field)
  if (WORST(largest_component, a) != WORST(largest_component, b)) {
    return WORST(largest_component, a) < WORST(largest_component, b);
  }
  if (WORST(active_vars, a) != WORST(active_vars, b)) {
    return WORST(active_vars, a) < WORST(active_vars, b);
  }
  if (WORST(active_edges, a) != WORST(active_edges, b)) {
    return WORST(active_edges, a) < WORST(active_edges, b);
  }
#undef WORST
  const uint64_t a_extra = (uint64_t)a->child[0].extra_reductions + a->child[1].extra_reductions;
  const uint64_t b_extra = (uint64_t)b->child[0].extra_reductions + b->child[1].extra_reductions;
  if (a_extra != b_extra) {
    return a_extra > b_extra;
  }
  return a->var < b->var;
}

static void branch_emit_conditioning_record(const branch_single_mode_state_t *state,
                                            const branch_cutset_candidate_t *candidate,
                                            uint8_t value, uint32_t before_vars) {
  if (state->sink == NULL || state->sink->file == NULL) {
    return;
  }
  FILE *f = state->sink->file;
  const branch_cutset_child_t *child = &candidate->child[value];
  fputs("{\"schema\":\"sop_solve_conditioning_v1\",\"instance\":", f);
  branch_jsonl_write_string(f, state->sink->instance);
  fprintf(f,
          ",\"candidate_variable\":%" PRIu32 ",\"value\":%u"
          ",\"active_vars_before\":%" PRIu32 ",\"active_vars_after\":%" PRIu32
          ",\"active_edges_after\":%" PRIu32 ",\"component_count\":%" PRIu32
          ",\"largest_component\":%" PRIu32 ",\"doublings\":%" PRIu32
          ",\"degree2_merges\":%" PRIu32 ",\"exact_zero\":%s}\n",
          candidate->var, (unsigned)value, before_vars, child->active_vars, child->active_edges,
          child->components, child->largest_component, child->doublings, child->degree2_merges,
          child->zero ? "true" : "false");
  (void)fflush(f);
}

static bool branch_cutset_lookahead_child(qsop_residual_t *residual,
                                         branch_single_mode_state_t *state, uint32_t parent_vars,
                                         uint32_t var, uint8_t value, bool full_simplify,
                                         branch_cutset_child_t *out, qsop_error_t *error) {
  *out = (branch_cutset_child_t){0};
  const size_t checkpoint = qsop_residual_checkpoint(residual);
  bool ok = qsop_residual_branch(residual, var, value, error);
  uint32_t propagated = 0;
  bool propagated_zero = false;
  if (ok && state->propagate) {
    ok = qsop_residual_propagate(residual, &propagated, &propagated_zero, error);
    if (ok) {
      qsop_add_saturating_u64(&state->propagations, propagated);
    }
  }

  branch_materialized_reduction_t materialized = {0};
  if (ok) {
    ok = branch_materialize_reduction(residual, full_simplify, &materialized, state->stats, error);
  }
  if (ok) {
    const uint64_t doublings = (uint64_t)propagated + materialized.doublings;
    if (doublings > UINT32_MAX) {
      qsop_set_error(error, "cutset child doubling count exceeds uint32 range");
      ok = false;
    } else {
      out->reduced = materialized.reduced;
      materialized.reduced = (qsop_instance_t){0};
      out->doublings = (uint32_t)doublings;
      out->degree2_merges = materialized.degree2_merges;
      out->zero = propagated_zero || materialized.zero;
      out->active_vars = out->reduced.nvars;
      out->active_edges = out->reduced.nedges;
      out->extra_reductions = parent_vars > out->active_vars + 1U
                                  ? parent_vars - 1U - out->active_vars
                                  : 0U;
      ok = branch_measure_instance_shape(&out->reduced, &out->components,
                                         &out->largest_component, error);
    }
  }
  free_subinstance(&materialized.reduced);
  const bool undo_ok = qsop_residual_undo(residual, checkpoint, error);
  if (!ok || !undo_ok) {
    branch_cutset_child_free(out);
    return false;
  }
  if (state->stats != NULL) {
    state->stats->branch_conditioning_lookaheads++;
  }
  return true;
}

/* Evaluates one candidate variable's two real children (branch, propagate, materialized
 * reduction, shape measurement -- exactly today's per-candidate treatment, unchanged) and fills
 * *out. Shared by both the shadow-shortlist-driven and the legacy unlock3/unlock4-shortlist-
 * driven candidate loops in branch_choose_cutset_candidate below, so the shadow graph can only
 * ever change *which* variables get this expensive real-residual lookahead, never how a
 * candidate is scored once evaluated. */
static bool branch_evaluate_cutset_candidate_real(qsop_residual_t *residual,
                                                   branch_single_mode_state_t *state,
                                                   bool diagnostic_full_simplify,
                                                   uint32_t parent_vars, uint32_t var,
                                                   branch_cutset_candidate_t *out,
                                                   qsop_error_t *error) {
  *out = (branch_cutset_candidate_t){.var = var};
  for (uint8_t value = 0; value <= 1U; value++) {
    if (!branch_cutset_lookahead_child(residual, state, parent_vars, var, value,
                                       state->hadamard_reduction_exact &&
                                           (diagnostic_full_simplify || state->materialized_reduction),
                                       &out->child[value], error)) {
      branch_cutset_candidate_free(out);
      return false;
    }
    if (state->diagnose_conditioning && !state->conditioning_diagnosed) {
      branch_emit_conditioning_record(state, out, value, parent_vars);
    }
  }
  return true;
}

/* Evaluates `var` (unless already present in `seen[0..nseen)`, in which case it's a no-op) and,
 * if it scores better than the current *out, replaces it. `*winner_is_shadow` is only ever set
 * to `shadow`, never cleared, and only meaningful once `*have_best` is true. */
static bool branch_cutset_consider_candidate(qsop_residual_t *residual,
                                             branch_single_mode_state_t *state,
                                             bool diagnostic_full_simplify, uint32_t parent_vars,
                                             uint32_t var, bool shadow,
                                             branch_cutset_candidate_t *out, bool *have_best,
                                             bool *winner_is_shadow, qsop_error_t *error) {
  branch_cutset_candidate_t candidate = {0};
  if (!branch_evaluate_cutset_candidate_real(residual, state, diagnostic_full_simplify,
                                             parent_vars, var, &candidate, error)) {
    branch_cutset_candidate_free(out);
    return false;
  }
  if (!*have_best || branch_cutset_score_better(&candidate, out)) {
    branch_cutset_candidate_free(out);
    *out = candidate;
    candidate = (branch_cutset_candidate_t){0};
    *have_best = true;
    *winner_is_shadow = shadow;
  }
  branch_cutset_candidate_free(&candidate);
  return true;
}

/* Picks the best variable to condition on.
 *
 * branch_cutset_score_better is a one-step-greedy comparator: it scores a candidate by the
 * shape of its own two children, with no visibility into what a candidate sets up two or three
 * branches later. unlock3/unlock4 (legacy's shortlist heuristic) is tuned to exploit exactly
 * that: a nonzero count means some neighbour is one pin away from qualifying for the exact [HH]
 * materialized-reduction cascade, the dominant source of real one-step progress in this
 * recursion, so whenever that signal exists a legacy candidate reliably outscores a shadow
 * candidate chosen for its multi-step structural payoff -- shadow doesn't see coefficients at
 * all, by design. Two designs were tried and rejected empirically before this one:
 *
 *   - Shadow *replacing* the shortlist unconditionally regressed a real mqt2040
 *     qnn_indep_qiskit fixture (1430-variable component): legacy alone solved it in 3
 *     conditioning nodes, shadow-only stalled into the stagnant-level budget.
 *   - Shadow candidates *merged* into the same real-child comparison alongside legacy's, always,
 *     neutralized shadow's own benefit on a synthetic gadget-chain fixture built with no unary
 *     value in {0, r/2} anywhere (so [HH] can fire nowhere): a degree-2 gadget "orphan" and a
 *     genuine structural hub look locally identical to a one-step-greedy comparator once neither
 *     can trigger an exact elimination, so legacy's generic degree/ID fallback tiebreak won the
 *     comparison as often as shadow's structurally-informed pick did, at double the per-node
 *     evaluation cost for no gain.
 *
 * The distinguishing case is exactly "does legacy's shortlist have any unlock signal at all" --
 * sorted unlock3 desc, unlock4 desc, so entry 0 carries the strongest signal in the list. When it
 * does (real circuits, empirically, almost always), trust it alone, unchanged from before this
 * feature existed. When it doesn't (a gadget-chain-heavy component whose coefficients don't
 * align with any exact rule -- what this feature targets), let shadow's own remove-and-rereduce
 * scoring pick the shortlist instead, competing only against itself, so its multi-step judgement
 * isn't drowned out by a legacy fallback pick the one-step comparator can't fairly rank against
 * it either. Either way, the actual winner is still chosen by the same real branch + materialized
 * reduction + branch_cutset_score_better used throughout this file -- shadow only ever changes
 * which variables are worth that treatment, never the treatment itself. */
static bool branch_choose_cutset_candidate(qsop_residual_t *residual,
                                           branch_single_mode_state_t *state,
                                           bool diagnostic_full_simplify,
                                           branch_cutset_candidate_t *out, qsop_error_t *error) {
  const uint32_t parent_vars = qsop_residual_active_vars(residual);

  branch_cutset_candidate_t *shortlist = NULL;
  uint32_t shortlist_len = 0;
  if (!branch_cutset_shortlist(residual, state->lookahead_candidates, &shortlist, &shortlist_len,
                               error)) {
    return false;
  }
  const bool legacy_has_unlock_signal =
      shortlist_len > 0U && (shortlist[0].unlock3 > 0U || shortlist[0].unlock4 > 0U);

  bool try_shadow = !legacy_has_unlock_signal && state->shadow_mode == QSOP_BRANCH_SHADOW_ON;
  if (!legacy_has_unlock_signal && state->shadow_mode == QSOP_BRANCH_SHADOW_AUTO) {
    const uint32_t active_edges = qsop_residual_active_edges(residual);
    try_shadow = parent_vars >= BRANCH_SHADOW_AUTO_TRIGGER_MIN_VARS &&
                active_edges >= 2U * parent_vars;
  }

  *out = (branch_cutset_candidate_t){0};
  bool have_best = false;
  bool winner_is_shadow = false;
  bool ok = true;

  if (try_shadow) {
    uint32_t *shadow_vars = NULL;
    uint32_t shadow_len = 0;
    ok = branch_shadow_shortlist(residual, state->lookahead_candidates, &shadow_vars, &shadow_len,
                                 state->stats, error);
    for (uint32_t i = 0; ok && i < shadow_len; i++) {
      ok = branch_cutset_consider_candidate(residual, state, diagnostic_full_simplify, parent_vars,
                                            shadow_vars[i], true, out, &have_best, &winner_is_shadow,
                                            error);
    }
    free(shadow_vars);
  }

  /* Shadow disabled/not triggered, budget-skipped, empty reduced core, or (defensively) no
   * shadow candidate evaluated cleanly: fall back to legacy's shortlist, exactly as before this
   * feature existed. */
  for (uint32_t i = 0; ok && !have_best && i < shortlist_len; i++) {
    ok = branch_cutset_consider_candidate(residual, state, diagnostic_full_simplify, parent_vars,
                                          shortlist[i].var, false, out, &have_best,
                                          &winner_is_shadow, error);
  }
  free(shortlist);

  if (!ok) {
    branch_cutset_candidate_free(out);
    return false;
  }
  if (!have_best) {
    qsop_set_error(error, "cutset lookahead found no active candidate");
    return false;
  }
  if (winner_is_shadow && state->stats != NULL) {
    state->stats->branch_shadow_selected++;
  }
  return true;
}

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
    qsop_set_error(error, "branch single-Fourier phase cache is too large");
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
    qsop_set_error(error, "out of memory while allocating branch single-Fourier phase cache");
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
    return (branch_c64_t){1.0, 0.0, 0};
  case 1:
    return (branch_c64_t){0.0, 1.0, 0};
  case 2:
    return (branch_c64_t){-1.0, 0.0, 0};
  default:
    return (branch_c64_t){0.0, -1.0, 0};
  }
}

static branch_c64_t branch_phase_compute(uint64_t r, uint32_t target_mode, uint64_t residue) {
  residue %= r;
  if (residue == 0) {
    return c64_one();
  }
  if ((r & 1U) == 0 && residue == r / 2U) {
    return (target_mode & 1U) != 0 ? (branch_c64_t){-1.0, 0.0, 0} : c64_one();
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
  return (branch_c64_t){cos(angle), sin(angle), 0};
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
      return (branch_c64_t){cache->re[idx], cache->im[idx], 0};
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
  qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->active_var)));
  qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->active_edge)));
  qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->unary)));
  qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->edge_u)));
  qsop_add_saturating_u64(&bytes, qsop_saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->edge_v)));
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
      qsop_set_error(error, "branch single-Fourier amplitude cache is too large");
      return false;
    }
    new_cap *= 2U;
  }
  branch_amp_cache_entry_t *entries = realloc(cache->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    qsop_set_error(error, "out of memory while growing branch single-Fourier amplitude cache");
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
    qsop_set_error(error, "out of memory while allocating branch single-Fourier amplitude cache");
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
  qsop_add_saturating_u64(&cache->estimated_bytes, entry_bytes);
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
  /* Without these the branch backend reports 0/0 SIMD work for its delegates, so `simd_kernel:
   * avx2` in --format stats reads like a claim the kernels ran when nothing was measured. */
  qsop_add_saturating_u64(&stats->simd_vectorized_ops, delegated->simd_vectorized_ops);
  qsop_add_saturating_u64(&stats->simd_scalar_fallback_ops, delegated->simd_scalar_fallback_ops);
  qsop_add_saturating_u64(&stats->treewidth_factor_scope_tests,
                          delegated->treewidth_factor_scope_tests);
  qsop_add_saturating_u64(&stats->treewidth_factor_bucket_visits,
                          delegated->treewidth_factor_bucket_visits);
  qsop_add_saturating_u64(&stats->treewidth_factor_multiplications,
                          delegated->treewidth_factor_multiplications);
  qsop_add_saturating_u64(&stats->treewidth_factor_allocations,
                          delegated->treewidth_factor_allocations);
  qsop_add_saturating_u64(&stats->treewidth_factor_discovery_ns,
                          delegated->treewidth_factor_discovery_ns);
  qsop_add_saturating_u64(&stats->treewidth_numeric_join_ns,
                          delegated->treewidth_numeric_join_ns);
  qsop_add_saturating_u64(&stats->treewidth_sum_out_ns, delegated->treewidth_sum_out_ns);
  if (delegated->treewidth_peak_live_bytes > stats->treewidth_peak_live_bytes) {
    stats->treewidth_peak_live_bytes = delegated->treewidth_peak_live_bytes;
  }
  if (delegated->treewidth_pool_retained_bytes > stats->treewidth_pool_retained_bytes) {
    stats->treewidth_pool_retained_bytes = delegated->treewidth_pool_retained_bytes;
  }
  if (delegated->treewidth_largest_allocation_bytes >
      stats->treewidth_largest_allocation_bytes) {
    stats->treewidth_largest_allocation_bytes =
        delegated->treewidth_largest_allocation_bytes;
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
                                  NULL, error)) {
    return false;
  }
  *order = fresh;
  *order_width = width;
  *order_owned = true;
  return true;
}

/* Conservative forecast of the treewidth DP's peak live memory for a component of this width (see
 * BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_MEMORY_MIB). Saturates to UINT64_MAX once 2^width * 128 B
 * would overflow, so an absurd width always exceeds any finite budget. */
static uint64_t branch_treewidth_delegate_peak_bytes(uint32_t width) {
  if (width >= 57U) { /* 2^57 * 128 = 2^64 overflows uint64_t */
    return UINT64_MAX;
  }
  return (UINT64_C(1) << width) * BRANCH_SINGLE_TREEWIDTH_DELEGATE_PEAK_BYTES_PER_ENTRY;
}

static bool branch_single_mode_delegate_component(
    const qsop_instance_t *sub, uint32_t max_vars, uint32_t target_mode,
    const qsop_branch_single_mode_options_t *options, bool fail_on_refusal, bool *out_delegated,
    qsop_amplitude_t *out, qsop_solve_stats_t *io_stats, branch_delegate_miss_t *out_miss,
    qsop_error_t *error) {
  if (out_delegated != NULL) {
    *out_delegated = false;
  }
  if (out_miss != NULL) {
    *out_miss = BRANCH_DELEGATE_MISS_NONE;
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
    qsop_set_error(error,
              "branch single-fourier solver refuses a %" PRIu32
              "-variable component; pass a larger --max-vars",
              sub->nvars);
    return false;
  }

  const qsop_branch_policy_t policy = branch_policy_normalize(&options->policy);
  const qsop_branch_rw_source_t rw_source = options->rw_source;
  /* A caller wanting "no cap" naturally writes UINT32_MAX, and tw_cap + 1 is the bag-variable limit
   * handed to the DP -- which would wrap to zero and make it refuse every non-constant instance.
   * Saturate, so an absurd cap means "as wide as the DP will go" rather than "nothing at all". */
  const uint32_t tw_cap_requested = options->treewidth_delegate_max_width != 0
                                        ? options->treewidth_delegate_max_width
                                        : BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_WIDTH;
  const uint32_t tw_cap = tw_cap_requested == UINT32_MAX ? UINT32_MAX - 1U : tw_cap_requested;
  const uint32_t rw_cap = options->rankwidth_delegate_max_width != 0
                              ? options->rankwidth_delegate_max_width
                              : BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH;
  const uint64_t dp_work_budget = options->treewidth_delegate_max_dp_work != 0
                                      ? options->treewidth_delegate_max_dp_work
                                      : BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_DP_WORK;
  const uint64_t mem_budget_mib = options->treewidth_delegate_max_memory_mib != 0
                                      ? options->treewidth_delegate_max_memory_mib
                                      : BRANCH_SINGLE_TREEWIDTH_DELEGATE_MAX_MEMORY_MIB;
  const uint64_t mem_budget_bytes =
      qsop_saturating_mul_u64(mem_budget_mib, UINT64_C(1024) * UINT64_C(1024));

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
    qsop_set_error(error, "out of memory while allocating branch single-fourier stats order buffer");
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

  /* Admit the treewidth DP on cost, not raw width: the min-fill DP work must be within budget
   * (a dense width-25 component like realamprandom_25 blows past it and would time out), the
   * width within the memory-safety ceiling, and the forecast peak memory within budget so the DP
   * cannot fail its own allocation mid-run. Single-Fourier tables carry no residue axis, so the
   * r factor is 1. */
  const uint64_t treewidth_peak_bytes = branch_treewidth_delegate_peak_bytes(treewidth_width);
  const bool treewidth_mem_ok = treewidth_peak_bytes <= mem_budget_bytes;
  const bool treewidth_usable = treewidth_available && treewidth_width <= tw_cap &&
                                sub_stats.min_fill_dp_work <= dp_work_budget && treewidth_mem_ok;
  const uint64_t tw_est =
      branch_treewidth_estimate_ns(&policy, treewidth_usable, sub_stats.min_fill_dp_work);

  if (rw_source != QSOP_BRANCH_RW_SOURCE_NONE) {
    const bool probe_worth_it =
        branch_should_probe_rankwidth(&policy, tw_est, sub->nvars, prefix_cut_rank, 1U);
    if (!probe_worth_it && io_stats != NULL) {
      io_stats->branch_rankwidth_skips++;
    }

    if (probe_worth_it) {
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
          } else {
            /* Probe is sunk; same inequality as the pre-probe check, with the measured forecasts.
             * sig_est = 2^cutrank = rankwidth_table (single-Fourier tables omit the r factor). */
            rankwidth_table = binary_assignment_forecast(cutrank_width);
            const uint64_t rw_est = branch_rankwidth_estimate_ns(
                &policy, rankwidth_table, rankwidth_join, rankwidth_table, 0);
            use_rankwidth = cutrank_width <= rw_cap &&
                            branch_rankwidth_wins(&policy, rw_est, tw_est);
            if (io_stats != NULL) {
              if (cutrank_width > io_stats->rankwidth_cutrank_width) {
                io_stats->rankwidth_cutrank_width = cutrank_width;
              }
              if (rankwidth_table > io_stats->rankwidth_table_forecast) {
                io_stats->rankwidth_table_forecast = rankwidth_table;
              }
              if (rankwidth_join > io_stats->rankwidth_join_pair_forecast) {
                io_stats->rankwidth_join_pair_forecast = rankwidth_join;
              }
              if (!use_rankwidth) {
                io_stats->branch_rankwidth_skips++;
              }
            }
          }
        }
      }
    }
  }

  if (setup_ok && !use_rankwidth && treewidth_usable) {
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
  } else if (treewidth_usable && order_width <= tw_cap &&
             branch_treewidth_delegate_peak_bytes(order_width) <= mem_budget_bytes) {
    /* Re-check memory against the *resolved* order width: branch_single_mode_ensure_order can
     * return an order wider than the pre-probe estimate treewidth_usable was computed from, and a
     * wider order means a larger peak. Falling through here refuses (or uses rankwidth) gracefully
     * rather than letting the DP OOM. */
    if (options->precision == QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE) {
      ok = qsop_solve_treewidth_precomputed_order_single_mode(
          sub, tw_cap + 1U, order, order_width, target_mode, out, &delegated, options->trace,
          error);
    } else {
      ok = qsop_solve_treewidth_precomputed_order_single_mode_f64(
          sub, tw_cap + 1U, order, order_width, target_mode, options->simd, out, &delegated,
          options->trace, error);
    }
    delegated.treewidth_delegations++;
  } else if (decomposition != NULL && cutrank_width <= rw_cap) {
    /* order_width (resolved lazily by branch_single_mode_ensure_order, using
     * QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE) can come out wider than treewidth_width -- the
     * cheap pre-probe estimate from qsop_compute_stats_with_order's plain MIN_FILL pass -- for
     * nvars > 63 components, where the two elimination orders tiebreak differently.
     * treewidth_usable above was computed from the stale estimate, so the earlier cost-model
     * evaluation may have judged rankwidth not worth attempting against a treewidth cost that
     * turns out unachievable. Having already generated and paid for a valid rankwidth
     * decomposition, use it rather than refuse. (The DP-work side of the estimate has the same
     * estimate-vs-actual-order gap and is not re-verified here.) */
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
    if (out_miss != NULL) {
      if (!treewidth_available || treewidth_width > tw_cap) {
        *out_miss = decomposition != NULL && cutrank_width > rw_cap
                        ? BRANCH_DELEGATE_MISS_RW_WIDTH
                        : BRANCH_DELEGATE_MISS_TW_WIDTH;
      } else if (sub_stats.min_fill_dp_work > dp_work_budget) {
        *out_miss = decomposition != NULL && cutrank_width > rw_cap
                        ? BRANCH_DELEGATE_MISS_RW_WIDTH
                        : BRANCH_DELEGATE_MISS_TW_WORK;
      } else if (!treewidth_mem_ok) {
        *out_miss = decomposition != NULL && cutrank_width > rw_cap ? BRANCH_DELEGATE_MISS_RW_WIDTH
                                                                    : BRANCH_DELEGATE_MISS_MEMORY;
      } else {
        *out_miss = BRANCH_DELEGATE_MISS_COST;
      }
    }
    if (fail_on_refusal) {
      /* Keep this concise: qsop_error_t.message is a 256-byte buffer, and the "no delegate
       * available" suffix is asserted by tests/test_differential_backends.py. The memory-forecast
       * detail lives in the BRANCH_DELEGATE_MISS_MEMORY stat, not here. */
      qsop_set_error(error,
                "branch single-fourier: connected component (%" PRIu32
                " vars) has treewidth %" PRIu32 " (DP work %" PRIu64 ", budget %" PRIu64
                ") and rankwidth cutrank %" PRIu32
                "; neither is within its delegate cap (width <= %" PRIu32 " / %" PRIu32
                ") -- no delegate available",
                sub->nvars, treewidth_width, sub_stats.min_fill_dp_work, dp_work_budget,
                cutrank_width, tw_cap, rw_cap);
      ok = false;
      if (io_stats != NULL) {
        io_stats->termination_reason = QSOP_SOLVE_TERMINATION_NO_DELEGATE;
        io_stats->failure_active_vars = sub->nvars;
        io_stats->failure_active_edges = sub->nedges;
      }
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
                                       bool may_materialize, branch_cutset_frame_t frame,
                                       branch_c64_t *out, qsop_error_t *error);

static uint64_t branch_cache_budget_bytes(uint64_t mib) {
  return qsop_saturating_mul_u64(mib, UINT64_C(1024) * UINT64_C(1024));
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
      .treewidth_delegate_max_width = o.treewidth_delegate_max_width,
      .rankwidth_delegate_max_width = o.rankwidth_delegate_max_width,
      .treewidth_delegate_max_dp_work = o.treewidth_delegate_max_dp_work,
      .cutset_treewidth_delegate_max_dp_work = o.cutset_treewidth_delegate_max_dp_work,
      .treewidth_delegate_max_memory_mib = o.treewidth_delegate_max_memory_mib,
      .materialized_reduction = o.materialized_reduction && qsop->r >= 2U &&
                                (qsop->r % 2U) == 0U && (target_mode % 2U) == 1U,
      .hadamard_reduction_exact = qsop->r >= 2U && (qsop->r % 2U) == 0U &&
                                  (target_mode % 2U) == 1U,
      .diagnose_conditioning = o.diagnose_conditioning,
      .max_cutset_depth = o.max_cutset_depth,
      .lookahead_candidates = o.lookahead_candidates != 0
                                  ? o.lookahead_candidates
                                  : BRANCH_SINGLE_DEFAULT_LOOKAHEAD_CANDIDATES,
      .max_conditioning_nodes = o.max_conditioning_nodes != 0
                                    ? o.max_conditioning_nodes
                                    : BRANCH_SINGLE_DEFAULT_MAX_CONDITIONING_NODES,
      .delegate_reprobe_interval = o.delegate_reprobe_interval != 0
                                       ? o.delegate_reprobe_interval
                                       : BRANCH_SINGLE_DEFAULT_DELEGATE_REPROBE_INTERVAL,
      .max_stagnant_levels = o.max_stagnant_levels != 0
                                 ? o.max_stagnant_levels
                                 : BRANCH_SINGLE_DEFAULT_MAX_STAGNANT_LEVELS,
      /* The rule rests on omega^(r/2) = -1 raised to an odd power; an even target mode turns
       * (-1)^(t*(s+S)) into 1 and the constraint disappears, so refuse rather than mis-fold. */
      .propagate = o.propagate != QSOP_BRANCH_SINGLE_PROPAGATE_OFF && qsop->r >= 2U &&
                   (qsop->r % 2U) == 0U && (target_mode % 2U) == 1U,
      .shadow_mode = o.shadow_mode,
      .stats = stats,
      .trace = o.trace,
      .sink = o.sink,
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
  stats->branch_conditioning_nodes = state->conditioning_nodes;
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
    branch_c64_t factor = c64_zero();
    if (unary == 0) {
      factor = (branch_c64_t){1.0L, 0.0L, 1}; /* 2 = 1 * 2^1 */
    } else if ((r & 1U) == 0 && unary == r / 2U && (state->target_mode & 1U) != 0) {
      *out = c64_zero();
      qsop_add_saturating_u64(&state->leaves, assignment_count(qsop_residual_active_vars(residual)));
      qsop_trace_emit_elapsed(state->trace, "branch.single.edge_free", state->depth,
                              qsop_residual_active_vars(residual), edge_free_start);
      return true;
    } else {
      const branch_c64_t phase = branch_phase_lookup(&state->phase_cache, unary);
      factor = c64_normalize((branch_c64_t){1.0L + phase.re, phase.im, phase.exp});
    }
    z = c64_mul(z, factor);
    c64_accum_error(1, &state->numeric_error_bound);
  }
  *out = z;
  qsop_add_saturating_u64(&state->leaves, assignment_count(qsop_residual_active_vars(residual)));
  qsop_trace_emit_elapsed(state->trace, "branch.single.edge_free", state->depth,
                          qsop_residual_active_vars(residual), edge_free_start);
  return true;
}

static bool branch_solve_component_single_mode(const qsop_instance_t *sub,
                                               branch_single_mode_state_t *state,
                                               branch_cutset_frame_t frame, branch_c64_t *out,
                                               qsop_error_t *error) {
  qsop_residual_t *component_residual = NULL;
  if (!qsop_residual_create(sub, &component_residual, error)) {
    return false;
  }
  frame.levels_since_delegate_probe = 0U;
  frame.vars_at_last_delegate_probe = 0U;
  frame.edges_at_last_delegate_probe = 0U;
  state->depth++;
  const bool ok = branch_sum_rec_single_mode(component_residual, state, false, frame, out, error);
  state->depth--;
  qsop_residual_free(component_residual);
  return ok;
}

static bool branch_sum_components_single_mode(qsop_residual_t *residual,
                                              branch_single_mode_state_t *state,
                                              branch_cutset_frame_t frame, bool *out_split,
                                              branch_c64_t *out, qsop_error_t *error) {
  *out_split = false;
  const uint32_t nvars = qsop_residual_nvars(residual);
  uint32_t *component = malloc((nvars == 0 ? 1U : nvars) * sizeof(*component));
  if (component == NULL) {
    qsop_set_error(error, "out of memory while splitting branch single-Fourier residual components");
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
        !branch_solve_component_single_mode(&sub, state, frame, &part, error)) {
      free_subinstance(&sub);
      free(component);
      return false;
    }
    acc = c64_mul(acc, part);
    c64_accum_error(1, &state->numeric_error_bound);
    free_subinstance(&sub);
    /* The amplitude is the product over components, so one exactly-zero factor -- which the
     * propagation conflict rule produces often -- decides the whole node. Amplitudes are never
     * rounded to zero (c64_normalize leaves a zero alone and never underflows a nonzero), so this
     * only fires on an exact algebraic zero. */
    if (c64_is_zero(acc)) {
      break;
    }
  }
  free(component);
  *out = acc;
  *out_split = true;
  return true;
}

/* inside_cutset: true for a delegate probe reached from within cutset conditioning's own
 * recursion (frame.depth > 0 at the call site), as opposed to the root-level probe or one from
 * the ordinary (non-cutset) branching fallback. Selects
 * state->cutset_treewidth_delegate_max_dp_work when set, instead of the root-level
 * state->treewidth_delegate_max_dp_work -- see the option's doc comment in qsop_solve.h for why
 * these need to differ: a DP-work budget calibrated for the one root-level attempt is far too
 * permissive once cutset conditioning re-attempts delegation on dozens of sub-residuals. */
static bool branch_try_single_mode_delegate(qsop_residual_t *residual,
                                            branch_single_mode_state_t *state, bool inside_cutset,
                                            bool *out_delegated, branch_c64_t *out,
                                            qsop_error_t *error) {
  *out_delegated = false;
  qsop_instance_t sub = {0};
  qsop_amplitude_t delegated_amp = {0};
  if (!build_active_residual_subinstance(residual, &sub, error)) {
    return false;
  }
  const uint64_t dp_work_budget =
      (inside_cutset && state->cutset_treewidth_delegate_max_dp_work != 0)
          ? state->cutset_treewidth_delegate_max_dp_work
          : state->treewidth_delegate_max_dp_work;
  const qsop_branch_single_mode_options_t delegate_options = {
      .rw_source = state->rw_source,
      .policy = state->policy,
      .heuristic = state->heuristic,
      .fallback = state->fallback,
      .precision = state->precision,
      .kernel = state->kernel,
      .simd = state->simd,
      .treewidth_delegate_max_width = state->treewidth_delegate_max_width,
      .rankwidth_delegate_max_width = state->rankwidth_delegate_max_width,
      .treewidth_delegate_max_dp_work = dp_work_budget,
      .treewidth_delegate_max_memory_mib = state->treewidth_delegate_max_memory_mib,
      .trace = state->trace,
  };
  /* Diagnostic mode must inspect a legal delegate miss even under delegate-only policy.  The
   * recursive caller emits the probe and then restores the ordinary NO_DELEGATE refusal. */
  const bool fail_on_refusal =
      state->fallback == QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY &&
      !state->diagnose_conditioning;
  const bool ok = branch_single_mode_delegate_component(
      &sub, state->max_vars, state->target_mode, &delegate_options, fail_on_refusal, out_delegated,
      &delegated_amp, state->stats, &state->last_delegate_miss, error);
  if (state->stats != NULL) {
    state->stats->branch_last_delegate_miss = (uint32_t)state->last_delegate_miss;
  }
  free_subinstance(&sub);
  if (!ok || !*out_delegated) {
    return ok;
  }

  branch_c64_t amp =
      c64_normalize((branch_c64_t){delegated_amp.re, delegated_amp.im, delegated_amp.scale_exp2});
  const branch_c64_t constant =
      branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual));
  *out = c64_mul(constant, amp);
  state->numeric_error_bound += delegated_amp.numeric_error_bound;
  c64_accum_error(2, &state->numeric_error_bound);
  return true;
}

static bool branch_single_mode_cache_lookup(branch_single_mode_state_t *state,
                                            const qsop_residual_t *residual, branch_c64_t *out) {
  if (qsop_residual_active_vars(residual) > state->max_fallback_vars ||
      qsop_residual_active_vars(residual) < state->amp_cache.min_vars) {
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
    qsop_add_saturating_u64(&state->cache_avoided_nodes, entry->search_nodes - 1U);
  }
  state->numeric_error_bound += entry->numeric_error_bound;
  /* Entries are stored constant-free (see residual_cache_key_t); re-apply this residual's own
   * constant as the unit-modulus phase omega^constant that it is. */
  *out = c64_mul(entry->amp,
                 branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual)));
  c64_accum_error(1, &state->numeric_error_bound);
  return true;
}

static void branch_single_mode_set_failure(branch_single_mode_state_t *state,
                                           const qsop_residual_t *residual,
                                           qsop_solve_termination_reason_t reason,
                                           uint32_t stagnant_levels) {
  if (state->stats == NULL) {
    return;
  }
  state->stats->termination_reason = reason;
  state->stats->failure_active_vars = qsop_residual_active_vars(residual);
  state->stats->failure_active_edges = qsop_residual_active_edges(residual);
  state->stats->branch_cutset_final_vars = state->stats->failure_active_vars;
  state->stats->branch_cutset_final_edges = state->stats->failure_active_edges;
  state->stats->branch_cutset_stagnant_levels = stagnant_levels;
  state->stats->branch_last_delegate_miss = (uint32_t)state->last_delegate_miss;
}

static bool branch_delegate_probe_due(const qsop_residual_t *residual,
                                      const branch_single_mode_state_t *state,
                                      const branch_cutset_frame_t *frame) {
  const uint32_t vars = qsop_residual_active_vars(residual);
  const uint32_t edges = qsop_residual_active_edges(residual);
  if (state->max_cutset_depth == 0U || vars <= state->max_fallback_vars ||
      frame->vars_at_last_delegate_probe == 0U) {
    return true;
  }
  if (vars <= 128U || frame->levels_since_delegate_probe >= state->delegate_reprobe_interval) {
    return true;
  }
  if ((uint64_t)vars * 10U <= (uint64_t)frame->vars_at_last_delegate_probe * 9U ||
      (uint64_t)edges * 10U <= (uint64_t)frame->edges_at_last_delegate_probe * 9U) {
    return true;
  }
  return false;
}

static bool branch_cutset_level_productive(const branch_cutset_candidate_t *candidate,
                                           uint32_t parent_vars, uint32_t parent_edges) {
  const uint32_t worst_extra = candidate->child[0].extra_reductions <
                                       candidate->child[1].extra_reductions
                                   ? candidate->child[0].extra_reductions
                                   : candidate->child[1].extra_reductions;
  const uint32_t worst_largest = candidate->child[0].largest_component >
                                         candidate->child[1].largest_component
                                     ? candidate->child[0].largest_component
                                     : candidate->child[1].largest_component;
  const uint32_t worst_vars = candidate->child[0].active_vars > candidate->child[1].active_vars
                                  ? candidate->child[0].active_vars
                                  : candidate->child[1].active_vars;
  const uint32_t worst_edges = candidate->child[0].active_edges > candidate->child[1].active_edges
                                   ? candidate->child[0].active_edges
                                   : candidate->child[1].active_edges;
  const bool split = candidate->child[0].components > 1U || candidate->child[1].components > 1U;
  const bool zero = candidate->child[0].zero || candidate->child[1].zero;
  const bool substantial = (uint64_t)worst_vars * 10U <= (uint64_t)parent_vars * 9U ||
                           (uint64_t)worst_edges * 10U <= (uint64_t)parent_edges * 9U;
  return worst_extra > 0U || split || worst_largest + 1U < parent_vars || zero || substantial;
}

static bool branch_solve_cutset_node(qsop_residual_t *residual,
                                     branch_single_mode_state_t *state,
                                     branch_cutset_frame_t frame, branch_c64_t *out,
                                     qsop_error_t *error) {
  const uint32_t parent_vars = qsop_residual_active_vars(residual);
  const uint32_t parent_edges = qsop_residual_active_edges(residual);
  if (state->stats != NULL) {
    if (state->stats->branch_cutset_initial_vars == 0U) {
      state->stats->branch_cutset_initial_vars = parent_vars;
      state->stats->branch_cutset_initial_edges = parent_edges;
    }
    state->stats->branch_cutset_final_vars = parent_vars;
    state->stats->branch_cutset_final_edges = parent_edges;
  }
  if (frame.depth >= state->max_cutset_depth ||
      state->conditioning_nodes >= state->max_conditioning_nodes) {
    branch_single_mode_set_failure(state, residual, QSOP_SOLVE_TERMINATION_CUTSET_BUDGET,
                                   frame.stagnant_levels);
    qsop_set_error(error,
                   "branch single-Fourier cutset budget exhausted at depth %" PRIu32
                   " after %" PRIu64 " conditioning nodes",
                   frame.depth,
                   state->conditioning_nodes);
    return false;
  }
  state->conditioning_nodes++;
  if (state->stats != NULL) {
    state->stats->branch_conditioning_nodes = state->conditioning_nodes;
    if (frame.depth > state->stats->branch_max_cutset_depth) {
      state->stats->branch_max_cutset_depth = frame.depth;
    }
  }

  branch_cutset_candidate_t candidate = {0};
  const bool emit_diagnostic = state->diagnose_conditioning && !state->conditioning_diagnosed;
  if (!branch_choose_cutset_candidate(residual, state, emit_diagnostic, &candidate, error)) {
    return false;
  }
  if (emit_diagnostic) {
    state->conditioning_diagnosed = true;
  }
  const bool productive = branch_cutset_level_productive(&candidate, parent_vars, parent_edges);
  const uint32_t stagnant_levels = productive ? 0U : frame.stagnant_levels + 1U;
  if (stagnant_levels > state->max_stagnant_levels) {
    branch_cutset_candidate_free(&candidate);
    branch_single_mode_set_failure(state, residual, QSOP_SOLVE_TERMINATION_CUTSET_BUDGET,
                                   stagnant_levels);
    qsop_set_error(error,
                   "branch single-Fourier cutset budget exhausted after %" PRIu32
                   " stagnant levels",
                   stagnant_levels);
    return false;
  }

  branch_c64_t child_amp[2] = {c64_zero(), c64_zero()};
  bool ok = true;
  for (uint8_t value = 0; ok && value <= 1U; value++) {
    branch_cutset_child_t *child = &candidate.child[value];
    if (child->zero) {
      state->zero_prunes++;
      continue;
    }
    qsop_residual_t *child_residual = NULL;
    if (!qsop_residual_create(&child->reduced, &child_residual, error)) {
      ok = false;
      break;
    }
    branch_cutset_frame_t child_frame = frame;
    child_frame.depth++;
    child_frame.levels_since_delegate_probe++;
    child_frame.stagnant_levels = stagnant_levels;
    if (state->stats != NULL && child_frame.depth > state->stats->branch_max_cutset_depth) {
      state->stats->branch_max_cutset_depth = child_frame.depth;
    }
    state->depth++;
    ok = branch_sum_rec_single_mode(child_residual, state, false, child_frame, &child_amp[value],
                                    error);
    state->depth--;
    qsop_residual_free(child_residual);
    if (ok) {
      if (child->doublings > INT_MAX) {
        qsop_set_error(error, "cutset child amplitude exponent exceeds int range");
        ok = false;
      } else {
        child_amp[value].exp += (int)child->doublings;
      }
    }
  }
  if (ok) {
    *out = c64_add(child_amp[0], child_amp[1]);
    c64_accum_error(1, &state->numeric_error_bound);
  }
  branch_cutset_candidate_free(&candidate);
  return ok;
}

/* The amplitude of `residual` exactly as it stands: propagation, node and depth accounting all
 * belong to branch_sum_rec_single_mode below, which wraps this. */
static bool branch_sum_rec_single_mode_node(qsop_residual_t *residual,
                                            branch_single_mode_state_t *state,
                                            branch_cutset_frame_t frame, branch_c64_t *out,
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
    ok = branch_sum_components_single_mode(residual, state, frame, &did_split, out, error);
    if (ok && !did_split) {
      bool delegated = false;
      if (branch_delegate_probe_due(residual, state, &frame)) {
        if (state->stats != NULL) {
          state->stats->branch_delegate_probes++;
        }
        ok = branch_try_single_mode_delegate(residual, state, frame.depth > 0U, &delegated, out,
                                             error);
        frame.levels_since_delegate_probe = 0U;
        frame.vars_at_last_delegate_probe = qsop_residual_active_vars(residual);
        frame.edges_at_last_delegate_probe = qsop_residual_active_edges(residual);
      } else if (state->stats != NULL) {
        state->stats->branch_delegate_probe_skips++;
      }
      if (ok && !delegated) {
        const bool delegate_only =
            state->fallback == QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY;
        const bool too_large = qsop_residual_active_vars(residual) > state->max_fallback_vars;
        if ((too_large || delegate_only) && state->diagnose_conditioning &&
            !state->conditioning_diagnosed) {
          branch_cutset_candidate_t diagnostic = {0};
          ok = branch_choose_cutset_candidate(residual, state, true, &diagnostic, error);
          branch_cutset_candidate_free(&diagnostic);
          state->conditioning_diagnosed = ok;
        }
        if (ok && delegate_only) {
          branch_single_mode_set_failure(state, residual, QSOP_SOLVE_TERMINATION_NO_DELEGATE,
                                         frame.stagnant_levels);
          qsop_set_error(error,
                         "branch single-fourier: connected component (%" PRIu32
                         " vars) has no delegate available",
                         qsop_residual_active_vars(residual));
          ok = false;
        } else if (ok && too_large) {
          if (state->max_cutset_depth != 0U) {
            ok = branch_solve_cutset_node(residual, state, frame, out, error);
          } else {
            if (ok) {
              branch_single_mode_set_failure(state, residual,
                                             QSOP_SOLVE_TERMINATION_MAX_FALLBACK_VARS,
                                             frame.stagnant_levels);
              qsop_set_error(error,
                             "branch single-Fourier fallback refused component with %" PRIu32
                             " active vars: residual fallback cap %" PRIu32
                             " exceeded; enable bounded cutset conditioning or use "
                             "--branch-single-fourier-fallback=delegate-only",
                             qsop_residual_active_vars(residual), state->max_fallback_vars);
              ok = false;
            }
          }
        } else if (state->precision == QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE) {
          qsop_set_error(error, "branch single-Fourier residual fallback does not implement "
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
              ok = branch_sum_rec_single_mode(residual, state, true, frame, &branch_amp, error);
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
  /* Store the constant-free amplitude: *out carries this residual's omega^constant, and the key
   * does not. Dividing it out is one multiply by the conjugate phase, charged to the entry's own
   * error bound rather than the caller's, since *out itself is returned untouched. */
  const branch_c64_t cached = c64_mul(
      *out, c64_conj(branch_phase_lookup(&state->phase_cache, qsop_residual_constant(residual))));
  long double cached_error = subtree_error;
  c64_accum_error(1, &cached_error);
  if (qsop_residual_active_vars(residual) <= state->max_fallback_vars) {
    if (!branch_amp_cache_store(&state->amp_cache, residual, cached, cached_error, subtree_nodes,
                                error)) {
      return false;
    }
    if (qsop_residual_active_vars(residual) >= state->amp_cache.min_vars) {
      state->cache_stores++;
      qsop_trace_emit(state->trace, "branch.single.cache_store", state->depth,
                      (uint64_t)state->amp_cache.len, 0);
    }
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
                                       bool may_materialize, branch_cutset_frame_t frame,
                                       branch_c64_t *out, qsop_error_t *error) {
  if (state->max_recursion_depth != 0 && state->depth > state->max_recursion_depth) {
    branch_single_mode_set_failure(state, residual,
                                   QSOP_SOLVE_TERMINATION_MAX_RECURSION_DEPTH,
                                   frame.stagnant_levels);
    qsop_set_error(error, "branch single-Fourier recursion-depth cap exceeded");
    return false;
  }
  state->nodes++;
  if (state->max_search_nodes != 0 && state->nodes > state->max_search_nodes) {
    branch_single_mode_set_failure(state, residual, QSOP_SOLVE_TERMINATION_MAX_SEARCH_NODES,
                                   frame.stagnant_levels);
    qsop_set_error(error,
              "branch single-Fourier search-node cap exceeded (%" PRIu64
              "); try --branch-single-max-search-nodes with a larger value",
              state->max_search_nodes);
    return false;
  }

  const size_t checkpoint = qsop_residual_checkpoint(residual);
  uint32_t doublings = 0;
  bool zero = false;
  if (state->propagate) {
    const uint64_t propagate_start = qsop_trace_begin(state->trace);
    if (!qsop_residual_propagate(residual, &doublings, &zero, error)) {
      return false;
    }
    qsop_trace_emit_elapsed(state->trace, "branch.single.propagate", state->depth, doublings,
                            propagate_start);
    state->propagations += doublings;
  }

  if (zero) {
    state->zero_prunes++;
    qsop_trace_emit(state->trace, "branch.single.zero_prune", state->depth,
                    qsop_residual_active_vars(residual), 0);
    *out = c64_zero();
    return qsop_residual_undo(residual, checkpoint, error);
  }

  branch_c64_t amp = c64_zero();
  bool ok = true;
  branch_materialized_reduction_t materialized = {0};
  if (may_materialize && state->materialized_reduction) {
    ok = branch_materialize_reduction(residual, true, &materialized, state->stats, error);
    if (ok && materialized.zero) {
      state->zero_prunes++;
      amp = c64_zero();
    } else if (ok && materialized.changed) {
      qsop_residual_t *reduced_residual = NULL;
      ok = qsop_residual_create(&materialized.reduced, &reduced_residual, error);
      if (ok) {
        ok = branch_sum_rec_single_mode(reduced_residual, state, false, frame, &amp, error);
      }
      qsop_residual_free(reduced_residual);
      if (ok) {
        if (materialized.doublings > INT_MAX) {
          qsop_set_error(error, "materialized amplitude exponent exceeds int range");
          ok = false;
        } else {
          amp.exp += (int)materialized.doublings;
        }
      }
    } else if (ok) {
      ok = branch_sum_rec_single_mode_node(residual, state, frame, &amp, error);
    }
    free_subinstance(&materialized.reduced);
  } else {
    ok = branch_sum_rec_single_mode_node(residual, state, frame, &amp, error);
  }
  if (!ok) {
    return false;
  }
  if (!qsop_residual_undo(residual, checkpoint, error)) {
    return false;
  }
  if (doublings > INT_MAX) {
    qsop_set_error(error, "propagated amplitude exponent exceeds int range");
    return false;
  }
  amp.exp += (int)doublings;
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
  const branch_cutset_frame_t root_frame = {0};
  const bool ok = branch_sum_rec_single_mode(residual, &state, true, root_frame, &amp, error);
  if (ok) {
    out->re = amp.re;
    out->im = amp.im;
    out->scale_exp2 = amp.exp;
    out->numeric_error_bound = state.numeric_error_bound;
  } else if (stats != NULL && stats->termination_reason == QSOP_SOLVE_TERMINATION_NONE) {
    branch_single_mode_set_failure(&state, residual, QSOP_SOLVE_TERMINATION_OTHER_ERROR, 0U);
  }
  branch_single_mode_merge_final_stats(&state);
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
    qsop_set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL) {
    qsop_set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->r == 0) {
    qsop_set_error(error, "internal error: QSOP instance has a zero modulus");
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
    qsop_set_error(error,
              "branch single-fourier solver refuses %" PRIu32
              " variables outright (exceeds %ux --max-vars); pass a larger --max-vars",
              qsop->nvars, BRANCH_ROOT_SANITY_MULTIPLIER);
    return false;
  }

  const bool ok =
      branch_solve_single_mode_residual(qsop, max_vars, target_mode, &o, out, stats, error);
  if (!ok && stats != NULL && stats->termination_reason == QSOP_SOLVE_TERMINATION_NONE) {
    stats->termination_reason = QSOP_SOLVE_TERMINATION_OTHER_ERROR;
    stats->failure_active_vars = qsop->nvars;
    stats->failure_active_edges = qsop->nedges;
  }
  return ok;
}
