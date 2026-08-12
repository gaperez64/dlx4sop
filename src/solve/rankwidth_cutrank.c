/* Cut-rank computation over a rank decomposition, and the width/forecast diagnostics built on top
 * of it.
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

static void note_rankwidth_bitset_ops(qsop_solve_stats_t *stats, const qsop_simd_vtable_t *simd,
                                      size_t words, uint64_t calls) {
  if (stats == NULL || calls == 0) {
    return;
  }
  const uint64_t ops =
      words != 0 && calls > UINT64_MAX / words ? UINT64_MAX : calls * (uint64_t)words;
  if (simd != qsop_simd_scalar_vtable() && qsop_bitset_simd_worthwhile(simd, words)) {
    qsop_add_saturating_u64(&stats->simd_vectorized_ops, ops);
  } else {
    qsop_add_saturating_u64(&stats->simd_scalar_fallback_ops, ops);
  }
}
bool rw_refuse_large_modulus(const qsop_instance_t *qsop, qsop_error_t *error) {
  if (qsop != NULL && qsop->r > UINT32_MAX) {
    qsop_set_error(error,
                   "rankwidth count-table/all-modes-Fourier solve refuses modulus > 2^32-1; use "
                   "--solve-mode single-fourier");
    return false;
  }
  return true;
}
uint64_t *rw_adjacency_bitsets(const qsop_instance_t *qsop, size_t words, qsop_error_t *error) {
  uint64_t *adj = calloc((qsop->nvars == 0 ? 1U : qsop->nvars) * words, sizeof(*adj));
  if (adj == NULL) {
    qsop_set_error(error, "out of memory while allocating rankwidth adjacency bitsets");
    return NULL;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(adj, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(adj, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  return adj;
}
uint32_t rw_cut_rank_bitsets(uint32_t nvars, const uint64_t *adj, const uint64_t *left,
                             const uint64_t *right, size_t words, qsop_solve_stats_t *stats,
                             qsop_error_t *error) {
  uint64_t *rows = calloc((nvars == 0 ? 1U : nvars) * words, sizeof(*rows));
  if (rows == NULL) {
    qsop_set_error(error, "out of memory while computing rankwidth cut rank");
    return UINT32_MAX;
  }
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  uint32_t nrows = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_bitset_get(left, v)) {
      const uint64_t *source = qsop_bitset_const_row(adj, words, v);
      uint64_t *target = qsop_bitset_row(rows, words, nrows++);
      qsop_bitset_copy(target, source, words);
      qsop_bitset_and_simd(target, right, words, simd);
    }
  }
  note_rankwidth_bitset_ops(stats, simd, words, nrows);
  const uint32_t rank = qsop_gf2_rank_bitsets_simd(rows, nrows, nvars, words, simd);
  free(rows);
  return rank;
}
uint32_t rw_decomposition_width(const qsop_rankwidth_decomposition_t *decomposition,
                                const uint64_t *adj, qsop_solve_stats_t *stats,
                                qsop_error_t *error) {
  uint64_t *all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  uint64_t *right = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*right));
  if (all == NULL || right == NULL) {
    free(all);
    free(right);
    qsop_set_error(error, "out of memory while computing rankwidth decomposition width");
    return UINT32_MAX;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(all, v);
  }
  uint32_t width = 0;
  for (uint32_t i = 0; i < decomposition->nnodes; i++) {
    if (i == decomposition->root) {
      continue;
    }
    const uint64_t *left = node_vars_const(decomposition, i);
    qsop_bitset_copy(right, all, decomposition->words);
    qsop_bitset_and_not(right, left, decomposition->words);
    const uint32_t rank = rw_cut_rank_bitsets(decomposition->nvars, adj, left, right,
                                              decomposition->words, stats, error);
    if (rank == UINT32_MAX) {
      free(all);
      free(right);
      return UINT32_MAX;
    }
    if (rank > width) {
      width = rank;
    }
  }
  free(all);
  free(right);
  return width;
}
bool rw_decomposition_score(const qsop_instance_t *qsop,
                            const qsop_rankwidth_decomposition_t *decomposition,
                            const uint64_t *adj, rw_decomposition_score_t *out,
                            qsop_error_t *error) {
  if (out == NULL) {
    qsop_set_error(error, "internal error: null rankwidth decomposition score output");
    return false;
  }
  const uint32_t cutrank_width = rw_decomposition_width(decomposition, adj, NULL, error);
  if (cutrank_width == UINT32_MAX) {
    return false;
  }
  uint64_t table_forecast = 0;
  uint64_t join_pair_forecast = 0;
  if (!qsop_rankwidth_decomposition_forecast(qsop, decomposition, &table_forecast,
                                             &join_pair_forecast, error)) {
    return false;
  }
  *out = (rw_decomposition_score_t){
      .cutrank_width = cutrank_width,
      .table_forecast = table_forecast,
      .join_pair_forecast = join_pair_forecast,
  };
  return true;
}

static qsop_rankwidth_decomposition_options_t
normalize_single_decomposition_options(const qsop_rankwidth_decomposition_options_t *options) {
  qsop_rankwidth_decomposition_options_t normalized = {0};
  if (options != NULL) {
    normalized = *options;
  }
  if (normalized.kernel > QSOP_RANKWIDTH_SINGLE_KERNEL_PAIRWISE) {
    normalized.kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO;
  }
  if (normalized.precision > QSOP_RANKWIDTH_SINGLE_PRECISION_LONG_DOUBLE) {
    normalized.precision = QSOP_RANKWIDTH_SINGLE_PRECISION_F64;
  }
  if (normalized.best_search > QSOP_RANKWIDTH_BEST_EXHAUSTIVE) {
    normalized.best_search = QSOP_RANKWIDTH_BEST_PROGRESSIVE;
  }
  return normalized;
}

static uint64_t single_table_bytes(uint64_t signatures, size_t words, size_t value_size) {
  const uint64_t bytes_per_signature = qsop_saturating_add_u64(
      (uint64_t)(2U * value_size + 32U), qsop_saturating_mul_u64((uint64_t)words, 8U));
  return qsop_saturating_mul_u64(signatures, bytes_per_signature);
}

bool rw_decomposition_single_score(const qsop_instance_t *qsop,
                                   qsop_rankwidth_decomposition_t *decomposition,
                                   const qsop_rankwidth_decomposition_options_t *options,
                                   rw_decomposition_score_t *out, qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL || out == NULL) {
    qsop_set_error(error, "internal error: null single-mode decomposition score argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  const qsop_rankwidth_decomposition_options_t o = normalize_single_decomposition_options(options);
  if (decomposition->single_score_cached && decomposition->cached_single_kernel == o.kernel &&
      decomposition->cached_single_precision == o.precision &&
      decomposition->cached_single_even_target_mode == o.even_target_mode &&
      decomposition->cached_single_memory_budget_bytes == o.memory_budget_bytes) {
    *out = (rw_decomposition_score_t){
        .cutrank_width = decomposition->cached_cutrank_width,
        .table_forecast = decomposition->cached_table_forecast,
        .join_pair_forecast = decomposition->cached_join_pair_forecast,
        .single_feasible = decomposition->cached_single_feasible,
        .single_predicted_ns = decomposition->cached_single_predicted_ns,
        .single_peak_bytes = decomposition->cached_single_peak_bytes,
        .single_pairwise_joins = decomposition->cached_single_pairwise_joins,
        .single_twist_joins = decomposition->cached_single_twist_joins,
    };
    return true;
  }

  const size_t value_size = o.precision == QSOP_RANKWIDTH_SINGLE_PRECISION_LONG_DOUBLE
                                ? sizeof(long double)
                                : sizeof(double);
  if (qsop->nvars == 0U || qsop->nedges == 0U) {
    const uint64_t signatures = qsop->nvars == 0U ? 1U : 1U;
    const uint64_t peak = single_table_bytes(signatures, decomposition->words, value_size);
    const bool feasible = o.memory_budget_bytes == 0U || peak <= o.memory_budget_bytes;
    *out = (rw_decomposition_score_t){
        .cutrank_width = 0U,
        .table_forecast = qsop->nvars == 0U ? 1U : qsop->r,
        .join_pair_forecast = 0U,
        .single_feasible = feasible,
        .single_predicted_ns = feasible ? signatures * UINT64_C(35) : UINT64_MAX,
        .single_peak_bytes = peak,
    };
    goto cache;
  }

  uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
  uint64_t *all = calloc(decomposition->words == 0U ? 1U : decomposition->words, sizeof(*all));
  uint64_t *right = calloc(decomposition->words == 0U ? 1U : decomposition->words, sizeof(*right));
  uint64_t *signature_counts =
      calloc(decomposition->nnodes == 0U ? 1U : decomposition->nnodes, sizeof(*signature_counts));
  uint64_t *table_bytes =
      calloc(decomposition->nnodes == 0U ? 1U : decomposition->nnodes, sizeof(*table_bytes));
  uint32_t *cut_ranks =
      calloc(decomposition->nnodes == 0U ? 1U : decomposition->nnodes, sizeof(*cut_ranks));
  if (adj == NULL || all == NULL || right == NULL || signature_counts == NULL ||
      table_bytes == NULL || cut_ranks == NULL) {
    free(adj);
    free(all);
    free(right);
    free(signature_counts);
    free(table_bytes);
    free(cut_ranks);
    qsop_set_error(error, "out of memory while scoring rankwidth single-mode decomposition");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(all, v);
  }

  rw_decomposition_score_t score = {.single_feasible = true};
  uint64_t retained_signature_bytes = 0U;
  bool ok = true;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    uint32_t cut_rank = 0U;
    uint64_t signature_cap = 1U;
    if (node_id != decomposition->root) {
      const uint64_t *left = node_vars_const(decomposition, node_id);
      qsop_bitset_copy(right, all, decomposition->words);
      qsop_bitset_and_not(right, left, decomposition->words);
      cut_rank = rw_cut_rank_bitsets(decomposition->nvars, adj, left, right, decomposition->words,
                                     NULL, error);
      if (cut_rank == UINT32_MAX) {
        ok = false;
        break;
      }
      signature_cap = rw_binary_signature_bound(cut_rank);
    }
    cut_ranks[node_id] = cut_rank;
    if (cut_rank > score.cutrank_width) {
      score.cutrank_width = cut_rank;
    }

    uint64_t signatures = 0U;
    uint64_t workspace = 0U;
    if (node->kind == RW_NODE_LEAF) {
      signatures = min_u64(2U, signature_cap);
      score.single_predicted_ns = qsop_saturating_add_u64(
          score.single_predicted_ns, qsop_saturating_mul_u64(signatures, UINT64_C(35)));
    } else {
      const uint64_t left_count = signature_counts[node->left];
      const uint64_t right_count = signature_counts[node->right];
      const uint64_t pairs = qsop_saturating_mul_u64(left_count, right_count);
      score.join_pair_forecast = qsop_saturating_add_u64(score.join_pair_forecast, pairs);
      signatures = min_u64(pairs, signature_cap);
      const uint32_t crossing_rank = rw_cut_rank_bitsets(
          decomposition->nvars, adj, node_vars_const(decomposition, node->left),
          node_vars_const(decomposition, node->right), decomposition->words, NULL, error);
      if (crossing_rank == UINT32_MAX) {
        ok = false;
        break;
      }
      const rw_single_join_forecast_t join = rw_single_join_forecast(
          o.kernel, left_count, right_count, signatures, cut_ranks[node->left],
          cut_ranks[node->right], cut_rank, o.even_target_mode ? 0U : crossing_rank,
          decomposition->words, value_size, RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT,
          o.memory_budget_bytes);
      if (!join.feasible) {
        score.single_feasible = false;
        score.single_predicted_ns = UINT64_MAX;
      } else if (score.single_feasible) {
        score.single_predicted_ns =
            qsop_saturating_add_u64(score.single_predicted_ns, join.selected_ns);
      }
      workspace = join.selected_workspace_bytes;
      if (join.selected == RW_SINGLE_JOIN_TWIST) {
        score.single_twist_joins++;
      } else {
        score.single_pairwise_joins++;
      }
    }
    signature_counts[node_id] = signatures;
    const uint64_t legacy_entries = qsop_saturating_mul_u64(signatures, qsop->r);
    if (legacy_entries > score.table_forecast) {
      score.table_forecast = legacy_entries;
    }
    table_bytes[node_id] = single_table_bytes(signatures, decomposition->words, value_size);
    const uint64_t pool_increment = qsop_saturating_mul_u64(
        signatures, qsop_saturating_add_u64((uint64_t)decomposition->words * 8U, 8U));
    retained_signature_bytes = qsop_saturating_add_u64(retained_signature_bytes, pool_increment);
    uint64_t live = qsop_saturating_add_u64(retained_signature_bytes, table_bytes[node_id]);
    if (node->kind == RW_NODE_JOIN) {
      live = qsop_saturating_add_u64(live, table_bytes[node->left]);
      live = qsop_saturating_add_u64(live, table_bytes[node->right]);
      live = qsop_saturating_add_u64(live, workspace);
    }
    if (live > score.single_peak_bytes) {
      score.single_peak_bytes = live;
    }
    if (o.memory_budget_bytes != 0U && live > o.memory_budget_bytes) {
      score.single_feasible = false;
      score.single_predicted_ns = UINT64_MAX;
    }
  }
  free(adj);
  free(all);
  free(right);
  free(signature_counts);
  free(table_bytes);
  free(cut_ranks);
  if (!ok) {
    return false;
  }
  *out = score;

cache:
  decomposition->score_cached = true;
  decomposition->cached_cutrank_width = out->cutrank_width;
  decomposition->cached_table_forecast = out->table_forecast;
  decomposition->cached_join_pair_forecast = out->join_pair_forecast;
  decomposition->single_score_cached = true;
  decomposition->cached_single_feasible = out->single_feasible;
  decomposition->cached_single_kernel = o.kernel;
  decomposition->cached_single_precision = o.precision;
  decomposition->cached_single_even_target_mode = o.even_target_mode;
  decomposition->cached_single_memory_budget_bytes = o.memory_budget_bytes;
  decomposition->cached_single_predicted_ns = out->single_predicted_ns;
  decomposition->cached_single_peak_bytes = out->single_peak_bytes;
  decomposition->cached_single_pairwise_joins = out->single_pairwise_joins;
  decomposition->cached_single_twist_joins = out->single_twist_joins;
  return true;
}
bool qsop_rankwidth_decomposition_width(const qsop_instance_t *qsop,
                                        qsop_rankwidth_decomposition_t *decomposition,
                                        uint32_t *cutrank_width_out, qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL || cutrank_width_out == NULL) {
    qsop_set_error(error, "internal error: null rankwidth width argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (qsop->nedges == 0) {
    const uint64_t table_forecast = qsop->nvars == 0 ? 1U : qsop->r;
    *cutrank_width_out = 0;
    decomposition->score_cached = true;
    decomposition->cached_cutrank_width = 0;
    decomposition->cached_table_forecast = table_forecast;
    decomposition->cached_join_pair_forecast = 0;
    return true;
  }

  uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
  if (adj == NULL) {
    return false;
  }

  rw_decomposition_score_t score = {0};
  const bool ok = rw_decomposition_score(qsop, decomposition, adj, &score, error);
  free(adj);
  if (!ok) {
    return false;
  }
  *cutrank_width_out = score.cutrank_width;
  /* Cache the full score so rankwidth_record_decomposition_diagnostics can skip
   * recomputing decomposition_score when the decomposition is immediately solved. */
  decomposition->score_cached = true;
  decomposition->cached_cutrank_width = score.cutrank_width;
  decomposition->cached_table_forecast = score.table_forecast;
  decomposition->cached_join_pair_forecast = score.join_pair_forecast;
  return true;
}
uint64_t rw_binary_signature_bound(uint32_t width) {
  if (width >= 64U) {
    return UINT64_MAX;
  }
  return UINT64_C(1) << width;
}
void rw_dense_fourier_forecast(const qsop_instance_t *qsop,
                               const qsop_rankwidth_decomposition_t *decomposition,
                               uint32_t cutrank_width, uint64_t *dense_table_out,
                               uint64_t *dense_even_join_out) {
  const uint64_t dense_signatures = rw_binary_signature_bound(cutrank_width);
  const uint64_t dense_table = qsop_saturating_mul_u64(dense_signatures, qsop->r);
  uint64_t join_nodes = 0;
  for (uint32_t i = 0; i < decomposition->nnodes; i++) {
    if (decomposition->nodes[i].kind == RW_NODE_JOIN) {
      join_nodes++;
    }
  }
  const uint64_t even_modes = qsop->r / 2U;
  const uint64_t width_work = qsop_saturating_mul_u64(cutrank_width, dense_signatures);
  const uint64_t per_join =
      qsop_saturating_mul_u64(qsop_saturating_mul_u64(3U, even_modes), width_work);
  if (dense_table_out != NULL) {
    *dense_table_out = dense_table;
  }
  if (dense_even_join_out != NULL) {
    *dense_even_join_out = qsop_saturating_mul_u64(join_nodes, per_join);
  }
}
bool qsop_rankwidth_decomposition_forecast(const qsop_instance_t *qsop,
                                           const qsop_rankwidth_decomposition_t *decomposition,
                                           uint64_t *max_table_entries_out,
                                           uint64_t *join_pairs_out, qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL ||
      (max_table_entries_out == NULL && join_pairs_out == NULL)) {
    qsop_set_error(error, "internal error: null rankwidth forecast argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (decomposition->score_cached) {
    if (max_table_entries_out != NULL) {
      *max_table_entries_out = decomposition->cached_table_forecast;
    }
    if (join_pairs_out != NULL) {
      *join_pairs_out = decomposition->cached_join_pair_forecast;
    }
    return true;
  }
  if (qsop->nedges == 0) {
    if (max_table_entries_out != NULL) {
      *max_table_entries_out = qsop->nvars == 0 ? 1U : qsop->r;
    }
    if (join_pairs_out != NULL) {
      *join_pairs_out = 0;
    }
    return true;
  }

  uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
  uint64_t *all = NULL;
  uint64_t *right = NULL;
  uint64_t *signature_counts = NULL;
  if (adj == NULL) {
    return false;
  }

  all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  right = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*right));
  signature_counts =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*signature_counts));
  if (all == NULL || right == NULL || signature_counts == NULL) {
    free(adj);
    free(all);
    free(right);
    free(signature_counts);
    qsop_set_error(error, "out of memory while forecasting rankwidth table pressure");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(all, v);
  }

  uint64_t max_table_entries = 0;
  uint64_t join_pairs = 0;
  bool ok = true;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];

    uint64_t signature_cap = 1;
    if (node_id != decomposition->root) {
      const uint64_t *left = node_vars_const(decomposition, node_id);
      qsop_bitset_copy(right, all, decomposition->words);
      qsop_bitset_and_not(right, left, decomposition->words);
      const uint32_t width = rw_cut_rank_bitsets(decomposition->nvars, adj, left, right,
                                                 decomposition->words, NULL, error);
      if (width == UINT32_MAX) {
        ok = false;
        break;
      }
      signature_cap = rw_binary_signature_bound(width);
    }

    uint64_t signatures = 0;
    if (node->kind == RW_NODE_LEAF) {
      signatures = min_u64(2U, signature_cap);
    } else {
      const uint64_t pair_count =
          qsop_saturating_mul_u64(signature_counts[node->left], signature_counts[node->right]);
      join_pairs = qsop_saturating_add_u64(join_pairs, pair_count);
      signatures = min_u64(pair_count, signature_cap);
    }
    signature_counts[node_id] = signatures;
    const uint64_t table_entries = qsop_saturating_mul_u64(signatures, qsop->r);
    if (table_entries > max_table_entries) {
      max_table_entries = table_entries;
    }
  }

  free(adj);
  free(all);
  free(right);
  free(signature_counts);
  if (!ok) {
    return false;
  }
  if (max_table_entries_out != NULL) {
    *max_table_entries_out = max_table_entries;
  }
  if (join_pairs_out != NULL) {
    *join_pairs_out = join_pairs;
  }
  return true;
}
bool rw_record_decomposition_diagnostics(const qsop_instance_t *qsop,
                                         const qsop_rankwidth_decomposition_t *decomposition,
                                         qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                         qsop_error_t *error) {
  if (stats == NULL && trace == NULL) {
    return true;
  }

  const uint64_t start = qsop_trace_begin(trace);

  rw_decomposition_score_t score = {0};
  if (decomposition->score_cached) {
    score.cutrank_width = decomposition->cached_cutrank_width;
    score.table_forecast = decomposition->cached_table_forecast;
    score.join_pair_forecast = decomposition->cached_join_pair_forecast;
  } else if (qsop->nedges == 0) {
    score.cutrank_width = 0;
    score.table_forecast = qsop->nvars == 0 ? 1U : qsop->r;
    score.join_pair_forecast = 0;
  } else {
    uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
    const bool ok = rw_decomposition_score(qsop, decomposition, adj, &score, error);
    free(adj);
    if (!ok) {
      return false;
    }
  }

  if (stats != NULL) {
    stats->rankwidth_cutrank_width = score.cutrank_width;
    stats->rankwidth_table_forecast = score.table_forecast;
    stats->rankwidth_join_pair_forecast = score.join_pair_forecast;
    rw_dense_fourier_forecast(qsop, decomposition, score.cutrank_width,
                              &stats->rankwidth_dense_table_forecast,
                              &stats->rankwidth_dense_even_join_forecast);
    if (decomposition->single_score_cached) {
      stats->rankwidth_predicted_solve_ns = decomposition->cached_single_predicted_ns;
      stats->rankwidth_predicted_peak_bytes = decomposition->cached_single_peak_bytes;
      stats->rankwidth_predicted_pairwise_join_events = decomposition->cached_single_pairwise_joins;
      stats->rankwidth_predicted_twist_join_events = decomposition->cached_single_twist_joins;
      stats->rankwidth_planner_candidates = decomposition->planner_candidates;
      stats->rankwidth_planner_estimated_ns = decomposition->planner_estimated_ns;
      stats->rankwidth_planner_budget_ns = decomposition->planner_budget_ns;
      stats->rankwidth_planner_budget_exhaustions = decomposition->planner_budget_exhaustions;
    }
  }
  qsop_trace_emit_elapsed(trace, "rankwidth.width_probe", 0, score.cutrank_width, start);
  qsop_trace_emit(trace, "rankwidth.cutrank_width_probe", 0, score.cutrank_width, 0);
  qsop_trace_emit(trace, "rankwidth.table_forecast", 0, score.table_forecast, 0);
  qsop_trace_emit(trace, "rankwidth.join_pair_forecast", 0, score.join_pair_forecast, 0);
  if (stats != NULL) {
    qsop_trace_emit(trace, "rankwidth.dense_table_forecast", 0,
                    stats->rankwidth_dense_table_forecast, 0);
    qsop_trace_emit(trace, "rankwidth.dense_even_join_forecast", 0,
                    stats->rankwidth_dense_even_join_forecast, 0);
    if (decomposition->single_score_cached) {
      qsop_trace_emit(trace, "rankwidth.single_predicted_ns", 0,
                      decomposition->cached_single_predicted_ns, 0);
      qsop_trace_emit(trace, "rankwidth.single_predicted_peak_bytes", 0,
                      decomposition->cached_single_peak_bytes, 0);
      qsop_trace_emit(trace, "rankwidth.single_predicted_twist_joins", 0,
                      decomposition->cached_single_twist_joins, 0);
      qsop_trace_emit(trace, "rankwidth.planner_candidates", 0, decomposition->planner_candidates,
                      0);
    }
  }
  return true;
}
