#include "dlx4sop/qsop_stats.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/min_fill.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define QSOP_EXACT_WIDTH_HARD_MAX 16U

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

static bool write_failed(FILE *file, qsop_error_t *error) {
  if (!ferror(file)) {
    return false;
  }
  set_error(error, "write failed: %s", strerror(errno));
  return true;
}

static uint64_t bit_for_var(uint32_t v) {
  return UINT64_C(1) << v;
}

static uint64_t all_vars_mask(uint32_t nvars) {
  return (UINT64_C(1) << nvars) - UINT64_C(1);
}

static uint32_t popcount_u64(uint64_t value) {
  uint32_t count = 0;
  while (value != 0) {
    value &= value - 1U;
    count++;
  }
  return count;
}

static uint32_t first_set_bit_u64(uint64_t value) {
  uint32_t bit = 0;
  while ((value & UINT64_C(1)) == 0) {
    value >>= 1U;
    bit++;
  }
  return bit;
}

static uint32_t gf2_rank_masks(uint64_t *rows, uint32_t nrows, uint32_t nvars) {
  uint32_t rank = 0;
  for (uint32_t col = 0; col < nvars && rank < nrows; col++) {
    const uint64_t bit = bit_for_var(col);
    uint32_t pivot = rank;
    while (pivot < nrows && (rows[pivot] & bit) == 0) {
      pivot++;
    }
    if (pivot == nrows) {
      continue;
    }
    if (pivot != rank) {
      const uint64_t tmp = rows[rank];
      rows[rank] = rows[pivot];
      rows[pivot] = tmp;
    }
    for (uint32_t row = 0; row < nrows; row++) {
      if (row != rank && (rows[row] & bit) != 0) {
        rows[row] ^= rows[rank];
      }
    }
    rank++;
  }
  return rank;
}

static bool compute_width_diagnostics_with_order(const qsop_instance_t *qsop, qsop_stats_t *stats,
                                                 uint32_t *order, qsop_error_t *error) {
  /* Exact prefix cut rank via the incremental GF(2) core (was a dense O(nvars^4/64) rebuild per
   * cut). */
  if (!qsop_prefix_cut_rank(qsop->nvars, qsop->edge_u, qsop->edge_v, qsop->nedges,
                            &stats->prefix_cut_rank, error)) {
    return false;
  }
  /* Min-fill width/fill/order via the shared sparse core. The elimination order is only captured
   * for the small (<=63-var) case, matching the historical contract. */
  uint32_t *order_out = qsop->nvars <= 63U ? order : NULL;
  if (!qsop_min_fill_eliminate(qsop->nvars, qsop->edge_u, qsop->edge_v, qsop->nedges,
                               QSOP_TREEWIDTH_ORDER_MIN_FILL, UINT32_MAX, order_out,
                               &stats->min_fill_width, &stats->min_fill_edges,
                               &stats->min_fill_dp_work, NULL, error)) {
    return false;
  }
  stats->width_diagnostics_available = true;
  return true;
}

static void build_support_adjacency_masks(const qsop_instance_t *qsop, uint64_t *adj) {
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    adj[v] = 0;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    adj[qsop->edge_u[e]] |= bit_for_var(qsop->edge_v[e]);
    adj[qsop->edge_v[e]] |= bit_for_var(qsop->edge_u[e]);
  }
}

static uint32_t active_degree(uint32_t v, const uint64_t *adj, uint64_t active) {
  return popcount_u64(adj[v] & active & ~bit_for_var(v));
}

