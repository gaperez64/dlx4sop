#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residual.h"
#include "dlx4sop/residue.h"

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
  uint32_t r;
  uint32_t nvars;
  uint32_t nedges;
  uint32_t constant;
  uint32_t active_vars;
  uint32_t active_edges;
  uint8_t *active_var;
  uint8_t *active_edge;
  uint32_t *unary;
} residual_cache_key_t;

typedef struct residual_cache_entry {
  residual_cache_key_t key;
  uint64_t *counts;
} residual_cache_entry_t;

typedef struct residual_cache {
  residual_cache_entry_t *entries;
  size_t len;
  size_t cap;
} residual_cache_t;

typedef struct branch_search_stats {
  uint64_t nodes;
  uint64_t leaves;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t *work;
  uint64_t *tmp;
  residual_cache_t cache;
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

static void add_counts(uint32_t r, uint64_t *dst, const uint64_t *src) {
  for (uint32_t residue = 0; residue < r; residue++) {
    dst[residue] += src[residue];
  }
}

static void residual_cache_key_free(residual_cache_key_t *key) {
  if (key == NULL) {
    return;
  }

  free(key->active_var);
  free(key->active_edge);
  free(key->unary);
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
  if (key->active_var == NULL || key->active_edge == NULL || key->unary == NULL) {
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
  }

  return true;
}

static bool residual_cache_key_matches_residual(const residual_cache_key_t *key,
                                                const qsop_residual_t *residual) {
  if (key->fingerprint != qsop_residual_fingerprint(residual) ||
      key->r != qsop_residual_modulus(residual) ||
      key->nvars != qsop_residual_nvars(residual) ||
      key->nedges != qsop_residual_nedges(residual) ||
      key->constant != qsop_residual_constant(residual) ||
      key->active_vars != qsop_residual_active_vars(residual) ||
      key->active_edges != qsop_residual_active_edges(residual)) {
    return false;
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

static const residual_cache_entry_t *residual_cache_find(const residual_cache_t *cache,
                                                         const qsop_residual_t *residual) {
  const uint64_t fingerprint = qsop_residual_fingerprint(residual);
  for (size_t i = 0; i < cache->len; i++) {
    const residual_cache_entry_t *entry = &cache->entries[i];
    if (entry->key.fingerprint == fingerprint &&
        residual_cache_key_matches_residual(&entry->key, residual)) {
      return entry;
    }
  }
  return NULL;
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

  residual_cache_entry_t *new_entries =
      realloc(cache->entries, new_cap * sizeof(*cache->entries));
  if (new_entries == NULL) {
    set_error(error, "out of memory while growing residual cache");
    return false;
  }

  cache->entries = new_entries;
  cache->cap = new_cap;
  return true;
}

static bool residual_cache_store(residual_cache_t *cache, const qsop_residual_t *residual,
                                 const uint64_t *counts, qsop_error_t *error) {
  residual_cache_entry_t entry = {0};
  if (!residual_cache_key_create(residual, &entry.key, error)) {
    return false;
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

  cache->entries[cache->len++] = entry;
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
  *cache = (residual_cache_t){0};
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
      const uint32_t degree = qsop_residual_active_degree(residual, v);
      if (degree == 0) {
        continue;
      }
      uint32_t components = 0;
      if (!qsop_residual_components_without_var(residual, v, &components, error)) {
        return false;
      }
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
                           branch_search_stats_t *stats, qsop_error_t *error);

static bool branch_sum_uncached(qsop_residual_t *residual, uint64_t *counts,
                                branch_search_stats_t *stats, qsop_error_t *error) {
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

static bool branch_sum_rec(qsop_residual_t *residual, uint64_t *counts,
                           branch_search_stats_t *stats, qsop_error_t *error) {
  stats->nodes++;

  const residual_cache_entry_t *entry = residual_cache_find(&stats->cache, residual);
  if (entry != NULL) {
    stats->cache_hits++;
    add_counts(qsop_residual_modulus(residual), counts, entry->counts);
    return true;
  }

  stats->cache_misses++;
  uint64_t *computed = NULL;
  const uint32_t r = qsop_residual_modulus(residual);
  if (!qsop_counts_alloc(r, &computed, error)) {
    return false;
  }
  if (!branch_sum_uncached(residual, computed, stats, error)) {
    free(computed);
    return false;
  }
  if (!residual_cache_store(&stats->cache, residual, computed, error)) {
    free(computed);
    return false;
  }

  add_counts(r, counts, computed);
  free(computed);
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
    residual_cache_free(&search.cache);
    free(search.work);
    free(search.tmp);
    return false;
  }
  if (!branch_sum_rec(residual, result->counts, &search, error)) {
    qsop_result_free(result);
    qsop_residual_free(residual);
    residual_cache_free(&search.cache);
    free(search.work);
    free(search.tmp);
    return false;
  }
  if (stats != NULL) {
    stats->search_nodes = search.nodes;
    stats->leaf_assignments = search.leaves;
    stats->cache_hits = search.cache_hits;
    stats->cache_misses = search.cache_misses;
  }

  qsop_residual_free(residual);
  residual_cache_free(&search.cache);
  free(search.work);
  free(search.tmp);
  *out = result;
  return true;
}
