#include "branch_shadow.h"

#include "../core/qsop_internal.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>

static bool branch_shadow_adjlist_push(branch_shadow_adjlist_t *a, uint32_t x,
                                       qsop_error_t *error) {
  if (a->len == a->cap) {
    const uint32_t ncap = a->cap == 0U ? 4U : (a->cap > UINT32_MAX / 2U ? UINT32_MAX : a->cap * 2U);
    uint32_t *nd = realloc(a->data, (size_t)ncap * sizeof(*nd));
    if (nd == NULL) {
      qsop_set_error(error, "out of memory while growing shadow adjacency list");
      return false;
    }
    a->data = nd;
    a->cap = ncap;
  }
  a->data[a->len++] = x;
  return true;
}

static void branch_shadow_adjlist_remove(branch_shadow_adjlist_t *a, uint32_t x) {
  for (uint32_t i = 0; i < a->len; i++) {
    if (a->data[i] == x) {
      a->data[i] = a->data[a->len - 1U];
      a->len--;
      return;
    }
  }
}

static bool branch_shadow_alloc(branch_shadow_t *shadow, uint32_t nvars, qsop_error_t *error) {
  *shadow = (branch_shadow_t){0};
  shadow->nvars = nvars;
  shadow->alive = calloc(nvars == 0U ? 1U : nvars, sizeof(*shadow->alive));
  shadow->adj = calloc(nvars == 0U ? 1U : nvars, sizeof(*shadow->adj));
  if (shadow->alive == NULL || shadow->adj == NULL) {
    branch_shadow_free(shadow);
    qsop_set_error(error, "out of memory while allocating shadow graph");
    return false;
  }
  return true;
}

void branch_shadow_free(branch_shadow_t *shadow) {
  if (shadow == NULL) {
    return;
  }
  if (shadow->adj != NULL) {
    for (uint32_t v = 0; v < shadow->nvars; v++) {
      free(shadow->adj[v].data);
    }
  }
  free(shadow->adj);
  free(shadow->alive);
  *shadow = (branch_shadow_t){0};
}

bool branch_shadow_add_edge(branch_shadow_t *shadow, uint32_t u, uint32_t v, qsop_error_t *error) {
  if (u == v || u >= shadow->nvars || v >= shadow->nvars) {
    return true; /* self-loops are dropped, matching qsop_min_fill_eliminate's contract */
  }
  branch_shadow_adjlist_t *au = &shadow->adj[u];
  for (uint32_t i = 0; i < au->len; i++) {
    if (au->data[i] == v) {
      return true; /* already adjacent: simple graph, no duplicate neighbours */
    }
  }
  if (!branch_shadow_adjlist_push(&shadow->adj[u], v, error)) {
    return false;
  }
  if (!branch_shadow_adjlist_push(&shadow->adj[v], u, error)) {
    branch_shadow_adjlist_remove(&shadow->adj[u], v); /* keep both sides consistent */
    return false;
  }
  shadow->alive_edges++;
  return true;
}

void branch_shadow_remove_vertex(branch_shadow_t *shadow, uint32_t v) {
  if (v >= shadow->nvars || !shadow->alive[v]) {
    return;
  }
  branch_shadow_adjlist_t *av = &shadow->adj[v];
  for (uint32_t i = 0; i < av->len; i++) {
    branch_shadow_adjlist_remove(&shadow->adj[av->data[i]], v);
    shadow->alive_edges--;
  }
  free(av->data);
  *av = (branch_shadow_adjlist_t){0};
  shadow->alive[v] = false;
  shadow->alive_vars--;
}

bool branch_shadow_clone(const branch_shadow_t *src, branch_shadow_t *out, qsop_error_t *error) {
  if (!branch_shadow_alloc(out, src->nvars, error)) {
    return false;
  }
  if (src->nvars != 0U) {
    memcpy(out->alive, src->alive, (size_t)src->nvars * sizeof(*out->alive));
  }
  out->alive_vars = src->alive_vars;
  out->alive_edges = src->alive_edges;
  for (uint32_t v = 0; v < src->nvars; v++) {
    const branch_shadow_adjlist_t *sadj = &src->adj[v];
    if (sadj->len == 0U) {
      continue;
    }
    out->adj[v].data = malloc((size_t)sadj->len * sizeof(*out->adj[v].data));
    if (out->adj[v].data == NULL) {
      branch_shadow_free(out);
      qsop_set_error(error, "out of memory while cloning shadow graph");
      return false;
    }
    memcpy(out->adj[v].data, sadj->data, (size_t)sadj->len * sizeof(*out->adj[v].data));
    out->adj[v].len = sadj->len;
    out->adj[v].cap = sadj->len;
  }
  return true;
}

