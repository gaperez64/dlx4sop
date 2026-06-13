#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "trace.h"

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

typedef struct component_cache_entry {
  uint64_t fingerprint;
  qsop_instance_t key;
  uint64_t *counts;
} component_cache_entry_t;

typedef struct component_cache {
  component_cache_entry_t *entries;
  size_t len;
  size_t cap;
} component_cache_t;

#define SMALL_COMPONENT_CANONICAL_NVARS 5U

static void free_component_cache(component_cache_t *cache) {
  if (cache == NULL) {
    return;
  }

  for (size_t i = 0; i < cache->len; i++) {
    free_subinstance(&cache->entries[i].key);
    free(cache->entries[i].counts);
  }
  free(cache->entries);
  *cache = (component_cache_t){0};
}

static bool same_u32_array(const uint32_t *a, const uint32_t *b, uint32_t len) {
  return len == 0 || memcmp(a, b, (size_t)len * sizeof(*a)) == 0;
}

static bool same_component_key(const qsop_instance_t *a, const qsop_instance_t *b) {
  return a->r == b->r && a->nvars == b->nvars && a->norm_h == b->norm_h &&
         a->constant == b->constant && a->mode == b->mode && a->nedges == b->nedges &&
         same_u32_array(a->unary, b->unary, a->nvars) &&
         same_u32_array(a->edge_u, b->edge_u, a->nedges) &&
         same_u32_array(a->edge_v, b->edge_v, a->nedges) &&
         same_u32_array(a->edge_q, b->edge_q, a->nedges);
}

static uint64_t fingerprint_u64(uint64_t hash, uint64_t value) {
  hash ^= value;
  hash *= UINT64_C(1099511628211);
  return hash;
}

static uint64_t component_fingerprint(const qsop_instance_t *key) {
  uint64_t hash = UINT64_C(1469598103934665603);
  hash = fingerprint_u64(hash, key->r);
  hash = fingerprint_u64(hash, key->nvars);
  hash = fingerprint_u64(hash, key->norm_h);
  hash = fingerprint_u64(hash, key->constant);
  hash = fingerprint_u64(hash, (uint32_t)key->mode);
  hash = fingerprint_u64(hash, key->nedges);
  for (uint32_t v = 0; v < key->nvars; v++) {
    hash = fingerprint_u64(hash, key->unary[v]);
  }
  for (uint32_t e = 0; e < key->nedges; e++) {
    hash = fingerprint_u64(hash, key->edge_u[e]);
    hash = fingerprint_u64(hash, key->edge_v[e]);
    hash = fingerprint_u64(hash, key->edge_q[e]);
  }
  return hash;
}

static component_cache_entry_t *find_cached_component(component_cache_t *cache,
                                                      const qsop_instance_t *key) {
  const uint64_t fingerprint = component_fingerprint(key);
  for (size_t i = 0; i < cache->len; i++) {
    if (cache->entries[i].fingerprint == fingerprint &&
        same_component_key(&cache->entries[i].key, key)) {
      return &cache->entries[i];
    }
  }
  return NULL;
}

static bool copy_component_key(const qsop_instance_t *src, qsop_instance_t *dst,
                               qsop_error_t *error) {
  *dst = (qsop_instance_t){
      .r = src->r,
      .nvars = src->nvars,
      .norm_h = src->norm_h,
      .constant = src->constant,
      .mode = src->mode,
      .nedges = src->nedges,
  };

  dst->unary = malloc((src->nvars == 0 ? 1U : src->nvars) * sizeof(*dst->unary));
  dst->edge_u = malloc((src->nedges == 0 ? 1U : src->nedges) * sizeof(*dst->edge_u));
  dst->edge_v = malloc((src->nedges == 0 ? 1U : src->nedges) * sizeof(*dst->edge_v));
  dst->edge_q = malloc((src->nedges == 0 ? 1U : src->nedges) * sizeof(*dst->edge_q));
  if (dst->unary == NULL || dst->edge_u == NULL || dst->edge_v == NULL || dst->edge_q == NULL) {
    free_subinstance(dst);
    set_error(error, "out of memory while copying component cache key");
    return false;
  }

  memcpy(dst->unary, src->unary, (size_t)src->nvars * sizeof(*dst->unary));
  memcpy(dst->edge_u, src->edge_u, (size_t)src->nedges * sizeof(*dst->edge_u));
  memcpy(dst->edge_v, src->edge_v, (size_t)src->nedges * sizeof(*dst->edge_v));
  memcpy(dst->edge_q, src->edge_q, (size_t)src->nedges * sizeof(*dst->edge_q));
  return true;
}

