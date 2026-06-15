#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "component_key.h"
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

typedef struct component_solve_context {
  uint64_t count_modulus;
} component_solve_context_t;

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
  if (!qsop_canonicalize_small_component(sub, SMALL_COMPONENT_CANONICAL_NVARS, error)) {
    free(map);
    return false;
  }

  free(map);
  return true;
}

static bool component_count_add(const component_solve_context_t *ctx, uint64_t *dst,
                                uint64_t value, qsop_error_t *error) {
  if (ctx->count_modulus != 0) {
    *dst = qsop_mod_add_u64(*dst, value % ctx->count_modulus, ctx->count_modulus);
    return true;
  }
  return qsop_count_add(dst, value, error);
}

static bool component_count_mul(const component_solve_context_t *ctx, uint64_t left,
                                uint64_t right, uint64_t *out, qsop_error_t *error) {
  if (ctx->count_modulus != 0) {
    *out = qsop_mod_mul_u64(left, right, ctx->count_modulus);
    return true;
  }
  return qsop_count_mul(left, right, out, error);
}

static bool component_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                                      const uint64_t *right,
                                      const component_solve_context_t *ctx,
                                      qsop_error_t *error) {
  if (r == 0 || dst == NULL || left == NULL || right == NULL) {
    set_error(error, "internal error: invalid component residue convolution argument");
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
      if (!component_count_mul(ctx, left[a], right[b], &product, error) ||
          !component_count_add(ctx, &dst[target], product, error)) {
        return false;
      }
    }
  }
  return true;
}

static bool shift_counts(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift,
                         const component_solve_context_t *ctx, qsop_error_t *error) {
  qsop_counts_clear(r, dst);
  if (r == 0 || dst == NULL || src == NULL) {
    set_error(error, "internal error: invalid component residue shift-add argument");
    return false;
  }

  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t target = residue + delta;
    if (target >= r) {
      target -= r;
    }
    if (!component_count_add(ctx, &dst[target], src[residue], error)) {
      return false;
    }
  }
  return true;
}

static bool component_counts_to_fourier(uint32_t r, const uint64_t *counts,
                                        const uint64_t *powers, uint64_t prime,
                                        uint64_t *modes, qsop_error_t *error) {
  if (r == 0 || counts == NULL || powers == NULL || modes == NULL) {
    set_error(error, "internal error: invalid component Fourier transform argument");
    return false;
  }
  qsop_counts_clear(r, modes);
  for (uint32_t mode = 0; mode < r; mode++) {
    for (uint32_t residue = 0; residue < r; residue++) {
      if (counts[residue] == 0) {
        continue;
      }
      const uint64_t value =
          qsop_mod_mul_u64(counts[residue] % prime, powers[(size_t)mode * r + residue], prime);
      modes[mode] = qsop_mod_add_u64(modes[mode], value, prime);
    }
  }
  return true;
}

static bool component_fourier_multiply(uint32_t r, uint64_t *acc, const uint64_t *part,
                                       uint64_t prime, qsop_error_t *error) {
  if (r == 0 || acc == NULL || part == NULL) {
    set_error(error, "internal error: invalid component Fourier multiply argument");
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    acc[mode] = qsop_mod_mul_u64(acc[mode], part[mode], prime);
  }
  return true;
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

static uint64_t component_cache_key_bytes(const component_cache_t *cache) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < cache->len; i++) {
    const qsop_instance_t *key = &cache->entries[i].key;
    const uint32_t nvars_alloc = key->nvars == 0 ? 1U : key->nvars;
    const uint32_t nedges_alloc = key->nedges == 0 ? 1U : key->nedges;
    add_saturating_u64(&bytes,
                       saturating_mul_u64((uint64_t)nvars_alloc, sizeof(*key->unary)));
    add_saturating_u64(
        &bytes, saturating_mul_u64(saturating_mul_u64((uint64_t)nedges_alloc, 3U),
                                   sizeof(*key->edge_u)));
  }
  return bytes;
}

static uint64_t component_cache_count_bytes(const component_cache_t *cache) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < cache->len; i++) {
    add_saturating_u64(
        &bytes, saturating_mul_u64((uint64_t)cache->entries[i].key.r,
                                   sizeof(*cache->entries[i].counts)));
  }
  return bytes;
}

static uint64_t component_cache_estimated_bytes(const component_cache_t *cache) {
  uint64_t bytes = saturating_mul_u64((uint64_t)cache->cap, sizeof(*cache->entries));
  add_saturating_u64(&bytes, component_cache_key_bytes(cache));
  add_saturating_u64(&bytes, component_cache_count_bytes(cache));
  return bytes;
}

static bool solve_components_once(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                  uint64_t count_modulus, uint64_t *counts,
                                  qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                  qsop_error_t *error);

static bool solve_components_fourier(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                     qsop_result_t **out, qsop_solve_stats_t *stats,
                                     qsop_solve_trace_t *trace, qsop_error_t *error);