static void exact_treewidth_search(uint32_t nvars, uint64_t active, const uint64_t *adj,
                                   uint32_t current_width, uint32_t *best_width) {
  if (current_width >= *best_width) {
    return;
  }
  if (active == 0) {
    *best_width = current_width;
    return;
  }

  uint32_t min_degree = UINT32_MAX;
  for (uint32_t v = 0; v < nvars; v++) {
    if ((active & bit_for_var(v)) == 0) {
      continue;
    }
    const uint32_t degree = active_degree(v, adj, active);
    if (degree < min_degree) {
      min_degree = degree;
    }
  }
  if (min_degree != UINT32_MAX &&
      (current_width > min_degree ? current_width : min_degree) >= *best_width) {
    return;
  }

  uint64_t candidates = active;
  while (candidates != 0) {
    bool found = false;
    uint32_t best_v = 0;
    uint32_t best_degree = UINT32_MAX;
    for (uint32_t v = 0; v < nvars; v++) {
      if ((candidates & bit_for_var(v)) == 0) {
        continue;
      }
      const uint32_t degree = active_degree(v, adj, active);
      if (!found || degree < best_degree) {
        found = true;
        best_v = v;
        best_degree = degree;
      }
    }
    if (!found) {
      break;
    }
    candidates &= ~bit_for_var(best_v);

    const uint32_t next_width = current_width > best_degree ? current_width : best_degree;
    if (next_width >= *best_width) {
      continue;
    }

    uint64_t next_adj[QSOP_EXACT_WIDTH_HARD_MAX] = {0};
    for (uint32_t v = 0; v < nvars; v++) {
      next_adj[v] = adj[v];
    }

    const uint64_t neighbors = adj[best_v] & active & ~bit_for_var(best_v);
    uint64_t remaining_neighbors = neighbors;
    while (remaining_neighbors != 0) {
      const uint32_t u = first_set_bit_u64(remaining_neighbors);
      remaining_neighbors &= ~bit_for_var(u);
      next_adj[u] |= neighbors & ~bit_for_var(u);
    }

    const uint64_t next_active = active & ~bit_for_var(best_v);
    for (uint32_t v = 0; v < nvars; v++) {
      next_adj[v] &= next_active;
    }
    next_adj[best_v] = 0;
    exact_treewidth_search(nvars, next_active, next_adj, next_width, best_width);
  }
}

static uint32_t exact_treewidth_masks(uint32_t nvars, const uint64_t *adj, uint32_t upper_bound) {
  if (nvars <= 1) {
    return 0;
  }
  uint32_t best_width = upper_bound;
  exact_treewidth_search(nvars, all_vars_mask(nvars), adj, 0, &best_width);
  return best_width;
}

static uint32_t max4_u32(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  uint32_t out = a > b ? a : b;
  out = out > c ? out : c;
  return out > d ? out : d;
}

static bool exact_rankwidth_masks(uint32_t nvars, const uint64_t *adj, uint32_t *out,
                                  qsop_error_t *error) {
  if (nvars <= 1) {
    *out = 0;
    return true;
  }

  const uint32_t states = UINT32_C(1) << nvars;
  uint8_t *cutrank = calloc(states, sizeof(*cutrank));
  uint8_t *dp = calloc(states, sizeof(*dp));
  if (cutrank == NULL || dp == NULL) {
    free(cutrank);
    free(dp);
    set_error(error, "out of memory while computing exact rankwidth");
    return false;
  }

  const uint64_t all = all_vars_mask(nvars);
  uint64_t rows[QSOP_EXACT_WIDTH_HARD_MAX] = {0};
  for (uint32_t mask = 0; mask < states; mask++) {
    const uint64_t right = all & ~(uint64_t)mask;
    uint32_t nrows = 0;
    for (uint32_t v = 0; v < nvars; v++) {
      if (((uint64_t)mask & bit_for_var(v)) != 0) {
        rows[nrows++] = adj[v] & right;
      }
    }
    cutrank[mask] = (uint8_t)gf2_rank_masks(rows, nrows, nvars);
  }

  for (uint32_t size = 2; size <= nvars; size++) {
    for (uint32_t mask = 1; mask < states; mask++) {
      if (popcount_u64(mask) != size) {
        continue;
      }
      const uint32_t anchor = mask & (0U - mask);
      uint32_t best = nvars;
      for (uint32_t sub = (mask - 1U) & mask; sub != 0; sub = (sub - 1U) & mask) {
        if ((sub & anchor) == 0 || sub == mask) {
          continue;
        }
        const uint32_t other = mask ^ sub;
        const uint32_t width = max4_u32(cutrank[sub], cutrank[other], dp[sub], dp[other]);
        if (width < best) {
          best = width;
        }
      }
      dp[mask] = (uint8_t)best;
    }
  }

  *out = dp[states - 1U];
  free(cutrank);
  free(dp);
  return true;
}