static bool reserve_component_cache(component_cache_t *cache, size_t needed, qsop_error_t *error) {
  if (needed <= cache->cap) {
    return true;
  }

  size_t new_cap = cache->cap == 0 ? 4U : cache->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "component cache is too large");
      return false;
    }
    new_cap *= 2U;
  }

  component_cache_entry_t *entries = realloc(cache->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    set_error(error, "out of memory while growing component cache");
    return false;
  }
  cache->entries = entries;
  cache->cap = new_cap;
  return true;
}

static bool store_cached_component(component_cache_t *cache, const qsop_instance_t *key,
                                   const uint64_t *counts, qsop_error_t *error) {
  if (!reserve_component_cache(cache, cache->len + 1U, error)) {
    return false;
  }

  component_cache_entry_t *entry = &cache->entries[cache->len];
  *entry = (component_cache_entry_t){0};
  entry->fingerprint = component_fingerprint(key);
  if (!copy_component_key(key, &entry->key, error) ||
      !qsop_counts_alloc(key->r, &entry->counts, error)) {
    free_subinstance(&entry->key);
    free(entry->counts);
    *entry = (component_cache_entry_t){0};
    return false;
  }

  memcpy(entry->counts, counts, (size_t)key->r * sizeof(*entry->counts));
  cache->len++;
  return true;
}

static void swap_u32(uint32_t *a, uint32_t *b) {
  const uint32_t tmp = *a;
  *a = *b;
  *b = tmp;
}

static bool component_edge_less(uint32_t au, uint32_t av, uint32_t aq, uint32_t bu, uint32_t bv,
                                uint32_t bq) {
  if (au != bu) {
    return au < bu;
  }
  if (av != bv) {
    return av < bv;
  }
  return aq < bq;
}

static void sort_component_edges(uint32_t nedges, uint32_t *edge_u, uint32_t *edge_v,
                                 uint32_t *edge_q) {
  for (uint32_t i = 1; i < nedges; i++) {
    uint32_t u = edge_u[i];
    uint32_t v = edge_v[i];
    uint32_t q = edge_q[i];
    uint32_t j = i;
    while (j > 0 && component_edge_less(u, v, q, edge_u[j - 1U], edge_v[j - 1U], edge_q[j - 1U])) {
      edge_u[j] = edge_u[j - 1U];
      edge_v[j] = edge_v[j - 1U];
      edge_q[j] = edge_q[j - 1U];
      j--;
    }
    edge_u[j] = u;
    edge_v[j] = v;
    edge_q[j] = q;
  }
}

static int compare_component_arrays(uint32_t nvars, uint32_t nedges, const uint32_t *lhs_unary,
                                    const uint32_t *lhs_edge_u, const uint32_t *lhs_edge_v,
                                    const uint32_t *lhs_edge_q, const uint32_t *rhs_unary,
                                    const uint32_t *rhs_edge_u, const uint32_t *rhs_edge_v,
                                    const uint32_t *rhs_edge_q) {
  for (uint32_t v = 0; v < nvars; v++) {
    if (lhs_unary[v] != rhs_unary[v]) {
      return lhs_unary[v] < rhs_unary[v] ? -1 : 1;
    }
  }
  for (uint32_t e = 0; e < nedges; e++) {
    if (lhs_edge_u[e] != rhs_edge_u[e]) {
      return lhs_edge_u[e] < rhs_edge_u[e] ? -1 : 1;
    }
    if (lhs_edge_v[e] != rhs_edge_v[e]) {
      return lhs_edge_v[e] < rhs_edge_v[e] ? -1 : 1;
    }
    if (lhs_edge_q[e] != rhs_edge_q[e]) {
      return lhs_edge_q[e] < rhs_edge_q[e] ? -1 : 1;
    }
  }
  return 0;
}

typedef struct small_component_canonicalizer {
  const qsop_instance_t *sub;
  uint32_t *perm;
  bool *used;
  uint32_t *candidate_unary;
  uint32_t *candidate_edge_u;
  uint32_t *candidate_edge_v;
  uint32_t *candidate_edge_q;
  uint32_t *best_unary;
  uint32_t *best_edge_u;
  uint32_t *best_edge_v;
  uint32_t *best_edge_q;
  bool have_best;
} small_component_canonicalizer_t;