static bool solve_components_crt(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                 qsop_result_t **out, qsop_solve_stats_t *stats,
                                 qsop_solve_trace_t *trace, qsop_error_t *error);

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
  return qsop_solve_components_bruteforce_mode_trace_stats(
      qsop, max_component_vars, QSOP_SOLVE_MODE_COUNT_TABLE, out, stats, trace, error);
}

bool qsop_solve_components_bruteforce_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_component_vars, qsop_solve_mode_t mode,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (mode == QSOP_SOLVE_MODE_FOURIER) {
    return solve_components_fourier(qsop, max_component_vars, out, stats, trace, error);
  }
  if (mode != QSOP_SOLVE_MODE_COUNT_TABLE) {
    set_error(error, "internal error: unsupported components solve mode");
    return false;
  }
  if (qsop->nvars >= 64U) {
    return solve_components_crt(qsop, max_component_vars, out, stats, trace, error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    set_error(error, "out of memory while allocating components result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    return false;
  }
  if (!solve_components_once(qsop, max_component_vars, 0, result->counts, stats, trace, error)) {
    qsop_result_free(result);
    return false;
  }
  *out = result;
  return true;
}

static bool solve_components_once(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                  uint64_t count_modulus, uint64_t *counts,
                                  qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                  qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }

  component_solve_context_t ctx = {
      .count_modulus = count_modulus,
  };
  uint64_t *rowptr = NULL;
  uint32_t *colind = NULL;
  uint32_t *component = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*component));
  uint64_t *acc = NULL;
  uint64_t *tmp = NULL;
  if (component == NULL || !qsop_counts_alloc(qsop->r, &acc, error) ||
      !qsop_counts_alloc(qsop->r, &tmp, error)) {
    free(component);
    free(acc);
    free(tmp);
    set_error(error, "out of memory while solving components");
    return false;
  }

  if (!alloc_graph(qsop, &rowptr, &colind, error)) {
    free(component);
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
      if (!solved) {
        free_subinstance(&sub);
        qsop_result_free(part);
        free_component_cache(&cache);
        free(rowptr);
        free(colind);
        free(component);
        free(acc);
        free(tmp);
        return false;
      }
      qsop_trace_emit_elapsed(trace, "components.solve_component", 0, sub.nvars, solve_start);
      const uint64_t store_start = qsop_trace_begin(trace);
      if (!store_cached_component(&cache, &sub, part->counts, error)) {
        free_subinstance(&sub);
        qsop_result_free(part);
        free_component_cache(&cache);
        free(rowptr);
        free(colind);
        free(component);
        free(acc);
        free(tmp);
        return false;
      }
      qsop_trace_emit_elapsed(trace, "components.cache_store", 0, cache.len, store_start);
      part_counts = part->counts;
      if (stats != NULL) {
        stats->cache_misses++;
        add_saturating_u64(&stats->leaf_assignments, part_stats.leaf_assignments);
      }
    }

    const uint64_t convolve_start = qsop_trace_begin(trace);
    if (!component_counts_convolve(qsop->r, tmp, acc, part_counts, &ctx, error)) {
      free_subinstance(&sub);
      qsop_result_free(part);
      free_component_cache(&cache);
      free(rowptr);
      free(colind);
      free(component);
      free(acc);
      free(tmp);
      return false;
    }
    qsop_trace_emit_elapsed(trace, "components.convolution", 0, qsop->r, convolve_start);

    memcpy(acc, tmp, (size_t)qsop->r * sizeof(*acc));
    free_subinstance(&sub);
    qsop_result_free(part);
  }

  if (!shift_counts(qsop->r, counts, acc, qsop->constant, &ctx, error)) {
    free_component_cache(&cache);
    free(rowptr);
    free(colind);
    free(component);
    free(acc);
    free(tmp);
    return false;
  }

  if (stats != NULL) {
    stats->cache_entries = (uint64_t)cache.len;
    stats->cache_stored_residue_slots = saturating_mul_u64((uint64_t)cache.len, qsop->r);
    stats->cache_key_bytes = component_cache_key_bytes(&cache);
    stats->cache_count_bytes = component_cache_count_bytes(&cache);
    stats->cache_estimated_bytes = component_cache_estimated_bytes(&cache);
  }

  free(rowptr);
  free(colind);
  free(component);
  free_component_cache(&cache);
  free(acc);
  free(tmp);
  return true;
}