static bool compute_exact_widths(const qsop_instance_t *qsop, const qsop_stats_options_t *options,
                                 qsop_stats_t *stats, qsop_error_t *error) {
  if (options == NULL || !options->exact_widths) {
    return true;
  }

  stats->exact_widths_requested = true;
  stats->exact_width_max_vars = options->exact_width_max_vars;
  if (stats->exact_width_max_vars == 0) {
    stats->exact_width_max_vars = 12;
  }
  if (stats->exact_width_max_vars > QSOP_EXACT_WIDTH_HARD_MAX) {
    stats->exact_width_max_vars = QSOP_EXACT_WIDTH_HARD_MAX;
  }
  if (qsop->nvars > stats->exact_width_max_vars || qsop->nvars > QSOP_EXACT_WIDTH_HARD_MAX) {
    stats->exact_widths_available = false;
    return true;
  }

  uint64_t adj[QSOP_EXACT_WIDTH_HARD_MAX] = {0};
  build_support_adjacency_masks(qsop, adj);
  stats->exact_treewidth = exact_treewidth_masks(qsop->nvars, adj, stats->min_fill_width);
  if (!exact_rankwidth_masks(qsop->nvars, adj, &stats->exact_rankwidth, error)) {
    return false;
  }
  stats->exact_widths_available = true;
  return true;
}

static bool compute_stats_internal(const qsop_instance_t *qsop, const qsop_stats_options_t *options,
                                   qsop_stats_t *stats, uint32_t *order, qsop_error_t *error) {
  if (qsop == NULL || stats == NULL) {
    set_error(error, "internal error: null stats argument");
    return false;
  }

  *stats = (qsop_stats_t){
      .r = qsop->r,
      .nvars = qsop->nvars,
      .nedges = qsop->nedges,
      .norm_h = qsop->norm_h,
  };

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (qsop->unary[v] != 0) {
      stats->nonzero_unary++;
    }
  }

  uint32_t *degree = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*degree));
  uint64_t *rowptr = calloc((size_t)qsop->nvars + 1U, sizeof(*rowptr));
  uint32_t *cursor = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*cursor));
  uint32_t *colind = calloc(qsop->nedges == 0 ? 1U : (size_t)2U * qsop->nedges, sizeof(*colind));
  bool *visited = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*visited));
  uint32_t *queue = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*queue));
  if (degree == NULL || rowptr == NULL || cursor == NULL || colind == NULL || visited == NULL ||
      queue == NULL) {
    free(degree);
    free(rowptr);
    free(cursor);
    free(colind);
    free(visited);
    free(queue);
    set_error(error, "out of memory while computing stats");
    return false;
  }

  for (uint32_t e = 0; e < qsop->nedges; e++) {
    degree[qsop->edge_u[e]]++;
    degree[qsop->edge_v[e]]++;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (degree[v] > stats->max_degree) {
      stats->max_degree = degree[v];
    }
    rowptr[v + 1U] = rowptr[v] + degree[v];
    cursor[v] = (uint32_t)rowptr[v];
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint32_t u = qsop->edge_u[e];
    const uint32_t v = qsop->edge_v[e];
    colind[cursor[u]++] = v;
    colind[cursor[v]++] = u;
  }

  for (uint32_t start = 0; start < qsop->nvars; start++) {
    if (visited[start]) {
      continue;
    }

    stats->components++;
    uint32_t head = 0;
    uint32_t tail = 0;
    visited[start] = true;
    queue[tail++] = start;
    while (head < tail) {
      const uint32_t v = queue[head++];
      for (uint64_t p = rowptr[v]; p < rowptr[v + 1U]; p++) {
        const uint32_t other = colind[p];
        if (!visited[other]) {
          visited[other] = true;
          queue[tail++] = other;
        }
      }
    }
  }

  free(degree);
  free(rowptr);
  free(cursor);
  free(colind);
  free(visited);
  free(queue);
  if (!compute_width_diagnostics_with_order(qsop, stats, order, error)) {
    return false;
  }
  return compute_exact_widths(qsop, options, stats, error);
}