static void consider_component_permutation(small_component_canonicalizer_t *ctx) {
  const qsop_instance_t *sub = ctx->sub;
  for (uint32_t v = 0; v < sub->nvars; v++) {
    ctx->candidate_unary[ctx->perm[v]] = sub->unary[v];
  }
  for (uint32_t e = 0; e < sub->nedges; e++) {
    uint32_t u = ctx->perm[sub->edge_u[e]];
    uint32_t v = ctx->perm[sub->edge_v[e]];
    if (u > v) {
      swap_u32(&u, &v);
    }
    ctx->candidate_edge_u[e] = u;
    ctx->candidate_edge_v[e] = v;
    ctx->candidate_edge_q[e] = sub->edge_q[e];
  }
  sort_component_edges(sub->nedges, ctx->candidate_edge_u, ctx->candidate_edge_v,
                       ctx->candidate_edge_q);

  if (ctx->have_best &&
      compare_component_arrays(sub->nvars, sub->nedges, ctx->candidate_unary, ctx->candidate_edge_u,
                               ctx->candidate_edge_v, ctx->candidate_edge_q, ctx->best_unary,
                               ctx->best_edge_u, ctx->best_edge_v, ctx->best_edge_q) >= 0) {
    return;
  }

  memcpy(ctx->best_unary, ctx->candidate_unary, (size_t)sub->nvars * sizeof(*ctx->best_unary));
  memcpy(ctx->best_edge_u, ctx->candidate_edge_u, (size_t)sub->nedges * sizeof(*ctx->best_edge_u));
  memcpy(ctx->best_edge_v, ctx->candidate_edge_v, (size_t)sub->nedges * sizeof(*ctx->best_edge_v));
  memcpy(ctx->best_edge_q, ctx->candidate_edge_q, (size_t)sub->nedges * sizeof(*ctx->best_edge_q));
  ctx->have_best = true;
}

static void enumerate_component_permutations(small_component_canonicalizer_t *ctx, uint32_t depth) {
  if (depth == ctx->sub->nvars) {
    consider_component_permutation(ctx);
    return;
  }

  for (uint32_t next = 0; next < ctx->sub->nvars; next++) {
    if (ctx->used[next]) {
      continue;
    }
    ctx->perm[depth] = next;
    ctx->used[next] = true;
    enumerate_component_permutations(ctx, depth + 1U);
    ctx->used[next] = false;
  }
}

static bool canonicalize_small_component(qsop_instance_t *sub, qsop_error_t *error) {
  if (sub->nvars <= 1U || sub->nvars > SMALL_COMPONENT_CANONICAL_NVARS) {
    return true;
  }

  const size_t nvars_alloc = sub->nvars == 0 ? 1U : sub->nvars;
  const size_t nedges_alloc = sub->nedges == 0 ? 1U : sub->nedges;
  small_component_canonicalizer_t ctx = {
      .sub = sub,
      .perm = malloc(nvars_alloc * sizeof(uint32_t)),
      .used = calloc(nvars_alloc, sizeof(bool)),
      .candidate_unary = malloc(nvars_alloc * sizeof(uint32_t)),
      .candidate_edge_u = malloc(nedges_alloc * sizeof(uint32_t)),
      .candidate_edge_v = malloc(nedges_alloc * sizeof(uint32_t)),
      .candidate_edge_q = malloc(nedges_alloc * sizeof(uint32_t)),
      .best_unary = malloc(nvars_alloc * sizeof(uint32_t)),
      .best_edge_u = malloc(nedges_alloc * sizeof(uint32_t)),
      .best_edge_v = malloc(nedges_alloc * sizeof(uint32_t)),
      .best_edge_q = malloc(nedges_alloc * sizeof(uint32_t)),
  };
  if (ctx.perm == NULL || ctx.used == NULL || ctx.candidate_unary == NULL ||
      ctx.candidate_edge_u == NULL || ctx.candidate_edge_v == NULL ||
      ctx.candidate_edge_q == NULL || ctx.best_unary == NULL || ctx.best_edge_u == NULL ||
      ctx.best_edge_v == NULL || ctx.best_edge_q == NULL) {
    free(ctx.perm);
    free(ctx.used);
    free(ctx.candidate_unary);
    free(ctx.candidate_edge_u);
    free(ctx.candidate_edge_v);
    free(ctx.candidate_edge_q);
    free(ctx.best_unary);
    free(ctx.best_edge_u);
    free(ctx.best_edge_v);
    free(ctx.best_edge_q);
    set_error(error, "out of memory while canonicalizing small component");
    return false;
  }

  enumerate_component_permutations(&ctx, 0);
  memcpy(sub->unary, ctx.best_unary, (size_t)sub->nvars * sizeof(*sub->unary));
  memcpy(sub->edge_u, ctx.best_edge_u, (size_t)sub->nedges * sizeof(*sub->edge_u));
  memcpy(sub->edge_v, ctx.best_edge_v, (size_t)sub->nedges * sizeof(*sub->edge_v));
  memcpy(sub->edge_q, ctx.best_edge_q, (size_t)sub->nedges * sizeof(*sub->edge_q));

  free(ctx.perm);
  free(ctx.used);
  free(ctx.candidate_unary);
  free(ctx.candidate_edge_u);
  free(ctx.candidate_edge_v);
  free(ctx.candidate_edge_q);
  free(ctx.best_unary);
  free(ctx.best_edge_u);
  free(ctx.best_edge_v);
  free(ctx.best_edge_q);
  return true;
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
  if (!canonicalize_small_component(sub, error)) {
    free(map);
    return false;
  }

  free(map);
  return true;
}

