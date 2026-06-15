#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/qsop_stats.h"
#include "dlx4sop/residual.h"
#include "dlx4sop/residue.h"
#include "component_key.h"
#include "trace.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
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

typedef struct residual_cache_key {
  uint64_t fingerprint;
  bool canonical;
  uint32_t r;
  uint32_t nvars;
  uint32_t nedges;
  uint32_t constant;
  uint32_t active_vars;
  uint32_t active_edges;
  uint8_t *active_var;
  uint8_t *active_edge;
  uint32_t *unary;
  uint32_t *edge_u;
  uint32_t *edge_v;
  uint32_t *edge_q;
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
  uint64_t rankwidth_labelled_exact_cuts;
  uint64_t rankwidth_labelled_proxy_cuts;
  uint64_t rankwidth_labelled_exact_assignments;
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
  qsop_solve_trace_t *trace;
  qsop_branch_heuristic_t heuristic;
  uint64_t count_modulus;
  uint32_t depth;
  uint32_t decomposition_width;
  uint32_t rankwidth_support_width;
  uint32_t rankwidth_labelled_width;
} branch_search_stats_t;

#define BRANCH_TREEWIDTH_DELEGATE_MIN_VARS 32U
#define BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH 14U
#define BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS (BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH + 1U)
#define BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH 5U
#define BRANCH_RANKWIDTH_TREEWIDTH_MARGIN 2U
#define BRANCH_SMALL_COMPONENT_CANONICAL_NVARS 5U

static bool build_active_residual_subinstance(const qsop_residual_t *residual, qsop_instance_t *sub,
                                             qsop_error_t *error);

static void free_subinstance(qsop_instance_t *sub) {
  if (sub == NULL) {
    return;
  }
  free(sub->unary);
  free(sub->edge_u);
  free(sub->edge_v);
  free(sub->edge_q);
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

static bool branch_counts_shift_add(uint32_t r, uint64_t *dst, const uint64_t *src,
                                    uint32_t shift, const branch_search_stats_t *stats,
                                    qsop_error_t *error) {
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
                                   const uint64_t *right,
                                   const branch_search_stats_t *stats, qsop_error_t *error) {
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

static void residual_cache_key_free(residual_cache_key_t *key) {
  if (key == NULL) {
    return;
  }

  free(key->active_var);
  free(key->active_edge);
  free(key->unary);
  free(key->edge_u);
  free(key->edge_v);
  free(key->edge_q);
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
  key->edge_q = malloc((nedges == 0 ? 1U : nedges) * sizeof(*key->edge_q));
  if (key->active_var == NULL || key->active_edge == NULL || key->unary == NULL ||
      key->edge_u == NULL || key->edge_v == NULL || key->edge_q == NULL) {
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
    key->edge_q[e] = qsop_residual_edge_q(residual, e);
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
    fingerprint ^= key->edge_q[e];
    fingerprint *= UINT64_C(1099511628211);
  }
  return fingerprint;
}

static bool residual_cache_key_create_from_instance(const qsop_instance_t *sub, uint32_t constant,
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
  key->edge_q = malloc((sub->nedges == 0 ? 1U : sub->nedges) * sizeof(*key->edge_q));
  if (key->active_var == NULL || key->active_edge == NULL || key->unary == NULL ||
      key->edge_u == NULL || key->edge_v == NULL || key->edge_q == NULL) {
    residual_cache_key_free(key);
    set_error(error, "out of memory while allocating canonical residual cache key");
    return false;
  }

  memset(key->active_var, 1, (size_t)sub->nvars * sizeof(*key->active_var));
  memset(key->active_edge, 1, (size_t)sub->nedges * sizeof(*key->active_edge));
  memcpy(key->unary, sub->unary, (size_t)sub->nvars * sizeof(*key->unary));
  memcpy(key->edge_u, sub->edge_u, (size_t)sub->nedges * sizeof(*key->edge_u));
  memcpy(key->edge_v, sub->edge_v, (size_t)sub->nedges * sizeof(*key->edge_v));
  memcpy(key->edge_q, sub->edge_q, (size_t)sub->nedges * sizeof(*key->edge_q));
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
  const bool ok = residual_cache_key_create_from_instance(
      &sub, qsop_residual_constant(residual), key, error);
  free_subinstance(&sub);
  return ok;
}

static bool residual_cache_key_create_for_store(const qsop_residual_t *residual,
                                                residual_cache_key_t *key,
                                                qsop_error_t *error) {
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
        lhs->edge_v[e] != rhs->edge_v[e] || lhs->edge_q[e] != rhs->edge_q[e]) {
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
        key->edge_v[e] != qsop_residual_edge_v(residual, e) ||
        key->edge_q[e] != qsop_residual_edge_q(residual, e)) {
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
                                const residual_cache_entry_t **out,
                                bool *out_canonical_lookup, qsop_error_t *error) {
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

  if (!qsop_counts_alloc(entry.key.r, &entry.counts, error)) {
    residual_cache_key_free(&entry.key);
    return false;
  }
  memcpy(entry.counts, counts, (size_t)entry.key.r * sizeof(*entry.counts));

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
    add_saturating_u64(&bytes,
                       saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->active_var)));
    add_saturating_u64(&bytes,
                       saturating_mul_u64((uint64_t)key->nedges, sizeof(*key->active_edge)));
    add_saturating_u64(&bytes,
                       saturating_mul_u64((uint64_t)key->nvars, sizeof(*key->unary)));
    add_saturating_u64(
        &bytes, saturating_mul_u64(saturating_mul_u64((uint64_t)key->nedges, 3U),
                                   sizeof(*key->edge_u)));
  }
  return bytes;
}