bool branch_shadow_build_from_edges(uint32_t nvars, const uint32_t *edge_u, const uint32_t *edge_v,
                                    uint32_t nedges, branch_shadow_t *out, qsop_error_t *error) {
  if (!branch_shadow_alloc(out, nvars, error)) {
    return false;
  }
  for (uint32_t v = 0; v < nvars; v++) {
    out->alive[v] = true;
  }
  out->alive_vars = nvars;
  for (uint32_t e = 0; e < nedges; e++) {
    const uint32_t u = edge_u[e];
    const uint32_t v = edge_v[e];
    if (u >= nvars || v >= nvars) {
      continue;
    }
    if (!branch_shadow_add_edge(out, u, v, error)) {
      branch_shadow_free(out);
      return false;
    }
  }
  return true;
}

bool branch_shadow_build(const qsop_residual_t *residual, branch_shadow_t *out,
                         qsop_error_t *error) {
  if (residual == NULL) {
    qsop_set_error(error, "internal error: null residual for shadow build");
    return false;
  }
  const uint32_t nvars = qsop_residual_nvars(residual);
  if (!branch_shadow_alloc(out, nvars, error)) {
    return false;
  }
  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_residual_var_active(residual, v)) {
      out->alive[v] = true;
      out->alive_vars++;
    }
  }
  const uint32_t nedges = qsop_residual_nedges(residual);
  for (uint32_t e = 0; e < nedges; e++) {
    if (!qsop_residual_edge_active(residual, e)) {
      continue;
    }
    const uint32_t u = qsop_residual_edge_u(residual, e);
    const uint32_t v = qsop_residual_edge_v(residual, e);
    if (u >= nvars || v >= nvars || !out->alive[u] || !out->alive[v]) {
      continue; /* defensive: shouldn't happen for a well-formed residual */
    }
    if (!branch_shadow_add_edge(out, u, v, error)) {
      branch_shadow_free(out);
      return false;
    }
  }
  return true;
}

bool branch_shadow_series_reduce(branch_shadow_t *shadow, qsop_error_t *error) {
  branch_shadow_adjlist_t wl = {0}; /* growable worklist; duplicates tolerated, revalidated on pop */
  for (uint32_t v = 0; v < shadow->nvars; v++) {
    if (shadow->alive[v] && shadow->adj[v].len <= 2U) {
      if (!branch_shadow_adjlist_push(&wl, v, error)) {
        free(wl.data);
        return false;
      }
    }
  }
  uint32_t head = 0;
  while (head < wl.len) {
    const uint32_t v = wl.data[head++];
    if (!shadow->alive[v] || shadow->adj[v].len > 2U) {
      continue;
    }
    const uint32_t degree = shadow->adj[v].len;
    if (degree == 2U) {
      const uint32_t u = shadow->adj[v].data[0];
      const uint32_t w = shadow->adj[v].data[1]; /* always != u: adjacency has no duplicates */
      branch_shadow_remove_vertex(shadow, v);
      if (u != w && !branch_shadow_add_edge(shadow, u, w, error)) {
        free(wl.data);
        return false;
      }
      if (shadow->alive[u] && shadow->adj[u].len <= 2U &&
          !branch_shadow_adjlist_push(&wl, u, error)) {
        free(wl.data);
        return false;
      }
      if (u != w && shadow->alive[w] && shadow->adj[w].len <= 2U &&
          !branch_shadow_adjlist_push(&wl, w, error)) {
        free(wl.data);
        return false;
      }
    } else {
      /* degree 0 or 1 */
      const bool has_neighbor = degree == 1U;
      const uint32_t neighbor = has_neighbor ? shadow->adj[v].data[0] : 0U;
      branch_shadow_remove_vertex(shadow, v);
      if (has_neighbor && shadow->alive[neighbor] && shadow->adj[neighbor].len <= 2U &&
          !branch_shadow_adjlist_push(&wl, neighbor, error)) {
        free(wl.data);
        return false;
      }
    }
  }
  free(wl.data);
  return true;
}

