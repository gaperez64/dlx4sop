#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"

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

static bool alloc_graph(const qsop_instance_t *qsop, uint64_t **rowptr_out, uint32_t **colind_out,
                        qsop_error_t *error) {
  uint32_t *degree = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*degree));
  uint64_t *rowptr = calloc((size_t)qsop->nvars + 1U, sizeof(*rowptr));
  uint32_t *cursor = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*cursor));
  uint32_t *colind = calloc(qsop->nedges == 0 ? 1U : (size_t)2U * qsop->nedges, sizeof(*colind));
  if (degree == NULL || rowptr == NULL || cursor == NULL || colind == NULL) {
    free(degree);
    free(rowptr);
    free(cursor);
    free(colind);
    set_error(error, "out of memory while building component graph");
    return false;
  }

  for (uint32_t e = 0; e < qsop->nedges; e++) {
    degree[qsop->edge_u[e]]++;
    degree[qsop->edge_v[e]]++;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    rowptr[v + 1U] = rowptr[v] + degree[v];
    cursor[v] = (uint32_t)rowptr[v];
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint32_t u = qsop->edge_u[e];
    const uint32_t v = qsop->edge_v[e];
    colind[cursor[u]++] = v;
    colind[cursor[v]++] = u;
  }

  free(degree);
  free(cursor);
  *rowptr_out = rowptr;
  *colind_out = colind;
  return true;
}

static bool label_components(const qsop_instance_t *qsop, const uint64_t *rowptr,
                             const uint32_t *colind, uint32_t *component, uint32_t *ncomponents,
                             qsop_error_t *error) {
  uint32_t *queue = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*queue));
  if (queue == NULL) {
    set_error(error, "out of memory while labelling components");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    component[v] = UINT32_MAX;
  }

  uint32_t count = 0;
  for (uint32_t start = 0; start < qsop->nvars; start++) {
    if (component[start] != UINT32_MAX) {
      continue;
    }

    uint32_t head = 0;
    uint32_t tail = 0;
    component[start] = count;
    queue[tail++] = start;
    while (head < tail) {
      const uint32_t v = queue[head++];
      for (uint64_t p = rowptr[v]; p < rowptr[v + 1U]; p++) {
        const uint32_t other = colind[p];
        if (component[other] == UINT32_MAX) {
          component[other] = count;
          queue[tail++] = other;
        }
      }
    }
    count++;
  }

  free(queue);
  *ncomponents = count;
  return true;
}

static void free_subinstance(qsop_instance_t *sub) {
  free(sub->unary);
  free(sub->edge_u);
  free(sub->edge_v);
  free(sub->edge_q);
}

static bool build_subinstance(const qsop_instance_t *qsop, const uint32_t *component,
                              uint32_t wanted, qsop_instance_t *sub, qsop_error_t *error) {
  uint32_t *map = malloc((qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*map));
  if (map == NULL) {
    set_error(error, "out of memory while building component subinstance");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    map[v] = UINT32_MAX;
  }

  uint32_t nvars = 0;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (component[v] == wanted) {
      map[v] = nvars++;
    }
  }

  uint32_t nedges = 0;
  bool sign_only = true;
  const uint32_t sign_coeff = qsop->r / 2U;
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (component[qsop->edge_u[e]] == wanted) {
      nedges++;
      if (qsop->edge_q[e] != sign_coeff) {
        sign_only = false;
      }
    }
  }

  *sub = (qsop_instance_t){
      .r = qsop->r,
      .nvars = nvars,
      .norm_h = 0,
      .constant = 0,
      .mode = sign_only ? QSOP_MODE_SIGN : QSOP_MODE_LABELLED,
      .nedges = nedges,
  };
  sub->unary = calloc(nvars == 0 ? 1U : nvars, sizeof(*sub->unary));
  sub->edge_u = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_u));
  sub->edge_v = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_v));
  sub->edge_q = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_q));
  if (sub->unary == NULL || sub->edge_u == NULL || sub->edge_v == NULL || sub->edge_q == NULL) {
    free(map);
    free_subinstance(sub);
    set_error(error, "out of memory while allocating component subinstance");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (component[v] == wanted) {
      sub->unary[map[v]] = qsop->unary[v];
    }
  }

  uint32_t out_edge = 0;
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (component[qsop->edge_u[e]] == wanted) {
      sub->edge_u[out_edge] = map[qsop->edge_u[e]];
      sub->edge_v[out_edge] = map[qsop->edge_v[e]];
      sub->edge_q[out_edge] = qsop->edge_q[e];
      out_edge++;
    }
  }

  free(map);
  return true;
}