bool qsop_compute_stats_with_options(const qsop_instance_t *qsop,
                                     const qsop_stats_options_t *options, qsop_stats_t *stats,
                                     qsop_error_t *error) {
  return compute_stats_internal(qsop, options, stats, NULL, error);
}

bool qsop_compute_stats(const qsop_instance_t *qsop, qsop_stats_t *stats, qsop_error_t *error) {
  return compute_stats_internal(qsop, NULL, stats, NULL, error);
}

bool qsop_compute_stats_with_order(const qsop_instance_t *qsop, qsop_stats_t *stats,
                                   uint32_t *order, qsop_error_t *error) {
  return compute_stats_internal(qsop, NULL, stats, order, error);
}

bool qsop_stats_write_text(FILE *file, const qsop_stats_t *stats, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    set_error(error, "internal error: null stats text write argument");
    return false;
  }

  fprintf(file, "modulus: %" PRIu64 "\n", stats->r);
  fprintf(file, "variables: %" PRIu32 "\n", stats->nvars);
  fprintf(file, "quadratic_terms: %" PRIu32 "\n", stats->nedges);
  fprintf(file, "nonzero_unary: %" PRIu32 "\n", stats->nonzero_unary);
  fprintf(file, "normalization_h: %" PRIu64 "\n", stats->norm_h);
  fprintf(file, "format: qsop-sign\n");
  fprintf(file, "components: %" PRIu32 "\n", stats->components);
  fprintf(file, "max_degree: %" PRIu32 "\n", stats->max_degree);
  fprintf(file, "width_diagnostics: %s\n",
          stats->width_diagnostics_available ? "available" : "unavailable");
  if (stats->width_diagnostics_available) {
    fprintf(file, "min_fill_width: %" PRIu32 "\n", stats->min_fill_width);
    fprintf(file, "min_fill_edges: %" PRIu64 "\n", stats->min_fill_edges);
    fprintf(file, "prefix_cut_rank: %" PRIu32 "\n", stats->prefix_cut_rank);
  }
  if (stats->exact_widths_requested) {
    fprintf(file, "exact_widths: %s\n", stats->exact_widths_available ? "available" : "skipped");
    fprintf(file, "exact_width_max_vars: %" PRIu32 "\n", stats->exact_width_max_vars);
    if (stats->exact_widths_available) {
      fprintf(file, "exact_treewidth: %" PRIu32 "\n", stats->exact_treewidth);
      fprintf(file, "exact_rankwidth: %" PRIu32 "\n", stats->exact_rankwidth);
    }
  }
  return !write_failed(file, error);
}

bool qsop_stats_write_json(FILE *file, const qsop_stats_t *stats, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    set_error(error, "internal error: null stats JSON write argument");
    return false;
  }

  fprintf(file,
          "{\"modulus\":%" PRIu64 ",\"variables\":%" PRIu32 ",\"quadratic_terms\":%" PRIu32
          ",\"nonzero_unary\":%" PRIu32 ",\"normalization_h\":%" PRIu64
          ",\"format\":\"qsop-sign\",\"components\":%" PRIu32 ",\"max_degree\":%" PRIu32
          ",\"width_diagnostics_available\":%s",
          stats->r, stats->nvars, stats->nedges, stats->nonzero_unary, stats->norm_h,
          stats->components, stats->max_degree,
          stats->width_diagnostics_available ? "true" : "false");
  if (stats->width_diagnostics_available) {
    fprintf(file,
            ",\"min_fill_width\":%" PRIu32 ",\"min_fill_edges\":%" PRIu64
            ",\"prefix_cut_rank\":%" PRIu32,
            stats->min_fill_width, stats->min_fill_edges, stats->prefix_cut_rank);
  }
  if (stats->exact_widths_requested) {
    fprintf(file, ",\"exact_widths_available\":%s,\"exact_width_max_vars\":%" PRIu32,
            stats->exact_widths_available ? "true" : "false", stats->exact_width_max_vars);
    if (stats->exact_widths_available) {
      fprintf(file, ",\"exact_treewidth\":%" PRIu32 ",\"exact_rankwidth\":%" PRIu32,
              stats->exact_treewidth, stats->exact_rankwidth);
    }
  }
  fputs("}\n", file);
  return !write_failed(file, error);
}