bool branch_shadow_component_stats(const branch_shadow_t *shadow, uint32_t *out_components,
                                   uint32_t *out_largest, qsop_error_t *error) {
  *out_components = 0;
  *out_largest = 0;
  if (shadow->nvars == 0U) {
    return true;
  }
  bool *visited = calloc(shadow->nvars, sizeof(*visited));
  uint32_t *stack = malloc((size_t)shadow->nvars * sizeof(*stack));
  if (visited == NULL || stack == NULL) {
    free(visited);
    free(stack);
    qsop_set_error(error, "out of memory while measuring shadow component shape");
    return false;
  }
  for (uint32_t v = 0; v < shadow->nvars; v++) {
    if (!shadow->alive[v] || visited[v]) {
      continue;
    }
    uint32_t top = 0;
    stack[top++] = v;
    visited[v] = true;
    uint32_t size = 0;
    while (top > 0U) {
      const uint32_t x = stack[--top];
      size++;
      const branch_shadow_adjlist_t *adj = &shadow->adj[x];
      for (uint32_t i = 0; i < adj->len; i++) {
        const uint32_t n = adj->data[i];
        if (!visited[n]) {
          visited[n] = true;
          stack[top++] = n;
        }
      }
    }
    (*out_components)++;
    if (size > *out_largest) {
      *out_largest = size;
    }
  }
  free(visited);
  free(stack);
  return true;
}

bool branch_shadow_candidate_better(const branch_shadow_candidate_t *a,
                                    const branch_shadow_candidate_t *b) {
  if (a->largest_component != b->largest_component) {
    return a->largest_component < b->largest_component;
  }
  if (a->remaining_vars != b->remaining_vars) {
    return a->remaining_vars < b->remaining_vars;
  }
  if (a->remaining_edges != b->remaining_edges) {
    return a->remaining_edges < b->remaining_edges;
  }
  if (a->eliminated_vars != b->eliminated_vars) {
    return a->eliminated_vars > b->eliminated_vars;
  }
  if (a->original_degree != b->original_degree) {
    return a->original_degree > b->original_degree;
  }
  return a->var < b->var;
}

bool branch_shadow_score_candidate(const branch_shadow_t *reduced, uint32_t v,
                                   branch_shadow_candidate_t *out, qsop_error_t *error) {
  *out = (branch_shadow_candidate_t){0};
  out->var = v;
  out->original_degree = reduced->adj[v].len;

  branch_shadow_t clone = {0};
  if (!branch_shadow_clone(reduced, &clone, error)) {
    return false;
  }
  branch_shadow_remove_vertex(&clone, v);
  if (!branch_shadow_series_reduce(&clone, error)) {
    branch_shadow_free(&clone);
    return false;
  }
  out->remaining_vars = clone.alive_vars;
  out->remaining_edges = clone.alive_edges;
  out->eliminated_vars = reduced->alive_vars - clone.alive_vars;
  const bool ok =
      branch_shadow_component_stats(&clone, &out->components, &out->largest_component, error);
  branch_shadow_free(&clone);
  return ok;
}

/* Keeps the `limit` vertices from `pool` with the highest current shadow degree (ties by lowest
 * ID) via a top-K insertion, O(pool_len * limit) instead of an O(pool_len log pool_len) full sort
 * -- cheaper when limit is small (BRANCH_SHADOW_MAX_CANDIDATE_EVALS) relative to a pool that
 * hasn't shrunk much. Returns a freshly allocated array; frees `pool` itself. */
static uint32_t *branch_shadow_select_degree_pool(const branch_shadow_t *shadow, uint32_t *pool,
                                                   uint32_t pool_len, uint32_t limit,
                                                   uint32_t *out_len, qsop_error_t *error) {
  uint32_t *top = malloc((size_t)limit * sizeof(*top));
  if (top == NULL) {
    free(pool);
    qsop_set_error(error, "out of memory while filtering shadow candidate pool");
    *out_len = 0;
    return NULL;
  }
  uint32_t top_len = 0;
  for (uint32_t i = 0; i < pool_len; i++) {
    const uint32_t v = pool[i];
    const uint32_t vdeg = shadow->adj[v].len;
    uint32_t pos = top_len;
    if (top_len < limit) {
      top_len++;
    } else {
      const uint32_t worst = top[top_len - 1U];
      const bool v_better =
          vdeg > shadow->adj[worst].len || (vdeg == shadow->adj[worst].len && v < worst);
      if (!v_better) {
        continue;
      }
      pos = top_len - 1U;
    }
    top[pos] = v;
    while (pos > 0U) {
      const uint32_t a = top[pos];
      const uint32_t b = top[pos - 1U];
      const bool a_better =
          shadow->adj[a].len > shadow->adj[b].len || (shadow->adj[a].len == shadow->adj[b].len && a < b);
      if (!a_better) {
        break;
      }
      top[pos] = b;
      top[pos - 1U] = a;
      pos--;
    }
  }
  free(pool);
  *out_len = top_len;
  return top;
}

