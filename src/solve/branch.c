#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residual.h"
#include "dlx4sop/residue.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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

typedef struct branch_search_stats {
  uint64_t nodes;
  uint64_t leaves;
  uint64_t *work;
  uint64_t *tmp;
} branch_search_stats_t;

static void add_saturating_u64(uint64_t *dst, uint64_t value) {
  if (UINT64_MAX - *dst < value) {
    *dst = UINT64_MAX;
  } else {
    *dst += value;
  }
}

static uint64_t assignment_count(uint32_t nvars) {
  if (nvars >= 63U) {
    return UINT64_MAX;
  }
  return UINT64_C(1) << nvars;
}

static bool choose_branch_var(const qsop_residual_t *residual, uint32_t *out,
                              qsop_error_t *error) {
  const uint32_t nvars = qsop_residual_nvars(residual);
  bool found = false;
  uint32_t best_var = 0;
  uint32_t best_components = 0;
  uint32_t best_degree = 0;
  bool best_has_unary = false;

  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_residual_var_active(residual, v)) {
      uint32_t components = 0;
      if (!qsop_residual_components_without_var(residual, v, &components, error)) {
        return false;
      }
      const uint32_t degree = qsop_residual_active_degree(residual, v);
      const bool has_unary = qsop_residual_unary(residual, v) != 0;
      if (!found || components > best_components ||
          (components == best_components && degree > best_degree) ||
          (components == best_components && degree == best_degree && has_unary && !best_has_unary)) {
        found = true;
        best_var = v;
        best_components = components;
        best_degree = degree;
        best_has_unary = has_unary;
      }
    }
  }

  if (!found) {
    set_error(error, "residual active-var count disagrees with active flags");
    return false;
  }

  *out = best_var;
  return true;
}

static void edge_free_sum(const qsop_residual_t *residual, uint64_t *counts,
                          branch_search_stats_t *stats) {
  const uint32_t r = qsop_residual_modulus(residual);
  uint64_t *current = stats->work;
  uint64_t *next = stats->tmp;
  qsop_counts_clear(r, current);
  current[qsop_residual_constant(residual)] = 1;

  for (uint32_t v = 0; v < qsop_residual_nvars(residual); v++) {
    if (!qsop_residual_var_active(residual, v)) {
      continue;
    }

    qsop_counts_clear(r, next);
    const uint32_t unary = qsop_residual_unary(residual, v);
    for (uint32_t residue = 0; residue < r; residue++) {
      const uint64_t count = current[residue];
      if (count == 0) {
        continue;
      }
      next[residue] += count;
      next[((uint64_t)residue + unary) % r] += count;
    }

    uint64_t *swap = current;
    current = next;
    next = swap;
  }

  for (uint32_t residue = 0; residue < r; residue++) {
    counts[residue] += current[residue];
  }
  add_saturating_u64(&stats->leaves, assignment_count(qsop_residual_active_vars(residual)));
}

static bool branch_sum_rec(qsop_residual_t *residual, uint64_t *counts,
                           branch_search_stats_t *stats, qsop_error_t *error) {
  stats->nodes++;
  if (qsop_residual_active_vars(residual) == 0) {
    stats->leaves++;
    counts[qsop_residual_constant(residual)]++;
    return true;
  }
  if (qsop_residual_active_edges(residual) == 0) {
    edge_free_sum(residual, counts, stats);
    return true;
  }

  uint32_t v = 0;
  if (!choose_branch_var(residual, &v, error)) {
    return false;
  }

  for (uint8_t value = 0; value <= 1U; value++) {
    const size_t checkpoint = qsop_residual_checkpoint(residual);
    if (!qsop_residual_branch(residual, v, value, error)) {
      return false;
    }
    if (!branch_sum_rec(residual, counts, stats, error)) {
      return false;
    }
    if (!qsop_residual_undo(residual, checkpoint, error)) {
      return false;
    }
  }

  return true;
}

bool qsop_solve_residual_branch(const qsop_instance_t *qsop, uint32_t max_vars,
                                qsop_result_t **out, qsop_error_t *error) {
  return qsop_solve_residual_branch_stats(qsop, max_vars, out, NULL, error);
}

bool qsop_solve_residual_branch_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                      qsop_result_t **out, qsop_solve_stats_t *stats,
                                      qsop_error_t *error) {
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
  if (qsop->nvars > max_vars) {
    set_error(error,
              "residual branch solver refuses %" PRIu32
              " variables; pass a larger --max-vars or use a future backend",
              qsop->nvars);
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  qsop_residual_t *residual = NULL;
  if (result == NULL) {
    set_error(error, "out of memory while allocating result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error) ||
      !qsop_residual_create(qsop, &residual, error)) {
    qsop_result_free(result);
    qsop_residual_free(residual);
    return false;
  }

  branch_search_stats_t search = {0};
  if (!qsop_counts_alloc(qsop->r, &search.work, error) ||
      !qsop_counts_alloc(qsop->r, &search.tmp, error)) {
    qsop_result_free(result);
    qsop_residual_free(residual);
    free(search.work);
    free(search.tmp);
    return false;
  }
  if (!branch_sum_rec(residual, result->counts, &search, error)) {
    qsop_result_free(result);
    qsop_residual_free(residual);
    free(search.work);
    free(search.tmp);
    return false;
  }
  if (stats != NULL) {
    stats->search_nodes = search.nodes;
    stats->leaf_assignments = search.leaves;
  }

  qsop_residual_free(residual);
  free(search.work);
  free(search.tmp);
  *out = result;
  return true;
}