static bool shift_counts(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift,
                         qsop_error_t *error) {
  qsop_counts_clear(r, dst);
  qsop_counts_shift_add(r, dst, src, shift);
  (void)error;
  return true;
}

static void add_saturating_u64(uint64_t *dst, uint64_t value) {
  if (UINT64_MAX - *dst < value) {
    *dst = UINT64_MAX;
  } else {
    *dst += value;
  }
}

bool qsop_solve_components_bruteforce(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                      qsop_result_t **out, qsop_error_t *error) {
  return qsop_solve_components_bruteforce_stats(qsop, max_component_vars, out, NULL, error);
}

bool qsop_solve_components_bruteforce_stats(const qsop_instance_t *qsop,
                                            uint32_t max_component_vars, qsop_result_t **out,
                                            qsop_solve_stats_t *stats, qsop_error_t *error) {
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

  uint64_t *rowptr = NULL;
  uint32_t *colind = NULL;
  uint32_t *component = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*component));
  uint64_t *acc = NULL;
  uint64_t *tmp = NULL;
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (component == NULL || result == NULL || !qsop_counts_alloc(qsop->r, &acc, error) ||
      !qsop_counts_alloc(qsop->r, &tmp, error)) {
    free(component);
    free(result);
    free(acc);
    free(tmp);
    set_error(error, "out of memory while solving components");
    return false;
  }

  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(component);
    free(result);
    free(acc);
    free(tmp);
    return false;
  }

  if (!alloc_graph(qsop, &rowptr, &colind, error)) {
    free(component);
    qsop_result_free(result);
    free(acc);
    free(tmp);
    return false;
  }

  uint32_t ncomponents = 0;
  if (!label_components(qsop, rowptr, colind, component, &ncomponents, error)) {
    free(rowptr);
    free(colind);
    free(component);
    qsop_result_free(result);
    free(acc);
    free(tmp);
    return false;
  }

  acc[0] = 1;
  if (stats != NULL) {
    stats->components = ncomponents;
    if (ncomponents == 0) {
      stats->leaf_assignments = 1;
    }
  }
  for (uint32_t c = 0; c < ncomponents; c++) {
    qsop_instance_t sub = {0};
    qsop_result_t *part = NULL;
    qsop_solve_stats_t part_stats = {0};
    if (!build_subinstance(qsop, component, c, &sub, error) ||
        !qsop_solve_bruteforce_stats(&sub, max_component_vars, &part, &part_stats, error) ||
        !qsop_counts_convolve(qsop->r, tmp, acc, part->counts, error)) {
      free_subinstance(&sub);
      qsop_result_free(part);
      free(rowptr);
      free(colind);
      free(component);
      qsop_result_free(result);
      free(acc);
      free(tmp);
      return false;
    }
    if (stats != NULL) {
      add_saturating_u64(&stats->leaf_assignments, part_stats.leaf_assignments);
    }
    memcpy(acc, tmp, (size_t)qsop->r * sizeof(*acc));
    free_subinstance(&sub);
    qsop_result_free(part);
  }

  if (!shift_counts(qsop->r, result->counts, acc, qsop->constant, error)) {
    free(rowptr);
    free(colind);
    free(component);
    qsop_result_free(result);
    free(acc);
    free(tmp);
    return false;
  }

  free(rowptr);
  free(colind);
  free(component);
  free(acc);
  free(tmp);
  *out = result;
  return true;
}