static bool solve_components_fourier(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                     qsop_result_t **out, qsop_solve_stats_t *stats,
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
  if (qsop->nvars >= 64U) {
    set_error(error,
              "components Fourier mode currently requires fewer than 64 variables; use "
              "count-table mode for CRT-backed larger solves");
    return false;
  }

  uint64_t prime = 0;
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;
  uint64_t *acc = NULL;
  uint64_t *part_modes = NULL;
  if (!qsop_fourier_find_ntt_prime(qsop->r, qsop->nvars, &prime, error) ||
      !qsop_fourier_find_order_root(prime, qsop->r, &root, error)) {
    return false;
  }
  inv_root = qsop_mod_pow_u64(root, prime - 2U, prime);
  if (!qsop_fourier_make_root_powers(qsop->r, root, prime, &powers, error) ||
      !qsop_fourier_make_root_powers(qsop->r, inv_root, prime, &inv_powers, error) ||
      !qsop_counts_alloc(qsop->r, &acc, error) || !qsop_counts_alloc(qsop->r, &part_modes, error)) {
    free(powers);
    free(inv_powers);
    free(acc);
    free(part_modes);
    return false;
  }

  uint64_t *rowptr = NULL;
  uint32_t *colind = NULL;
  uint32_t *component = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*component));
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (component == NULL || result == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(powers);
    free(inv_powers);
    free(acc);
    free(part_modes);
    free(component);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating components Fourier solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  if (!alloc_graph(qsop, &rowptr, &colind, error)) {
    free(powers);
    free(inv_powers);
    free(acc);
    free(part_modes);
    free(component);
    qsop_result_free(result);
    return false;
  }

  uint32_t ncomponents = 0;
  const uint64_t label_start = qsop_trace_begin(trace);
  if (!label_components(qsop, rowptr, colind, component, &ncomponents, error)) {
    free(rowptr);
    free(colind);
    free(powers);
    free(inv_powers);
    free(acc);
    free(part_modes);
    free(component);
    qsop_result_free(result);
    return false;
  }
  qsop_trace_emit_elapsed(trace, "components.label_components", 0, ncomponents, label_start);

  for (uint32_t mode = 0; mode < qsop->r; mode++) {
    acc[mode] = 1;
  }
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
      free(powers);
      free(inv_powers);
      free(acc);
      free(part_modes);
      free(component);
      qsop_result_free(result);
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
      if (!solved) {
        free_subinstance(&sub);
        qsop_result_free(part);
        free_component_cache(&cache);
        free(rowptr);
        free(colind);
        free(powers);
        free(inv_powers);
        free(acc);
        free(part_modes);
        free(component);
        qsop_result_free(result);
        return false;
      }
      qsop_trace_emit_elapsed(trace, "components.solve_component", 0, sub.nvars, solve_start);
      const uint64_t store_start = qsop_trace_begin(trace);
      if (!store_cached_component(&cache, &sub, part->counts, error)) {
        free_subinstance(&sub);
        qsop_result_free(part);
        free_component_cache(&cache);
        free(rowptr);
        free(colind);
        free(powers);
        free(inv_powers);
        free(acc);
        free(part_modes);
        free(component);
        qsop_result_free(result);
        return false;
      }
      qsop_trace_emit_elapsed(trace, "components.cache_store", 0, cache.len, store_start);
      part_counts = part->counts;
      if (stats != NULL) {
        stats->cache_misses++;
        add_saturating_u64(&stats->leaf_assignments, part_stats.leaf_assignments);
      }
    }

    const uint64_t transform_start = qsop_trace_begin(trace);
    if (!component_counts_to_fourier(qsop->r, part_counts, powers, prime, part_modes, error) ||
        !component_fourier_multiply(qsop->r, acc, part_modes, prime, error)) {
      free_subinstance(&sub);
      qsop_result_free(part);
      free_component_cache(&cache);
      free(rowptr);
      free(colind);
      free(powers);
      free(inv_powers);
      free(acc);
      free(part_modes);
      free(component);
      qsop_result_free(result);
      return false;
    }
    qsop_trace_emit_elapsed(trace, "components.fourier_multiply", 0, qsop->r, transform_start);

    free_subinstance(&sub);
    qsop_result_free(part);
  }

  if (!qsop_fourier_inverse_counts(qsop->r, acc, qsop->constant, powers, inv_powers, prime,
                                   result->counts, error)) {
    free_component_cache(&cache);
    free(rowptr);
    free(colind);
    free(powers);
    free(inv_powers);
    free(acc);
    free(part_modes);
    free(component);
    qsop_result_free(result);
    return false;
  }

  if (stats != NULL) {
    stats->cache_entries = (uint64_t)cache.len;
    stats->cache_stored_residue_slots = saturating_mul_u64((uint64_t)cache.len, qsop->r);
    stats->cache_key_bytes = component_cache_key_bytes(&cache);
    stats->cache_count_bytes = component_cache_count_bytes(&cache);
    stats->cache_estimated_bytes = component_cache_estimated_bytes(&cache);
  }

  free(rowptr);
  free(colind);
  free_component_cache(&cache);
  free(powers);
  free(inv_powers);
  free(acc);
  free(part_modes);
  free(component);
  *out = result;
  return true;
}

static bool solve_components_crt(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                 qsop_result_t **out, qsop_solve_stats_t *stats,
                                 qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "components CRT count table is too large");
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
    set_error(error, "out of memory while allocating components CRT solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(qsop->r, sizeof(*result->count_strings));
  if (result->count_strings == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating components CRT result strings");
    return false;
  }

  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!solve_components_once(qsop, max_component_vars, primes[p],
                               &all_counts[p * (size_t)qsop->r], stats_for_prime,
                               trace_for_prime, error)) {
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
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes,
                                      &result->count_strings[residue], error)) {
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