static uint64_t residual_cache_count_bytes(const residual_cache_t *cache) {
  uint64_t bytes = 0;
  for (size_t i = 0; i < cache->len; i++) {
    add_saturating_u64(
        &bytes, saturating_mul_u64((uint64_t)cache->entries[i].key.r,
                                   sizeof(*cache->entries[i].counts)));
  }
  return bytes;
}

static uint64_t residual_cache_estimated_bytes(const residual_cache_t *cache) {
  uint64_t bytes = saturating_mul_u64((uint64_t)cache->cap, sizeof(*cache->entries));
  add_saturating_u64(
      &bytes, saturating_mul_u64((uint64_t)cache->bucket_count, sizeof(*cache->buckets)));
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

  uint32_t nedges = 0;
  bool sign_only = true;
  const uint32_t r = qsop_residual_modulus(residual);
  const uint32_t sign_coeff = r / 2U;
  for (uint32_t e = 0; e < source_edges; e++) {
    const uint32_t u = qsop_residual_edge_u(residual, e);
    if (qsop_residual_edge_active(residual, e) && component[u] == wanted) {
      nedges++;
      if (qsop_residual_edge_q(residual, e) != sign_coeff) {
        sign_only = false;
      }
    }
  }

  *sub = (qsop_instance_t){
      .r = r,
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
    sub->edge_q[out_edge] = qsop_residual_edge_q(residual, e);
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

static void merge_delegated_stats(branch_search_stats_t *stats,
                                  const qsop_solve_stats_t *delegated,
                                  uint32_t delegated_nvars) {
  add_saturating_u64(&stats->leaves, assignment_count(delegated_nvars));
  add_saturating_u64(&stats->treewidth_delegations, delegated->treewidth_delegations);
  add_saturating_u64(&stats->rankwidth_delegations, delegated->rankwidth_delegations);
  add_saturating_u64(&stats->table_entries, delegated->table_entries);
  add_saturating_u64(&stats->signature_entries, delegated->signature_entries);
  add_saturating_u64(&stats->join_pairs, delegated->join_pairs);
  add_saturating_u64(&stats->join_signature_pairs, delegated->join_signature_pairs);
  add_saturating_u64(&stats->rankwidth_labelled_exact_cuts,
                     delegated->rankwidth_labelled_exact_cuts);
  add_saturating_u64(&stats->rankwidth_labelled_proxy_cuts,
                     delegated->rankwidth_labelled_proxy_cuts);
  add_saturating_u64(&stats->rankwidth_labelled_exact_assignments,
                     delegated->rankwidth_labelled_exact_assignments);
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
  if (delegated->rankwidth_support_width > stats->rankwidth_support_width) {
    stats->rankwidth_support_width = delegated->rankwidth_support_width;
  }
  if (delegated->rankwidth_labelled_width > stats->rankwidth_labelled_width) {
    stats->rankwidth_labelled_width = delegated->rankwidth_labelled_width;
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

static bool rankwidth_should_override_treewidth(uint32_t treewidth_width,
                                                uint32_t decision_width, uint32_t r) {
  return decision_width <= BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH &&
         binary_assignment_forecast(decision_width) <
             treewidth_table_forecast(treewidth_width, r) &&
         treewidth_width > decision_width + BRANCH_RANKWIDTH_TREEWIDTH_MARGIN;
}

static bool branch_try_rankwidth_delegate(qsop_instance_t *sub, uint64_t *counts,
                                          uint32_t treewidth_width,
                                          uint32_t prefix_cut_rank,
                                          bool treewidth_available,
                                          uint32_t constant_shift,
                                          branch_search_stats_t *stats,
                                          bool *out_delegated, qsop_error_t *error) {
  *out_delegated = false;
  uint64_t treewidth_table = 0;
  uint64_t treewidth_join_pairs = 0;
  if (treewidth_available) {
    treewidth_table = treewidth_table_forecast(treewidth_width, sub->r);
    treewidth_join_pairs = treewidth_join_pair_forecast(treewidth_width, sub->nvars);
    branch_trace_event(stats, "branch.treewidth_table_forecast", treewidth_table);
    branch_trace_event(stats, "branch.treewidth_join_pair_forecast", treewidth_join_pairs);
  }
  if (treewidth_available &&
      treewidth_width <=
          BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH + BRANCH_RANKWIDTH_TREEWIDTH_MARGIN) {
    note_rankwidth_skip(stats, "branch.rankwidth_skip_treewidth_preferred", treewidth_width);
    return true;
  }
  if (treewidth_available && prefix_cut_rank > BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH &&
      prefix_cut_rank + BRANCH_RANKWIDTH_TREEWIDTH_MARGIN >= treewidth_width) {
    note_rankwidth_skip(stats, "branch.rankwidth_skip_prefix_proxy", prefix_cut_rank);
    return true;
  }

  qsop_rankwidth_decomposition_t *decomposition = NULL;
  const uint64_t generate_start = qsop_trace_begin(stats->trace);
  if (!qsop_rankwidth_decomposition_generate(sub, QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
                                             &decomposition, error)) {
    return false;
  }

  uint32_t support_width = 0;
  uint32_t labelled_width = 0;
  if (!qsop_rankwidth_decomposition_widths(sub, decomposition, &support_width, &labelled_width,
                                           error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  qsop_trace_emit_elapsed(stats->trace, "branch.rankwidth_probe", stats->depth, labelled_width,
                          generate_start);
  branch_trace_event(stats, "branch.rankwidth_support_probe", support_width);
  max_u32(&stats->rankwidth_support_width, support_width);
  max_u32(&stats->rankwidth_labelled_width, labelled_width);
  uint64_t rankwidth_table_forecast =
      saturating_mul_u64(binary_assignment_forecast(labelled_width), sub->r);
  uint64_t rankwidth_join_pair_forecast = 0;
  if (!qsop_rankwidth_decomposition_forecast(sub, decomposition, &rankwidth_table_forecast,
                                             &rankwidth_join_pair_forecast, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  branch_trace_event(stats, "branch.rankwidth_table_forecast", rankwidth_table_forecast);
  branch_trace_event(stats, "branch.rankwidth_join_pair_forecast", rankwidth_join_pair_forecast);
  if (rankwidth_table_forecast > stats->rankwidth_table_forecast) {
    stats->rankwidth_table_forecast = rankwidth_table_forecast;
  }
  if (rankwidth_join_pair_forecast > stats->rankwidth_join_pair_forecast) {
    stats->rankwidth_join_pair_forecast = rankwidth_join_pair_forecast;
  }

  const bool rankwidth_table_forecast_wins =
      !treewidth_available || rankwidth_table_forecast < treewidth_table;
  const bool rankwidth_join_forecast_wins =
      !treewidth_available || rankwidth_join_pair_forecast <= treewidth_join_pairs;
  const bool use_rankwidth =
      rankwidth_table_forecast_wins && rankwidth_join_forecast_wins &&
      (!treewidth_available || treewidth_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH ||
       rankwidth_should_override_treewidth(treewidth_width, labelled_width, sub->r));
  if (!use_rankwidth || labelled_width > BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
    const char *skip_phase = "branch.rankwidth_skip_policy";
    if (labelled_width > BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH) {
      skip_phase = "branch.rankwidth_skip_width";
    } else if (!rankwidth_table_forecast_wins) {
      skip_phase = "branch.rankwidth_skip_table_forecast";
    } else if (!rankwidth_join_forecast_wins) {
      skip_phase = "branch.rankwidth_skip_join_pair_forecast";
    }
    note_rankwidth_skip(stats, skip_phase,
                        rankwidth_join_forecast_wins ? labelled_width
                                                     : rankwidth_join_pair_forecast);
    qsop_rankwidth_decomposition_free(decomposition);
    return true;
  }

  uint64_t *part_counts = NULL;
  qsop_solve_stats_t delegated = {0};
  const uint64_t solve_start = qsop_trace_begin(stats->trace);
  const bool ok =
      qsop_counts_alloc(sub->r, &part_counts, error) &&
      qsop_solve_rankwidth_count_table_mod_stats(sub, decomposition, stats->count_modulus,
                                                 part_counts, &delegated, stats->trace, error);
  qsop_trace_emit_elapsed(stats->trace, "branch.rankwidth_delegate", stats->depth, sub->nvars,
                          solve_start);
  qsop_rankwidth_decomposition_free(decomposition);
  if (!ok) {
    free(part_counts);
    return false;
  }

  if (!branch_counts_shift_add(sub->r, counts, part_counts, constant_shift, stats, error)) {
    free(part_counts);
    return false;
  }
  delegated.rankwidth_delegations = 1;
  merge_delegated_stats(stats, &delegated, sub->nvars);
  free(part_counts);
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

  qsop_instance_t sub = {0};
  qsop_stats_t sub_stats = {0};
  if (!build_active_residual_subinstance(residual, &sub, error)) {
    return false;
  }
  const uint64_t stats_start = qsop_trace_begin(stats->trace);
  if (!qsop_compute_stats(&sub, &sub_stats, error)) {
    free_subinstance(&sub);
    return false;
  }
  note_width_probe(stats, &sub_stats);
  qsop_trace_emit_elapsed(stats->trace, "branch.width_probe", stats->depth,
                          sub_stats.min_fill_width, stats_start);

  bool delegated = false;
  if (!branch_try_rankwidth_delegate(&sub, counts, sub_stats.min_fill_width,
                                     sub_stats.prefix_cut_rank,
                                     sub_stats.width_diagnostics_available,
                                     qsop_residual_constant(residual), stats, &delegated, error)) {
    free_subinstance(&sub);
    return false;
  }
  if (delegated) {
    free_subinstance(&sub);
    *out_delegated = true;
    return true;
  }

  if (!sub_stats.width_diagnostics_available ||
      sub_stats.min_fill_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    note_treewidth_skip(
        stats,
        sub_stats.width_diagnostics_available ? "branch.treewidth_skip_width"
                                              : "branch.treewidth_skip_unavailable",
        sub_stats.width_diagnostics_available ? sub_stats.min_fill_width : 0);
    free_subinstance(&sub);
    return true;
  }

  uint32_t *order = NULL;
  uint32_t order_width = 0;
  const uint64_t order_start = qsop_trace_begin(stats->trace);
  if (!qsop_treewidth_order_alloc(&sub, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &order,
                                  &order_width, error)) {
    free_subinstance(&sub);
    return false;
  }
  qsop_trace_emit_elapsed(stats->trace, "branch.treewidth_order_probe", stats->depth,
                          order_width, order_start);
  if (order_width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    note_treewidth_skip(stats, "branch.treewidth_skip_order_width", order_width);
    free(order);
    free_subinstance(&sub);
    return true;
  }

  uint64_t *part_counts = NULL;
  qsop_solve_stats_t delegated_stats = {0};
  const uint64_t solve_start = qsop_trace_begin(stats->trace);
  const bool ok =
      qsop_counts_alloc(sub.r, &part_counts, error) &&
      qsop_solve_treewidth_precomputed_order_count_mod_stats(
          &sub, BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS, order, order_width,
          stats->count_modulus, part_counts, &delegated_stats, stats->trace, error);
  qsop_trace_emit_elapsed(stats->trace, "branch.treewidth_delegate", stats->depth, sub.nvars,
                          solve_start);
  free(order);
  if (!ok) {
    free_subinstance(&sub);
    free(part_counts);
    return false;
  }

  if (!branch_counts_shift_add(sub.r, counts, part_counts, qsop_residual_constant(residual),
                               stats, error)) {
    free_subinstance(&sub);
    free(part_counts);
    return false;
  }
  delegated_stats.treewidth_delegations = 1;
  merge_delegated_stats(stats, &delegated_stats, sub.nvars);
  free_subinstance(&sub);
  free(part_counts);
  *out_delegated = true;
  return true;
}

static bool branch_sum_rec(qsop_residual_t *residual, uint64_t *counts,
                           branch_search_stats_t *stats, qsop_error_t *error);

static void branch_search_free(branch_search_stats_t *search) {
  if (search == NULL) {
    return;
  }
  residual_cache_free(&search->cache);
  free(search->work);
  free(search->tmp);
  search->work = NULL;
  search->tmp = NULL;
}

static bool branch_solve_counts_once(const qsop_instance_t *qsop, uint64_t count_modulus,
                                     qsop_branch_heuristic_t heuristic, uint64_t *counts,
                                     qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                     qsop_error_t *error) {
  qsop_residual_t *residual = NULL;
  branch_search_stats_t search = {
      .trace = trace,
      .heuristic = heuristic,
      .count_modulus = count_modulus,
  };

  if (!qsop_residual_create(qsop, &residual, error) ||
      !qsop_counts_alloc(qsop->r, &search.work, error) ||
      !qsop_counts_alloc(qsop->r, &search.tmp, error)) {
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
    stats->cache_stored_residue_slots =
        saturating_mul_u64((uint64_t)search.cache.len, qsop->r);
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
    stats->rankwidth_labelled_exact_cuts = search.rankwidth_labelled_exact_cuts;
    stats->rankwidth_labelled_proxy_cuts = search.rankwidth_labelled_proxy_cuts;
    stats->rankwidth_labelled_exact_assignments = search.rankwidth_labelled_exact_assignments;
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
    stats->rankwidth_support_width = search.rankwidth_support_width;
    stats->rankwidth_labelled_width = search.rankwidth_labelled_width;
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
  const uint32_t r = qsop_residual_modulus(residual);
  uint32_t *component = malloc((nvars == 0 ? 1U : nvars) * sizeof(*component));
  uint64_t *acc = NULL;
  uint64_t *tmp = NULL;
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

  acc[0] = 1;
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
      return false;
    }

    const uint64_t convolve_start = qsop_trace_begin(stats->trace);
    if (!branch_counts_convolve(r, tmp, acc, part_counts, stats, error)) {
      free_subinstance(&sub);
      free(part_counts);
      free(component);
      free(acc);
      free(tmp);
      return false;
    }
    qsop_trace_emit_elapsed(stats->trace, "branch.convolution", stats->depth, r, convolve_start);

    memcpy(acc, tmp, (size_t)r * sizeof(*acc));
    free_subinstance(&sub);
    free(part_counts);
  }

  if (!branch_counts_shift_add(r, counts, acc, qsop_residual_constant(residual), stats, error)) {
    free(component);
    free(acc);
    free(tmp);
    return false;
  }
  free(component);
  free(acc);
  free(tmp);
  *out_split = true;
  return true;
}

typedef struct branch_candidate {
  uint32_t var;
  uint32_t components;
  uint32_t largest_component;
  uint32_t degree;
  uint64_t fill_edges;
  uint32_t cut_rank;
  bool has_unary;
} branch_candidate_t;

static bool split_candidate_better(const branch_candidate_t *candidate,
                                   const branch_candidate_t *best) {
  /* One-variable balance gains can add component subsolves without reducing current corpus search. */
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

static bool choose_branch_var(const qsop_residual_t *residual, qsop_branch_heuristic_t heuristic,
                              uint32_t *out, qsop_error_t *error) {
  const uint32_t nvars = qsop_residual_nvars(residual);
  bool found = false;
  branch_candidate_t best = {0};

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
      if (heuristic == QSOP_BRANCH_HEURISTIC_TREEWIDTH &&
          !qsop_residual_fill_edges_without_var(residual, v, &candidate.fill_edges, error)) {
        return false;
      }
      if (heuristic == QSOP_BRANCH_HEURISTIC_CUTRANK_PROXY &&
          !qsop_residual_neighbor_cut_rank(residual, v, &candidate.cut_rank, error)) {
        return false;
      }
      if (!found || branch_candidate_better(heuristic, &candidate, &best)) {
        found = true;
        best = candidate;
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
    return branch_count_add(stats, &counts[qsop_residual_constant(residual)], 1, error);
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
  qsop_trace_emit_elapsed(stats->trace,
                          canonical_lookup ? "branch.cache_canonical_lookup"
                                           : "branch.cache_lookup",
                          stats->depth, stats->cache.len, lookup_start);
  if (entry != NULL) {
    stats->cache_hits++;
    if (entry->key.canonical) {
      stats->cache_canonical_hits++;
    }
    if (entry->search_nodes > 1U) {
      add_saturating_u64(&stats->cache_avoided_nodes, entry->search_nodes - 1U);
    }
    return add_counts(qsop_residual_modulus(residual), counts, entry->counts, stats, error);
  }

  stats->cache_misses++;
  const uint64_t subtree_start_nodes = stats->nodes;
  uint64_t *computed = NULL;
  const uint32_t r = qsop_residual_modulus(residual);
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
                          canonical_store ? "branch.cache_canonical_store"
                                          : "branch.cache_store",
                          stats->depth, stats->cache.len, store_start);

  if (!add_counts(r, counts, computed, stats, error)) {
    free(computed);
    return false;
  }
  free(computed);
  return true;
}

static bool solve_branch_crt(const qsop_instance_t *qsop, qsop_branch_heuristic_t heuristic,
                             qsop_result_t **out, qsop_solve_stats_t *stats,
                             qsop_solve_trace_t *trace, qsop_error_t *error) {
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
  result->r = qsop->r;
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
    if (!branch_solve_counts_once(qsop, primes[p], heuristic,
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
                                                qsop_solve_trace_t *trace,
                                                bool *out_handled, qsop_error_t *error) {
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
  const uint64_t stats_start = qsop_trace_begin(trace);
  if (!qsop_treewidth_order_alloc(qsop, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &order, &width,
                                  error)) {
    return false;
  }
  qsop_trace_emit_elapsed(trace, "branch.root_width_probe", 0, width, stats_start);
  if (width > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH) {
    free(order);
    return true;
  }
  qsop_trace_emit(trace, "branch.treewidth_table_forecast", 0,
                  treewidth_table_forecast(width, qsop->r), 0);
  qsop_trace_emit(trace, "branch.treewidth_join_pair_forecast", 0,
                  treewidth_join_pair_forecast(width, qsop->nvars), 0);
  qsop_trace_emit(trace, "branch.rankwidth_skip_treewidth_preferred", 0, width, 0);

  qsop_result_t *result = NULL;
  qsop_solve_stats_t delegated = {0};
  const uint64_t solve_start = qsop_trace_begin(trace);
  if (!qsop_solve_treewidth_precomputed_order_trace_stats(
          qsop, BRANCH_TREEWIDTH_DELEGATE_MAX_BAG_VARS, order, width, &result, &delegated, trace,
          error)) {
    free(order);
    return false;
  }
  free(order);
  qsop_trace_emit_elapsed(trace, "branch.root_treewidth_delegate", 0, qsop->nvars, solve_start);

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

  *out = result;
  *out_handled = true;
  return true;
}

bool qsop_solve_residual_branch(const qsop_instance_t *qsop, uint32_t max_vars, qsop_result_t **out,
                                qsop_error_t *error) {
  return qsop_solve_residual_branch_stats(qsop, max_vars, out, NULL, error);
}

bool qsop_solve_residual_branch_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                      qsop_result_t **out, qsop_solve_stats_t *stats,
                                      qsop_error_t *error) {
  return qsop_solve_residual_branch_trace_stats(qsop, max_vars, out, stats, NULL, error);
}

bool qsop_solve_residual_branch_trace_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                            qsop_result_t **out, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_residual_branch_heuristic_trace_stats(
      qsop, max_vars, QSOP_BRANCH_HEURISTIC_SPLIT, out, stats, trace, error);
}

bool qsop_solve_residual_branch_heuristic_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_vars, qsop_branch_heuristic_t heuristic,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
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
  bool root_handled = false;
  if (!branch_try_root_treewidth_fast_path(qsop, out, stats, trace, &root_handled, error)) {
    return false;
  }
  if (root_handled) {
    return true;
  }
  if (qsop->nvars >= 64U) {
    return solve_branch_crt(qsop, heuristic, out, stats, trace, error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    set_error(error, "out of memory while allocating result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    return false;
  }

  if (!branch_solve_counts_once(qsop, 0, heuristic, result->counts, stats, trace, error)) {
    qsop_result_free(result);
    return false;
  }

  *out = result;
  return true;
}