bool branch_shadow_shortlist(const qsop_residual_t *residual, uint32_t limit, uint32_t **out_vars,
                             uint32_t *out_len, qsop_solve_stats_t *stats, qsop_error_t *error) {
  *out_vars = NULL;
  *out_len = 0;
  if (limit == 0U || residual == NULL) {
    return true;
  }

  const uint32_t active_vars = qsop_residual_active_vars(residual);
  const uint32_t active_edges = qsop_residual_active_edges(residual);
  if (active_vars > BRANCH_SHADOW_MAX_SOURCE_VARS || active_edges > BRANCH_SHADOW_MAX_SOURCE_EDGES) {
    if (stats != NULL) {
      stats->branch_shadow_skips++;
    }
    return true;
  }

  const uint64_t build_start_ns = qsop_trace_now_ns();
  branch_shadow_t shadow = {0};
  if (!branch_shadow_build(residual, &shadow, error)) {
    return false;
  }
  if (stats != NULL) {
    stats->branch_shadow_builds++;
    if (active_vars > stats->branch_shadow_max_source_vars) {
      stats->branch_shadow_max_source_vars = active_vars;
    }
  }
  if (!branch_shadow_series_reduce(&shadow, error)) {
    branch_shadow_free(&shadow);
    return false;
  }
  if (stats != NULL && shadow.alive_vars > stats->branch_shadow_max_core_vars) {
    stats->branch_shadow_max_core_vars = shadow.alive_vars;
  }

  if (shadow.alive_vars == 0U) {
    branch_shadow_free(&shadow);
    if (stats != NULL) {
      stats->branch_shadow_build_ns += qsop_trace_elapsed_ns(build_start_ns);
    }
    return true;
  }

  uint32_t *pool = malloc((size_t)shadow.alive_vars * sizeof(*pool));
  if (pool == NULL) {
    branch_shadow_free(&shadow);
    qsop_set_error(error, "out of memory while building shadow candidate pool");
    return false;
  }
  uint32_t pool_len = 0;
  for (uint32_t v = 0; v < shadow.nvars; v++) {
    if (shadow.alive[v]) {
      pool[pool_len++] = v;
    }
  }
  if (pool_len > BRANCH_SHADOW_MAX_CANDIDATE_EVALS) {
    pool = branch_shadow_select_degree_pool(&shadow, pool, pool_len,
                                            BRANCH_SHADOW_MAX_CANDIDATE_EVALS, &pool_len, error);
    if (pool == NULL) {
      branch_shadow_free(&shadow);
      return false;
    }
  }

  branch_shadow_candidate_t *top = calloc(limit, sizeof(*top));
  if (top == NULL) {
    free(pool);
    branch_shadow_free(&shadow);
    qsop_set_error(error, "out of memory while scoring shadow candidates");
    return false;
  }
  uint32_t top_len = 0;
  bool ok = true;
  for (uint32_t i = 0; ok && i < pool_len; i++) {
    branch_shadow_candidate_t candidate = {0};
    ok = branch_shadow_score_candidate(&shadow, pool[i], &candidate, error);
    if (!ok) {
      break;
    }
    uint32_t pos = top_len;
    if (top_len < limit) {
      top_len++;
    } else if (!branch_shadow_candidate_better(&candidate, &top[top_len - 1U])) {
      continue;
    } else {
      pos = top_len - 1U;
    }
    top[pos] = candidate;
    while (pos > 0U && branch_shadow_candidate_better(&top[pos], &top[pos - 1U])) {
      const branch_shadow_candidate_t tmp = top[pos - 1U];
      top[pos - 1U] = top[pos];
      top[pos] = tmp;
      pos--;
    }
  }
  free(pool);
  branch_shadow_free(&shadow);
  if (stats != NULL) {
    stats->branch_shadow_build_ns += qsop_trace_elapsed_ns(build_start_ns);
  }
  if (!ok) {
    free(top);
    return false;
  }
  if (top_len == 0U) {
    free(top);
    return true;
  }

  uint32_t *vars = malloc((size_t)top_len * sizeof(*vars));
  if (vars == NULL) {
    free(top);
    qsop_set_error(error, "out of memory while finalizing shadow shortlist");
    return false;
  }
  uint32_t vlen = 0;
  for (uint32_t i = 0; i < top_len; i++) {
    /* Defensive: every shadow vertex is seeded 1:1 from an active residual variable and
     * elimination never revives one, so this should always hold. */
    if (qsop_residual_var_active(residual, top[i].var)) {
      vars[vlen++] = top[i].var;
    }
  }
  free(top);
  if (vlen == 0U) {
    free(vars);
    return true;
  }
  *out_vars = vars;
  *out_len = vlen;
  return true;
}
