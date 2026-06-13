#include "dlx4sop/qsop_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

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

static uint64_t min_fill_edges_for(uint32_t v, const uint64_t *adj, uint64_t active) {
  uint64_t neighbors = adj[v] & active;
  uint64_t fill = 0;
  while (neighbors != 0) {
    const uint32_t u = first_set_bit_u64(neighbors);
    neighbors &= ~bit_for_var(u);
    const uint64_t missing = neighbors & ~adj[u];
    fill += popcount_u64(missing);
  }
  return fill;
}

static bool compute_width_diagnostics(const qsop_instance_t *qsop, qsop_stats_t *stats,
                                      qsop_error_t *error) {
  if (qsop->nvars > 63U) {
    return true;
  }

  uint64_t *adj = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*adj));
  uint64_t *work = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*work));
  uint64_t *rows = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*rows));
  if (adj == NULL || work == NULL || rows == NULL) {
    free(adj);
    free(work);
    free(rows);
    set_error(error, "out of memory while computing width diagnostics");
    return false;
  }

  for (uint32_t e = 0; e < qsop->nedges; e++) {
    adj[qsop->edge_u[e]] |= bit_for_var(qsop->edge_v[e]);
    adj[qsop->edge_v[e]] |= bit_for_var(qsop->edge_u[e]);
  }

  const uint64_t all = all_vars_mask(qsop->nvars);
  for (uint32_t cut = 1; cut < qsop->nvars; cut++) {
    const uint64_t left = all_vars_mask(cut);
    const uint64_t right = all & ~left;
    uint32_t nrows = 0;
    for (uint32_t v = 0; v < cut; v++) {
      rows[nrows++] = adj[v] & right;
    }
    const uint32_t rank = gf2_rank_masks(rows, nrows, qsop->nvars);
    if (rank > stats->linear_cut_rank) {
      stats->linear_cut_rank = rank;
    }
  }

  memcpy(work, adj, (qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*work));
  uint64_t active = all;
  while (active != 0) {
    bool found = false;
    uint32_t best = 0;
    uint64_t best_fill = UINT64_MAX;
    uint32_t best_degree = UINT32_MAX;
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if ((active & bit_for_var(v)) == 0) {
        continue;
      }
      const uint32_t degree = popcount_u64(work[v] & active);
      const uint64_t fill = min_fill_edges_for(v, work, active);
      if (!found || fill < best_fill || (fill == best_fill && degree < best_degree)) {
        found = true;
        best = v;
        best_fill = fill;
        best_degree = degree;
      }
    }
    if (!found) {
      break;
    }
    if (best_degree > stats->min_fill_width) {
      stats->min_fill_width = best_degree;
    }
    stats->min_fill_edges += best_fill;

    uint64_t neighbors = work[best] & active;
    for (uint32_t u = 0; u < qsop->nvars; u++) {
      if ((neighbors & bit_for_var(u)) == 0) {
        continue;
      }
      for (uint32_t v = u + 1U; v < qsop->nvars; v++) {
        if ((neighbors & bit_for_var(v)) != 0) {
          work[u] |= bit_for_var(v);
          work[v] |= bit_for_var(u);
        }
      }
    }
    active &= ~bit_for_var(best);
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      work[v] &= active;
    }
  }

  stats->width_diagnostics_available = true;
  free(adj);
  free(work);
  free(rows);
  return true;
}

bool qsop_compute_stats(const qsop_instance_t *qsop, qsop_stats_t *stats, qsop_error_t *error) {
  if (qsop == NULL || stats == NULL) {
    set_error(error, "internal error: null stats argument");
    return false;
  }

  *stats = (qsop_stats_t){
      .r = qsop->r,
      .nvars = qsop->nvars,
      .nedges = qsop->nedges,
      .norm_h = qsop->norm_h,
      .mode = qsop->mode,
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
  return compute_width_diagnostics(qsop, stats, error);
}

bool qsop_stats_write_text(FILE *file, const qsop_stats_t *stats, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    set_error(error, "internal error: null stats text write argument");
    return false;
  }

  fprintf(file, "modulus: %" PRIu32 "\n", stats->r);
  fprintf(file, "variables: %" PRIu32 "\n", stats->nvars);
  fprintf(file, "quadratic_terms: %" PRIu32 "\n", stats->nedges);
  fprintf(file, "nonzero_unary: %" PRIu32 "\n", stats->nonzero_unary);
  fprintf(file, "normalization_h: %" PRIu64 "\n", stats->norm_h);
  fprintf(file, "mode: %s\n", qsop_mode_name(stats->mode));
  fprintf(file, "components: %" PRIu32 "\n", stats->components);
  fprintf(file, "max_degree: %" PRIu32 "\n", stats->max_degree);
  fprintf(file, "width_diagnostics: %s\n",
          stats->width_diagnostics_available ? "available" : "unavailable");
  if (stats->width_diagnostics_available) {
    fprintf(file, "min_fill_width: %" PRIu32 "\n", stats->min_fill_width);
    fprintf(file, "min_fill_edges: %" PRIu64 "\n", stats->min_fill_edges);
    fprintf(file, "linear_cut_rank: %" PRIu32 "\n", stats->linear_cut_rank);
  }
  return !write_failed(file, error);
}

bool qsop_stats_write_json(FILE *file, const qsop_stats_t *stats, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    set_error(error, "internal error: null stats JSON write argument");
    return false;
  }

  fprintf(file,
          "{\"modulus\":%" PRIu32 ",\"variables\":%" PRIu32
          ",\"quadratic_terms\":%" PRIu32 ",\"nonzero_unary\":%" PRIu32
          ",\"normalization_h\":%" PRIu64 ",\"mode\":\"%s\",\"components\":%" PRIu32
          ",\"max_degree\":%" PRIu32 ",\"width_diagnostics_available\":%s",
          stats->r, stats->nvars, stats->nedges, stats->nonzero_unary, stats->norm_h,
          qsop_mode_name(stats->mode), stats->components, stats->max_degree,
          stats->width_diagnostics_available ? "true" : "false");
  if (stats->width_diagnostics_available) {
    fprintf(file,
            ",\"min_fill_width\":%" PRIu32 ",\"min_fill_edges\":%" PRIu64
            ",\"linear_cut_rank\":%" PRIu32,
            stats->min_fill_width, stats->min_fill_edges, stats->linear_cut_rank);
  }
  fputs("}\n", file);
  return !write_failed(file, error);
}