static bool shift_counts(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift,
                         qsop_error_t *error) {
  qsop_counts_clear(r, dst);
  return qsop_counts_shift_add_checked(r, dst, src, shift, error);
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
  return qsop_solve_components_bruteforce_trace_stats(qsop, max_component_vars, out, stats, NULL,
                                                      error);
}

bool qsop_solve_components_bruteforce_trace_stats(const qsop_instance_t *qsop,
                                                  uint32_t max_component_vars, qsop_result_t **out,
                                                  qsop_solve_stats_t *stats,
                                                  qsop_solve_trace_t *trace, qsop_error_t *error) {
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
  const uint64_t label_start = qsop_trace_begin(trace);
  if (!label_components(qsop, rowptr, colind, component, &ncomponents, error)) {
    free(rowptr);
    free(colind);
    free(component);
    qsop_result_free(result);
    free(acc);
    free(tmp);
    return false;
  }
  qsop_trace_emit_elapsed(trace, "components.label_components", 0, ncomponents, label_start);

  acc[0] = 1;
  component_cache_t cache = {0};
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
    if (!build_subinstance(qsop, component, c, &sub, error)) {
      free_subinstance(&sub);
      free_component_cache(&cache);
      free(rowptr);
      free(colind);
      free(component);
      qsop_result_free(result);
      free(acc);
      free(tmp);
      return false;
    }

    const uint64_t lookup_start = qsop_trace_begin(trace);
    const component_cache_entry_t *cached = find_cached_component(&cache, &sub);
    qsop_trace_emit_elapsed(trace, "components.cache_lookup", 0, cache.len, lookup_start);
    const uint64_t *part_counts = NULL;
    if (cached != NULL) {
      part_counts = cached->counts;
      if (stats != NULL) {
        stats->cache_hits++;
      }
    } else {
      const uint64_t solve_start = qsop_trace_begin(trace);
      const bool solved = qsop_solve_bruteforce_trace_stats(&sub, max_component_vars, &part,
                                                            &part_stats, trace, error);
      if (!solved || !store_cached_component(&cache, &sub, part->counts, error)) {
        free_subinstance(&sub);
        qsop_result_free(part);
        free_component_cache(&cache);
        free(rowptr);
        free(colind);
        free(component);
        qsop_result_free(result);
        free(acc);
        free(tmp);
        return false;
      }
      qsop_trace_emit_elapsed(trace, "components.solve_component", 0, sub.nvars, solve_start);
      part_counts = part->counts;
      if (stats != NULL) {
        stats->cache_misses++;
        add_saturating_u64(&stats->leaf_assignments, part_stats.leaf_assignments);
      }
    }

    const uint64_t convolve_start = qsop_trace_begin(trace);
    if (!qsop_counts_convolve(qsop->r, tmp, acc, part_counts, error)) {
      free_subinstance(&sub);
      qsop_result_free(part);
      free_component_cache(&cache);
      free(rowptr);
      free(colind);
      free(component);
      qsop_result_free(result);
      free(acc);
      free(tmp);
      return false;
    }
    qsop_trace_emit_elapsed(trace, "components.convolution", 0, qsop->r, convolve_start);

    memcpy(acc, tmp, (size_t)qsop->r * sizeof(*acc));
    free_subinstance(&sub);
    qsop_result_free(part);
  }

  if (!shift_counts(qsop->r, result->counts, acc, qsop->constant, error)) {
    free_component_cache(&cache);
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
  free_component_cache(&cache);
  free(acc);
  free(tmp);
  *out = result;
  return true;
}
