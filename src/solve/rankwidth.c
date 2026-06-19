#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/residue.h"
#include "trace.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RW_JOIN_MAP_INITIAL_CAP 1024U
#define RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT UINT64_C(1000000)

typedef enum rw_node_kind {
  RW_NODE_UNDEFINED,
  RW_NODE_LEAF,
  RW_NODE_JOIN,
} rw_node_kind_t;

typedef struct rw_node {
  rw_node_kind_t kind;
  uint32_t var;
  uint32_t left;
  uint32_t right;
} rw_node_t;

struct qsop_rankwidth_decomposition {
  uint32_t nvars;
  uint32_t nnodes;
  uint32_t root;
  size_t words;
  rw_node_t *nodes;
  uint64_t *node_vars;
  uint32_t *postorder;
  uint32_t postorder_len;
  /* Cached score populated by qsop_rankwidth_decomposition_widths.
   * When score_cached is true, rankwidth_record_decomposition_diagnostics
   * skips recomputing decomposition_score and uses these values directly. */
  bool score_cached;
  uint32_t cached_support_width;
  uint32_t cached_labelled_width;
  uint64_t cached_table_forecast;
  uint64_t cached_join_pair_forecast;
  uint64_t cached_exact_cuts;
  uint64_t cached_proxy_cuts;
  uint64_t cached_exact_assignments;
};

typedef struct rw_entry {
  uint32_t signature;
  uint32_t residue;
  uint64_t count;
} rw_entry_t;

typedef struct rw_signature_rep {
  uint32_t signature;
} rw_signature_rep_t;

typedef struct rw_table {
  rw_entry_t *entries;
  size_t len;
  size_t cap;
  rw_signature_rep_t *reps;
  uint64_t *assignments;
  size_t reps_len;
  size_t reps_cap;
} rw_table_t;

typedef struct rw_join_map_entry {
  uint32_t left_signature;
  uint32_t right_signature;
  uint32_t parent_signature;
  uint32_t residue_shift;
} rw_join_map_entry_t;

typedef struct rw_join_map {
  rw_join_map_entry_t *entries;
  uint64_t *assignments;
  uint64_t *sorted_keys; /* packed (left_sig << 32 | right_sig) in sorted order */
  uint32_t *sorted_idx;  /* original entry index in sorted key order */
  size_t len;
  size_t cap;
} rw_join_map_t;

/* Pure transition result: filled by rw_compute_join_transition_sign(). */
typedef struct rw_transition_eval {
  bool valid;
  uint32_t left_signature;
  uint32_t right_signature;
  uint32_t parent_signature;
  uint32_t residue_shift;
} rw_transition_eval_t;

/* Compact 16-bit transition item for CSR (used when all sig ids and r fit in uint16_t). */
typedef struct rw_transition16 {
  uint16_t right_signature;
  uint16_t parent_signature;
  uint16_t residue_shift;
  uint16_t flags; /* reserved, keeps 8-byte alignment */
} rw_transition16_t;

/* Generic 32-bit transition item for CSR. */
typedef struct rw_transition32 {
  uint32_t right_signature;
  uint32_t parent_signature;
  uint32_t residue_shift;
  uint32_t flags;
} rw_transition32_t;

typedef enum rw_transition_layout_kind {
  RW_TRANSITION_LAYOUT_U16,
  RW_TRANSITION_LAYOUT_U32,
} rw_transition_layout_kind_t;

/* CSR (Compressed Sparse Row) materialized join map.
 * Rows are left signatures; columns are right-signature transitions.
 * left_signature is implicit from offset index — not stored per item. */
typedef struct rw_transition_csr {
  rw_transition_layout_kind_t kind;
  uint32_t left_sig_count;      /* number of unique left signatures */
  uint64_t transition_count;    /* total number of valid transitions */
  uint32_t *left_signatures;    /* [left_sig_count]: left sig id for each CSR row */
  uint32_t *offsets;            /* [left_sig_count + 1]: offset into items */
  union {
    rw_transition16_t *t16;
    rw_transition32_t *t32;
    void *raw;
  } items;
} rw_transition_csr_t;

typedef struct rw_fourier_table {
  uint32_t *signatures;
  uint64_t *assignments;
  uint64_t *values;
  size_t len;
  size_t cap;
} rw_fourier_table_t;

/* Open-addressing hash table for signature pool fast lookup.
 * Slot value UINT32_MAX means empty; no deletions so no tombstones. */
typedef struct rw_sig_ht {
  uint32_t *slots;  /* maps hash bucket → pool index (UINT32_MAX = empty) */
  uint64_t *keys;   /* parallel fingerprint for fast comparison without dereferencing pool */
  uint32_t mask;    /* slot count − 1 (power of two) */
} rw_sig_ht_t;

typedef struct rw_signature_pool {
  uint64_t *bits;
  uint64_t *fingerprints;
  size_t len;
  size_t cap;
  size_t words;
  rw_sig_ht_t ht; /* hash table index; only populated when len > RW_SIG_HT_THRESHOLD */
} rw_signature_pool_t;

typedef struct rw_label_signature_pool {
  uint32_t *coeffs;
  uint64_t *fingerprints;
  size_t len;
  size_t cap;
  uint32_t nvars;
  rw_sig_ht_t ht; /* hash table index */
} rw_label_signature_pool_t;

#define RW_SIG_HT_THRESHOLD 32U /* use linear scan below this; hash table above */

typedef struct rw_decomposition_score {
  uint32_t labelled_width;
  uint32_t support_width;
  uint64_t table_forecast;
  uint64_t join_pair_forecast;
  uint64_t labelled_exact_cuts;
  uint64_t labelled_proxy_cuts;
  uint64_t labelled_exact_assignments;
} rw_decomposition_score_t;

typedef struct rw_labelled_cut_stats {
  uint64_t exact_cuts;
  uint64_t proxy_cuts;
  uint64_t exact_assignments;
} rw_labelled_cut_stats_t;

#define RW_LABELLED_EXACT_SIGNATURE_MAX_SIGNATURES UINT64_C(4096)
#define RW_LABELLED_EXACT_SIGNATURE_MAX_TRANSITIONS UINT64_C(65536)

static int compare_decomposition_scores(rw_decomposition_score_t left,
                                        rw_decomposition_score_t right) {
  if (left.table_forecast != right.table_forecast) {
    return left.table_forecast < right.table_forecast ? -1 : 1;
  }
  if (left.join_pair_forecast != right.join_pair_forecast) {
    return left.join_pair_forecast < right.join_pair_forecast ? -1 : 1;
  }
  if (left.labelled_width != right.labelled_width) {
    return left.labelled_width < right.labelled_width ? -1 : 1;
  }
  if (left.support_width != right.support_width) {
    return left.support_width < right.support_width ? -1 : 1;
  }
  return 0;
}

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

static void set_parse_error(qsop_error_t *error, const char *path, size_t line, const char *fmt,
                            ...) {
  if (error == NULL) {
    return;
  }

  error->path = path;
  error->line = line;
  error->column = 1;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
}

static uint64_t *node_vars(qsop_rankwidth_decomposition_t *decomposition, uint32_t node) {
  return qsop_bitset_row(decomposition->node_vars, decomposition->words, node);
}

static const uint64_t *node_vars_const(const qsop_rankwidth_decomposition_t *decomposition,
                                       uint32_t node) {
  return qsop_bitset_const_row(decomposition->node_vars, decomposition->words, node);
}

static uint64_t *table_assignment(const rw_table_t *table, size_t index, size_t words) {
  return qsop_bitset_row(table->assignments, words, (uint32_t)index);
}

static uint64_t *join_map_assignment(const rw_join_map_t *map, size_t index, size_t words) {
  return qsop_bitset_row(map->assignments, words, (uint32_t)index);
}

static uint64_t *fourier_assignment(const rw_fourier_table_t *table, size_t index, size_t words) {
  return qsop_bitset_row(table->assignments, words, (uint32_t)index);
}

static const uint64_t *signature_bits(const rw_signature_pool_t *pool, uint32_t signature) {
  return qsop_bitset_const_row(pool->bits, pool->words, signature);
}

static uint32_t *label_signature_coeffs(rw_label_signature_pool_t *pool, uint32_t signature) {
  return pool->coeffs + (size_t)signature * pool->nvars;
}

static const uint32_t *label_signature_coeffs_const(const rw_label_signature_pool_t *pool,
                                                    uint32_t signature) {
  return pool->coeffs + (size_t)signature * pool->nvars;
}

/* Build or rebuild an open-addressing hash table from an array of fingerprints.
 * The new table has capacity ≥ 2n+1 (power of two) so load ≤ 50% after insertion. */
static bool rw_sig_ht_build(rw_sig_ht_t *ht, const uint64_t *fingerprints, uint32_t n,
                             qsop_error_t *error) {
  size_t cap = 64U;
  while (cap < (size_t)n * 2U + 1U) cap *= 2U;
  uint32_t *slots = malloc(cap * sizeof(*slots));
  uint64_t *keys  = malloc(cap * sizeof(*keys));
  if (slots == NULL || keys == NULL) {
    free(slots);
    free(keys);
    set_error(error, "out of memory building signature hash table");
    return false;
  }
  memset(slots, 0xFF, cap * sizeof(*slots));  /* UINT32_MAX = empty */
  uint32_t mask = (uint32_t)(cap - 1U);
  for (uint32_t i = 0; i < n; i++) {
    uint64_t fp = fingerprints[i];
    uint32_t h = (uint32_t)(fp ^ (fp >> 32U)) & mask;
    while (slots[h] != UINT32_MAX) h = (h + 1U) & mask;
    slots[h] = i;
    keys[h] = fp;
  }
  free(ht->slots);
  free(ht->keys);
  ht->slots = slots;
  ht->keys  = keys;
  ht->mask  = mask;
  return true;
}

static void rw_sig_ht_free(rw_sig_ht_t *ht) {
  free(ht->slots);
  free(ht->keys);
  *ht = (rw_sig_ht_t){0};
}

static bool signature_pool_init(rw_signature_pool_t *pool, size_t words, qsop_error_t *error) {
  if (pool == NULL) {
    set_error(error, "internal error: null rankwidth signature pool");
    return false;
  }
  *pool = (rw_signature_pool_t){
      .words = words,
  };
  return true;
}

static void signature_pool_free(rw_signature_pool_t *pool) {
  if (pool == NULL) {
    return;
  }
  rw_sig_ht_free(&pool->ht);
  free(pool->bits);
  free(pool->fingerprints);
  *pool = (rw_signature_pool_t){0};
}

static bool signature_pool_reserve(rw_signature_pool_t *pool, size_t needed,
                                   qsop_error_t *error) {
  if (needed <= pool->cap) {
    return true;
  }

  size_t new_cap = pool->cap == 0 ? 8U : pool->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth signature pool is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (pool->words != 0 && new_cap > SIZE_MAX / pool->words / sizeof(uint64_t)) {
    set_error(error, "rankwidth signature pool is too large");
    return false;
  }
  uint64_t *bits = calloc(new_cap * pool->words, sizeof(*bits));
  uint64_t *fingerprints = calloc(new_cap, sizeof(*fingerprints));
  if (bits == NULL || fingerprints == NULL) {
    free(bits);
    free(fingerprints);
    set_error(error, "out of memory while growing rankwidth signature pool");
    return false;
  }
  if (pool->len != 0) {
    memcpy(bits, pool->bits, pool->len * pool->words * sizeof(*bits));
    memcpy(fingerprints, pool->fingerprints, pool->len * sizeof(*fingerprints));
  }
  free(pool->bits);
  free(pool->fingerprints);
  pool->bits = bits;
  pool->fingerprints = fingerprints;
  pool->cap = new_cap;
  return true;
}

static bool signature_pool_intern(rw_signature_pool_t *pool, const uint64_t *bits,
                                  uint32_t *out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null rankwidth signature output");
    return false;
  }

  const uint64_t fingerprint = qsop_bitset_fingerprint(bits, pool->words);
  rw_sig_ht_t *ht = &pool->ht;

  if (ht->slots != NULL) {
    /* Hash table fast path: O(1) expected lookup */
    uint32_t h = (uint32_t)(fingerprint ^ (fingerprint >> 32U)) & ht->mask;
    for (;;) {
      if (ht->slots[h] == UINT32_MAX) break;  /* empty: not found */
      if (ht->keys[h] == fingerprint &&
          qsop_bitset_equal(signature_bits(pool, ht->slots[h]), bits, pool->words)) {
        *out = ht->slots[h];
        return true;
      }
      h = (h + 1U) & ht->mask;
    }
    /* h is the insertion slot; rebuild ht if load would exceed 50% */
    if (pool->len + 1U > (size_t)(ht->mask + 1U) / 2U) {
      if (!rw_sig_ht_build(ht, pool->fingerprints, (uint32_t)pool->len, error))
        return false;
      h = (uint32_t)(fingerprint ^ (fingerprint >> 32U)) & ht->mask;
      while (ht->slots[h] != UINT32_MAX) h = (h + 1U) & ht->mask;
    }
    if (pool->len > UINT32_MAX) {
      set_error(error, "rankwidth signature pool exceeds uint32 ids");
      return false;
    }
    if (!signature_pool_reserve(pool, pool->len + 1U, error)) return false;
    uint64_t *dst = qsop_bitset_row(pool->bits, pool->words, (uint32_t)pool->len);
    qsop_bitset_copy(dst, bits, pool->words);
    pool->fingerprints[pool->len] = fingerprint;
    ht->slots[h] = (uint32_t)pool->len;
    ht->keys[h]  = fingerprint;
    *out = (uint32_t)pool->len;
    pool->len++;
    return true;
  }

  /* Linear scan below RW_SIG_HT_THRESHOLD */
  for (size_t i = 0; i < pool->len; i++) {
    if (pool->fingerprints[i] == fingerprint &&
        qsop_bitset_equal(signature_bits(pool, (uint32_t)i), bits, pool->words)) {
      *out = (uint32_t)i;
      return true;
    }
  }
  if (pool->len > UINT32_MAX) {
    set_error(error, "rankwidth signature pool exceeds uint32 ids");
    return false;
  }
  if (!signature_pool_reserve(pool, pool->len + 1U, error)) return false;
  uint64_t *dst = qsop_bitset_row(pool->bits, pool->words, (uint32_t)pool->len);
  qsop_bitset_copy(dst, bits, pool->words);
  pool->fingerprints[pool->len] = fingerprint;
  *out = (uint32_t)pool->len;
  pool->len++;
  /* Build hash table once pool crosses the linear-scan threshold */
  if (pool->len >= RW_SIG_HT_THRESHOLD &&
      !rw_sig_ht_build(ht, pool->fingerprints, (uint32_t)pool->len, error)) {
    return false;
  }
  return true;
}

static bool label_signature_pool_init(rw_label_signature_pool_t *pool, uint32_t nvars,
                                      qsop_error_t *error) {
  if (pool == NULL) {
    set_error(error, "internal error: null labelled rankwidth signature pool");
    return false;
  }
  *pool = (rw_label_signature_pool_t){
      .nvars = nvars,
  };
  return true;
}

static void label_signature_pool_free(rw_label_signature_pool_t *pool) {
  if (pool == NULL) {
    return;
  }
  rw_sig_ht_free(&pool->ht);
  free(pool->coeffs);
  free(pool->fingerprints);
  *pool = (rw_label_signature_pool_t){0};
}

static uint64_t label_signature_fingerprint(const uint32_t *coeffs, uint32_t nvars) {
  uint64_t fingerprint = UINT64_C(1469598103934665603);
  for (uint32_t i = 0; i < nvars; i++) {
    fingerprint ^= coeffs[i];
    fingerprint *= UINT64_C(1099511628211);
  }
  return fingerprint;
}

static bool label_signature_pool_reserve(rw_label_signature_pool_t *pool, size_t needed,
                                         qsop_error_t *error) {
  if (needed <= pool->cap) {
    return true;
  }

  size_t new_cap = pool->cap == 0 ? 8U : pool->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "labelled rankwidth signature pool is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (pool->nvars != 0 && new_cap > SIZE_MAX / pool->nvars / sizeof(uint32_t)) {
    set_error(error, "labelled rankwidth signature pool is too large");
    return false;
  }
  uint32_t *coeffs = calloc(new_cap * (size_t)pool->nvars, sizeof(*coeffs));
  uint64_t *fingerprints = calloc(new_cap, sizeof(*fingerprints));
  if (coeffs == NULL || fingerprints == NULL) {
    free(coeffs);
    free(fingerprints);
    set_error(error, "out of memory while growing labelled rankwidth signature pool");
    return false;
  }
  if (pool->len != 0) {
    memcpy(coeffs, pool->coeffs, pool->len * (size_t)pool->nvars * sizeof(*coeffs));
    memcpy(fingerprints, pool->fingerprints, pool->len * sizeof(*fingerprints));
  }
  free(pool->coeffs);
  free(pool->fingerprints);
  pool->coeffs = coeffs;
  pool->fingerprints = fingerprints;
  pool->cap = new_cap;
  return true;
}

static bool label_signature_pool_intern(rw_label_signature_pool_t *pool, const uint32_t *coeffs,
                                        uint32_t *out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null labelled rankwidth signature output");
    return false;
  }

  const uint64_t fingerprint = label_signature_fingerprint(coeffs, pool->nvars);
  rw_sig_ht_t *ht = &pool->ht;

  if (ht->slots != NULL) {
    /* Hash table fast path */
    uint32_t h = (uint32_t)(fingerprint ^ (fingerprint >> 32U)) & ht->mask;
    for (;;) {
      if (ht->slots[h] == UINT32_MAX) break;
      if (ht->keys[h] == fingerprint) {
        const uint32_t *candidate = label_signature_coeffs_const(pool, ht->slots[h]);
        if (memcmp(candidate, coeffs, (size_t)pool->nvars * sizeof(*coeffs)) == 0) {
          *out = ht->slots[h];
          return true;
        }
      }
      h = (h + 1U) & ht->mask;
    }
    if (pool->len + 1U > (size_t)(ht->mask + 1U) / 2U) {
      if (!rw_sig_ht_build(ht, pool->fingerprints, (uint32_t)pool->len, error))
        return false;
      h = (uint32_t)(fingerprint ^ (fingerprint >> 32U)) & ht->mask;
      while (ht->slots[h] != UINT32_MAX) h = (h + 1U) & ht->mask;
    }
    if (pool->len > UINT32_MAX) {
      set_error(error, "labelled rankwidth signature pool exceeds uint32 ids");
      return false;
    }
    if (!label_signature_pool_reserve(pool, pool->len + 1U, error)) return false;
    uint32_t *dst = label_signature_coeffs(pool, (uint32_t)pool->len);
    memcpy(dst, coeffs, (size_t)pool->nvars * sizeof(*dst));
    pool->fingerprints[pool->len] = fingerprint;
    ht->slots[h] = (uint32_t)pool->len;
    ht->keys[h]  = fingerprint;
    *out = (uint32_t)pool->len;
    pool->len++;
    return true;
  }

  /* Linear scan below RW_SIG_HT_THRESHOLD */
  for (size_t i = 0; i < pool->len; i++) {
    const uint32_t *candidate = label_signature_coeffs_const(pool, (uint32_t)i);
    if (pool->fingerprints[i] == fingerprint &&
        memcmp(candidate, coeffs, (size_t)pool->nvars * sizeof(*coeffs)) == 0) {
      *out = (uint32_t)i;
      return true;
    }
  }
  if (pool->len > UINT32_MAX) {
    set_error(error, "labelled rankwidth signature pool exceeds uint32 ids");
    return false;
  }
  if (!label_signature_pool_reserve(pool, pool->len + 1U, error)) return false;
  uint32_t *dst = label_signature_coeffs(pool, (uint32_t)pool->len);
  memcpy(dst, coeffs, (size_t)pool->nvars * sizeof(*dst));
  pool->fingerprints[pool->len] = fingerprint;
  *out = (uint32_t)pool->len;
  pool->len++;
  if (pool->len >= RW_SIG_HT_THRESHOLD &&
      !rw_sig_ht_build(ht, pool->fingerprints, (uint32_t)pool->len, error)) {
    return false;
  }
  return true;
}

static bool qsop_is_sign_edge_instance(const qsop_instance_t *qsop);
static uint64_t *adjacency_bitsets(const qsop_instance_t *qsop, size_t words,
                                   qsop_error_t *error);
static uint32_t *coefficient_matrix(const qsop_instance_t *qsop, qsop_error_t *error);
static uint32_t cut_rank_bitsets(uint32_t nvars, const uint64_t *adj, const uint64_t *left,
                                 const uint64_t *right, size_t words, qsop_error_t *error);
static uint32_t ceil_log2_u64(uint64_t value);
static uint64_t saturating_add_u64(uint64_t left, uint64_t right);
static uint64_t binary_signature_bound(uint32_t width);
static uint32_t decomposition_width(const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_error_t *error);
static bool decomposition_score(const qsop_instance_t *qsop,
                                const qsop_rankwidth_decomposition_t *decomposition,
                                const uint64_t *adj, const uint32_t *coeffs,
                                rw_labelled_cut_stats_t *cut_stats,
                                rw_decomposition_score_t *out, qsop_error_t *error);

static bool reserve_entries(rw_table_t *table, size_t needed, qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  rw_entry_t *entries = realloc(table->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    set_error(error, "out of memory while growing rankwidth table");
    return false;
  }
  table->entries = entries;
  table->cap = new_cap;
  return true;
}

static bool reserve_reps(rw_table_t *table, size_t needed, size_t words, qsop_error_t *error) {
  if (needed <= table->reps_cap) {
    return true;
  }

  size_t new_cap = table->reps_cap == 0 ? 8U : table->reps_cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth signature table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    set_error(error, "rankwidth representative assignment table is too large");
    return false;
  }
  rw_signature_rep_t *reps = calloc(new_cap, sizeof(*reps));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  if (reps == NULL || assignments == NULL) {
    free(reps);
    free(assignments);
    set_error(error, "out of memory while growing rankwidth signature table");
    return false;
  }
  if (table->reps_len != 0) {
    memcpy(reps, table->reps, table->reps_len * sizeof(*reps));
    memcpy(assignments, table->assignments, table->reps_len * words * sizeof(*assignments));
  }
  free(table->reps);
  free(table->assignments);
  table->reps = reps;
  table->assignments = assignments;
  table->reps_cap = new_cap;
  return true;
}

static bool table_add_rep(rw_table_t *table, uint32_t signature, const uint64_t *assignment,
                          size_t words, qsop_error_t *error) {
  for (size_t i = 0; i < table->reps_len; i++) {
    if (table->reps[i].signature == signature) {
      return true;
    }
  }
  if (!reserve_reps(table, table->reps_len + 1U, words, error)) {
    return false;
  }
  table->reps[table->reps_len] = (rw_signature_rep_t){
      .signature = signature,
  };
  qsop_bitset_copy(table_assignment(table, table->reps_len, words), assignment, words);
  table->reps_len++;
  return true;
}

static bool table_add_entry(rw_table_t *table, uint32_t signature, uint32_t residue,
                            uint64_t count, qsop_error_t *error) {
  if (count == 0) {
    return true;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->entries[i].signature == signature && table->entries[i].residue == residue) {
      return qsop_count_add(&table->entries[i].count, count, error);
    }
  }
  if (!reserve_entries(table, table->len + 1U, error)) {
    return false;
  }
  table->entries[table->len++] = (rw_entry_t){
      .signature = signature,
      .residue = residue,
      .count = count,
  };
  return true;
}

static bool table_add_entry_mod(rw_table_t *table, uint32_t signature, uint32_t residue,
                                uint64_t count, uint64_t modulus, qsop_error_t *error) {
  if (count == 0) {
    return true;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->entries[i].signature == signature && table->entries[i].residue == residue) {
      table->entries[i].count = qsop_mod_add_u64(table->entries[i].count, count, modulus);
      return true;
    }
  }
  if (!reserve_entries(table, table->len + 1U, error)) {
    return false;
  }
  table->entries[table->len++] = (rw_entry_t){
      .signature = signature,
      .residue = residue,
      .count = count,
  };
  return true;
}


static void table_free(rw_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->entries);
  free(table->reps);
  free(table->assignments);
  *table = (rw_table_t){0};
}

static int compare_entries_sig_residue(const void *a, const void *b) {
  const rw_entry_t *ea = (const rw_entry_t *)a;
  const rw_entry_t *eb = (const rw_entry_t *)b;
  if (ea->signature != eb->signature) {
    return (ea->signature < eb->signature) ? -1 : 1;
  }
  if (ea->residue != eb->residue) {
    return (ea->residue < eb->residue) ? -1 : 1;
  }
  return 0;
}

static void table_sort(rw_table_t *table) {
  if (table->len <= 1) {
    return;
  }
  qsort(table->entries, table->len, sizeof(*table->entries), compare_entries_sig_residue);
}

/* Build an O(1) signature-to-entry-range index over a sorted table.
 * Replaces per-entry binary search in join hot paths.
 * starts[sig] == UINT32_MAX means sig is absent from the table.
 * Caller owns *starts_out and *ends_out and must free both. */
static bool build_sig_range_index(const rw_table_t *table, uint32_t max_sig,
                                   uint32_t **starts_out, uint32_t **ends_out,
                                   qsop_error_t *error) {
  const size_t n = (size_t)max_sig + 1U;
  uint32_t *starts = malloc(n * sizeof(*starts));
  uint32_t *ends   = malloc(n * sizeof(*ends));
  if (starts == NULL || ends == NULL) {
    free(starts);
    free(ends);
    set_error(error, "out of memory allocating sig range index");
    return false;
  }
  memset(starts, 0xFF, n * sizeof(*starts));
  for (size_t i = 0; i < table->len; i++) {
    const uint32_t sig = table->entries[i].signature;
    if (sig > max_sig) {
      continue;
    }
    if (starts[sig] == UINT32_MAX) {
      starts[sig] = (uint32_t)i;
    }
    ends[sig] = (uint32_t)(i + 1U);
  }
  *starts_out = starts;
  *ends_out   = ends;
  return true;
}

/* Writes the range index into pre-allocated starts/ends buffers (no malloc).
 * starts and ends must each have at least (max_sig + 1) uint32_t entries. */
static void build_sig_range_index_into(const rw_table_t *table, uint32_t max_sig,
                                       uint32_t *starts, uint32_t *ends) {
  const size_t n = (size_t)max_sig + 1U;
  memset(starts, 0xFF, n * sizeof(*starts));
  for (size_t i = 0; i < table->len; i++) {
    const uint32_t sig = table->entries[i].signature;
    if (sig > max_sig) {
      continue;
    }
    if (starts[sig] == UINT32_MAX) {
      starts[sig] = (uint32_t)i;
    }
    ends[sig] = (uint32_t)(i + 1U);
  }
}

/* Per-solve workspace for join accumulator buffers. Eliminates per-join malloc/free
 * pairs for acc, sig_map_idx, and the four range index arrays. */
typedef struct rw_join_workspace {
  uint64_t *acc;          /* [cap_entries] — zeroed by caller before each join */
  uint32_t *sig_map_idx;  /* [cap_sigs]    — 0xFF-filled by caller before each join */
  uint32_t *left_starts;  /* [cap_sigs] */
  uint32_t *left_ends;    /* [cap_sigs] */
  uint32_t *right_starts; /* [cap_sigs] */
  uint32_t *right_ends;   /* [cap_sigs] */
  size_t cap_entries;     /* capacity of acc in uint64_t elements (= cap_sigs * r) */
  size_t cap_sigs;        /* max signatures per node */
} rw_join_workspace_t;

static bool join_workspace_alloc(size_t cap_sigs, uint32_t r, rw_join_workspace_t *ws,
                                 qsop_error_t *error) {
  const size_t cap_entries = cap_sigs * (size_t)r;
  ws->cap_sigs    = cap_sigs;
  ws->cap_entries = cap_entries;
  ws->acc         = calloc(cap_entries == 0 ? 1U : cap_entries, sizeof(*ws->acc));
  ws->sig_map_idx = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->sig_map_idx));
  ws->left_starts = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->left_starts));
  ws->left_ends   = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->left_ends));
  ws->right_starts= malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->right_starts));
  ws->right_ends  = malloc((cap_sigs == 0 ? 1U : cap_sigs) * sizeof(*ws->right_ends));
  if (ws->acc == NULL || ws->sig_map_idx == NULL || ws->left_starts == NULL ||
      ws->left_ends == NULL || ws->right_starts == NULL || ws->right_ends == NULL) {
    free(ws->acc);
    free(ws->sig_map_idx);
    free(ws->left_starts);
    free(ws->left_ends);
    free(ws->right_starts);
    free(ws->right_ends);
    *ws = (rw_join_workspace_t){0};
    set_error(error, "out of memory allocating join workspace");
    return false;
  }
  return true;
}

static void join_workspace_free(rw_join_workspace_t *ws) {
  if (ws == NULL) {
    return;
  }
  free(ws->acc);
  free(ws->sig_map_idx);
  free(ws->left_starts);
  free(ws->left_ends);
  free(ws->right_starts);
  free(ws->right_ends);
  *ws = (rw_join_workspace_t){0};
}

static bool reserve_join_map(rw_join_map_t *map, size_t needed, size_t words,
                             qsop_error_t *error) {
  if (needed <= map->cap) {
    return true;
  }

  size_t new_cap = map->cap == 0 ? 8U : map->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth join map is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    set_error(error, "rankwidth join assignment map is too large");
    return false;
  }
  rw_join_map_entry_t *entries = calloc(new_cap, sizeof(*entries));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  if (entries == NULL || assignments == NULL) {
    free(entries);
    free(assignments);
    set_error(error, "out of memory while growing rankwidth join map");
    return false;
  }
  if (map->len != 0) {
    memcpy(entries, map->entries, map->len * sizeof(*entries));
    memcpy(assignments, map->assignments, map->len * words * sizeof(*assignments));
  }
  free(map->entries);
  free(map->assignments);
  map->entries = entries;
  map->assignments = assignments;
  map->cap = new_cap;
  return true;
}

static void join_map_free(rw_join_map_t *map) {
  if (map == NULL) {
    return;
  }
  free(map->entries);
  free(map->assignments);
  free(map->sorted_keys);
  free(map->sorted_idx);
  *map = (rw_join_map_t){0};
}

static void rw_transition_csr_free(rw_transition_csr_t *csr) {
  if (csr == NULL) {
    return;
  }
  free(csr->left_signatures);
  free(csr->offsets);
  free(csr->items.raw);
  *csr = (rw_transition_csr_t){0};
}

static uint64_t rw_transition_csr_bytes(const rw_transition_csr_t *csr) {
  if (csr == NULL || csr->left_sig_count == 0) {
    return 0;
  }
  const uint64_t item_size = (csr->kind == RW_TRANSITION_LAYOUT_U16)
                                 ? sizeof(rw_transition16_t)
                                 : sizeof(rw_transition32_t);
  return (uint64_t)(csr->left_sig_count + 1U) * sizeof(uint32_t) * 2U
       + csr->transition_count * item_size;
}

typedef struct {
  uint64_t key;
  uint32_t idx;
} rw_join_sort_entry_t;

static int compare_join_sort_entries(const void *a, const void *b) {
  const rw_join_sort_entry_t *sa = (const rw_join_sort_entry_t *)a;
  const rw_join_sort_entry_t *sb = (const rw_join_sort_entry_t *)b;
  if (sa->key < sb->key) return -1;
  if (sa->key > sb->key) return 1;
  return 0;
}

static bool join_map_build_sorted_idx(rw_join_map_t *map, qsop_error_t *error) {
  if (map->len == 0) {
    return true;
  }
  rw_join_sort_entry_t *tmp = malloc(map->len * sizeof(*tmp));
  uint32_t *sorted_idx = malloc(map->len * sizeof(*sorted_idx));
  uint64_t *sorted_keys = malloc(map->len * sizeof(*sorted_keys));
  if (tmp == NULL || sorted_idx == NULL || sorted_keys == NULL) {
    free(tmp);
    free(sorted_idx);
    free(sorted_keys);
    set_error(error, "out of memory while building rankwidth join map index");
    return false;
  }
  for (size_t i = 0; i < map->len; i++) {
    tmp[i].key = ((uint64_t)map->entries[i].left_signature << 32) |
                 map->entries[i].right_signature;
    tmp[i].idx = (uint32_t)i;
  }
  qsort(tmp, map->len, sizeof(*tmp), compare_join_sort_entries);
  for (size_t i = 0; i < map->len; i++) {
    sorted_keys[i] = tmp[i].key;
    sorted_idx[i] = tmp[i].idx;
  }
  free(tmp);
  map->sorted_keys = sorted_keys;
  map->sorted_idx = sorted_idx;
  return true;
}

static bool reserve_fourier_table(rw_fourier_table_t *table, size_t needed, uint32_t r,
                                  size_t words, qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth Fourier table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / (r == 0 ? 1U : (size_t)r) / sizeof(uint64_t)) {
    set_error(error, "rankwidth Fourier table is too large");
    return false;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    set_error(error, "rankwidth Fourier assignment table is too large");
    return false;
  }

  uint32_t *signatures = calloc(new_cap, sizeof(*signatures));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  uint64_t *values = calloc(new_cap * (size_t)r, sizeof(*values));
  if (signatures == NULL || assignments == NULL || values == NULL) {
    free(signatures);
    free(assignments);
    free(values);
    set_error(error, "out of memory while growing rankwidth Fourier table");
    return false;
  }
  if (table->len != 0) {
    memcpy(signatures, table->signatures, table->len * sizeof(*signatures));
    memcpy(assignments, table->assignments, table->len * words * sizeof(*assignments));
    memcpy(values, table->values, table->len * (size_t)r * sizeof(*values));
  }
  free(table->signatures);
  free(table->assignments);
  free(table->values);
  table->signatures = signatures;
  table->assignments = assignments;
  table->values = values;
  table->cap = new_cap;
  return true;
}

static void fourier_table_free(rw_fourier_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->signatures);
  free(table->assignments);
  free(table->values);
  *table = (rw_fourier_table_t){0};
}

static bool parse_u32_token(const char *text, uint32_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    return false;
  }
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)value;
  return true;
}

static bool validate_decomposition_dfs(qsop_rankwidth_decomposition_t *decomposition,
                                       uint32_t node, uint8_t *state, uint8_t *seen_var,
                                       qsop_error_t *error) {
  if (node >= decomposition->nnodes) {
    set_error(error, "rankwidth decomposition references node outside range");
    return false;
  }
  if (state[node] == 1U) {
    set_error(error, "rankwidth decomposition contains a cycle");
    return false;
  }
  if (state[node] == 2U) {
    return true;
  }

  rw_node_t *entry = &decomposition->nodes[node];
  if (entry->kind == RW_NODE_UNDEFINED) {
    set_error(error, "rankwidth decomposition references undefined node %" PRIu32, node);
    return false;
  }
  state[node] = 1U;
  if (entry->kind == RW_NODE_LEAF) {
    if (entry->var >= decomposition->nvars) {
      set_error(error, "rankwidth decomposition leaf variable is outside range");
      return false;
    }
    if (seen_var[entry->var] != 0) {
      set_error(error, "rankwidth decomposition maps variable %" PRIu32 " more than once",
                entry->var);
      return false;
    }
    seen_var[entry->var] = 1U;
    uint64_t *vars = node_vars(decomposition, node);
    qsop_bitset_zero(vars, decomposition->words);
    qsop_bitset_set(vars, entry->var);
  } else {
    if (!validate_decomposition_dfs(decomposition, entry->left, state, seen_var, error) ||
        !validate_decomposition_dfs(decomposition, entry->right, state, seen_var, error)) {
      return false;
    }
    const uint64_t *left = node_vars_const(decomposition, entry->left);
    const uint64_t *right = node_vars_const(decomposition, entry->right);
    uint64_t *vars = node_vars(decomposition, node);
    for (size_t w = 0; w < decomposition->words; w++) {
      if ((left[w] & right[w]) != 0) {
        set_error(error, "rankwidth decomposition children are not disjoint");
        return false;
      }
      vars[w] = left[w] | right[w];
    }
    if (qsop_bitset_empty(vars, decomposition->words)) {
      set_error(error, "rankwidth decomposition children are not disjoint");
      return false;
    }
  }
  state[node] = 2U;
  decomposition->postorder[decomposition->postorder_len++] = node;
  return true;
}

static bool validate_decomposition(qsop_rankwidth_decomposition_t *decomposition,
                                   qsop_error_t *error) {
  decomposition->postorder_len = 0;
  memset(decomposition->node_vars, 0,
         (size_t)decomposition->nnodes * decomposition->words * sizeof(*decomposition->node_vars));

  uint8_t *state = calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*state));
  uint8_t *seen_var =
      calloc(decomposition->nvars == 0 ? 1U : decomposition->nvars, sizeof(*seen_var));
  if (state == NULL || seen_var == NULL) {
    free(state);
    free(seen_var);
    set_error(error, "out of memory while validating rankwidth decomposition");
    return false;
  }

  const bool ok =
      validate_decomposition_dfs(decomposition, decomposition->root, state, seen_var, error);
  uint64_t *all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  if (all == NULL) {
    free(state);
    free(seen_var);
    set_error(error, "out of memory while validating rankwidth decomposition");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(all, v);
  }
  if (ok &&
      !qsop_bitset_equal(node_vars_const(decomposition, decomposition->root), all,
                         decomposition->words)) {
    set_error(error, "rankwidth decomposition root does not cover every variable");
    free(all);
    free(state);
    free(seen_var);
    return false;
  }
  for (uint32_t v = 0; ok && v < decomposition->nvars; v++) {
    if (seen_var[v] == 0) {
      set_error(error, "rankwidth decomposition does not include variable %" PRIu32, v);
      free(all);
      free(state);
      free(seen_var);
      return false;
    }
  }

  free(all);
  free(state);
  free(seen_var);
  return ok;
}

bool qsop_rankwidth_decomposition_parse_file(FILE *file, const char *path, uint32_t nvars,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error) {
  if (file == NULL || out == NULL) {
    set_error(error, "internal error: null rankwidth decomposition parse argument");
    return false;
  }
  *out = NULL;

  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    set_error(error, "out of memory while allocating rankwidth decomposition");
    return false;
  }

  char line[512];
  size_t line_number = 0;
  bool saw_header = false;
  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') {
      cursor++;
    }
    if (*cursor == '\0' || *cursor == '\n' || *cursor == 'c' || *cursor == '#') {
      continue;
    }

    char tag[32] = {0};
    char a[64] = {0};
    char b[64] = {0};
    char c[64] = {0};
    char d[64] = {0};
    const int fields = sscanf(cursor, "%31s %63s %63s %63s %63s", tag, a, b, c, d);
    if (fields <= 0) {
      continue;
    }
    if (strcmp(tag, "p") == 0) {
      uint32_t header_nvars = 0;
      uint32_t nnodes = 0;
      uint32_t root = 0;
      if (saw_header || fields != 5 || strcmp(a, "rwdec") != 0 ||
          !parse_u32_token(b, &header_nvars) || !parse_u32_token(c, &nnodes) ||
          !parse_u32_token(d, &root)) {
        set_parse_error(error, path, line_number,
                        "expected header: p rwdec <variables> <nodes> <root>");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      if (header_nvars != nvars) {
        set_parse_error(error, path, line_number,
                        "rankwidth decomposition variable count does not match QSOP");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      if (nnodes == 0 || root >= nnodes) {
        set_parse_error(error, path, line_number, "invalid rankwidth decomposition size or root");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      decomposition->nvars = nvars;
      decomposition->nnodes = nnodes;
      decomposition->root = root;
      decomposition->words = qsop_bitset_words(nvars);
      decomposition->nodes = calloc(nnodes, sizeof(*decomposition->nodes));
      decomposition->node_vars =
          calloc((size_t)nnodes * decomposition->words, sizeof(*decomposition->node_vars));
      decomposition->postorder = calloc(nnodes, sizeof(*decomposition->postorder));
      if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
          decomposition->postorder == NULL) {
        set_error(error, "out of memory while allocating rankwidth decomposition nodes");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      saw_header = true;
      continue;
    }

    if (!saw_header) {
      set_parse_error(error, path, line_number, "rankwidth decomposition missing header");
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }

    uint32_t node = 0;
    if (!parse_u32_token(a, &node) || node >= decomposition->nnodes ||
        decomposition->nodes[node].kind != RW_NODE_UNDEFINED) {
      set_parse_error(error, path, line_number, "invalid or duplicate rankwidth node id");
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (strcmp(tag, "l") == 0) {
      uint32_t var = 0;
      if (fields != 3 || !parse_u32_token(b, &var)) {
        set_parse_error(error, path, line_number, "expected leaf: l <node> <variable>");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_LEAF,
          .var = var,
      };
    } else if (strcmp(tag, "j") == 0) {
      uint32_t left = 0;
      uint32_t right = 0;
      if (fields != 4 || !parse_u32_token(b, &left) || !parse_u32_token(c, &right)) {
        set_parse_error(error, path, line_number, "expected join: j <node> <left> <right>");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = left,
          .right = right,
      };
    } else {
      set_parse_error(error, path, line_number, "unsupported rankwidth decomposition record");
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
  }

  if (!saw_header) {
    set_error(error, "rankwidth decomposition missing header");
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }

  *out = decomposition;
  return true;
}

bool qsop_rankwidth_decomposition_write_file(FILE *file,
                                             const qsop_rankwidth_decomposition_t *decomposition,
                                             qsop_error_t *error) {
  if (file == NULL || decomposition == NULL) {
    set_error(error, "internal error: null rankwidth decomposition write argument");
    return false;
  }

  fprintf(file, "p rwdec %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", decomposition->nvars,
          decomposition->nnodes, decomposition->root);

  for (uint32_t node = 0; node < decomposition->nnodes; node++) {
    const rw_node_t *n = &decomposition->nodes[node];
    if (n->kind == RW_NODE_LEAF) {
      fprintf(file, "l %" PRIu32 " %" PRIu32 "\n", node, n->var);
    } else if (n->kind == RW_NODE_JOIN) {
      fprintf(file, "j %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", node, n->left, n->right);
    }
  }

  if (ferror(file)) {
    set_error(error, "write error while serializing rankwidth decomposition");
    return false;
  }
  return true;
}

static uint64_t bitset_missing_edges_for(uint32_t v, const uint64_t *work,
                                         const uint64_t *active, uint64_t *remaining,
                                         size_t words, uint32_t nvars) {
  const uint64_t *neighbors = qsop_bitset_const_row(work, words, v);
  for (size_t w = 0; w < words; w++) {
    remaining[w] = neighbors[w] & active[w];
  }

  uint64_t fill = 0;
  for (uint32_t u = 0; u < nvars; u++) {
    if (!qsop_bitset_get(remaining, u)) {
      continue;
    }
    qsop_bitset_clear(remaining, u);
    const uint64_t *u_neighbors = qsop_bitset_const_row(work, words, u);
    for (size_t w = 0; w < words; w++) {
      fill += qsop_popcount_u64(remaining[w] & ~u_neighbors[w]);
    }
  }
  return fill;
}

static bool make_min_fill_order(const qsop_instance_t *qsop, uint32_t *order,
                                qsop_error_t *error) {
  const size_t words = qsop_bitset_words(qsop->nvars);
  uint64_t *work = adjacency_bitsets(qsop, words, error);
  uint64_t *active = calloc(words == 0 ? 1U : words, sizeof(*active));
  uint64_t *scratch = calloc(words == 0 ? 1U : words, sizeof(*scratch));
  if (work == NULL || active == NULL || scratch == NULL) {
    free(work);
    free(active);
    free(scratch);
    set_error(error, "out of memory while building rankwidth min-fill order");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    qsop_bitset_set(active, v);
  }

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    bool found = false;
    uint32_t best = 0;
    uint64_t best_fill = UINT64_MAX;
    uint32_t best_degree = UINT32_MAX;
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if (!qsop_bitset_get(active, v)) {
        continue;
      }
      const uint32_t degree =
          qsop_bitset_popcount_intersection(qsop_bitset_const_row(work, words, v), active, words);
      const uint64_t fill =
          bitset_missing_edges_for(v, work, active, scratch, words, qsop->nvars);
      if (!found || fill < best_fill || (fill == best_fill && degree < best_degree)) {
        found = true;
        best = v;
        best_fill = fill;
        best_degree = degree;
      }
    }
    if (!found) {
      free(work);
      free(active);
      free(scratch);
      set_error(error, "internal error: rankwidth min-fill order stopped early");
      return false;
    }
    order[pos] = best;

    const uint64_t *best_row = qsop_bitset_const_row(work, words, best);
    for (size_t w = 0; w < words; w++) {
      scratch[w] = best_row[w] & active[w];
    }
    for (uint32_t u = 0; u < qsop->nvars; u++) {
      if (!qsop_bitset_get(scratch, u)) {
        continue;
      }
      uint64_t *u_row = qsop_bitset_row(work, words, u);
      for (size_t w = 0; w < words; w++) {
        u_row[w] |= scratch[w];
        u_row[w] &= active[w];
      }
      qsop_bitset_clear(u_row, u);
    }
    qsop_bitset_clear(active, best);
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      qsop_bitset_and(qsop_bitset_row(work, words, v), active, words);
    }
  }

  free(work);
  free(active);
  free(scratch);
  return true;
}

static uint32_t build_balanced_nodes(qsop_rankwidth_decomposition_t *decomposition,
                                     const uint32_t *leaf_nodes, uint32_t begin, uint32_t end,
                                     uint32_t *next_join) {
  if (end - begin == 1U) {
    return leaf_nodes[begin];
  }
  const uint32_t mid = begin + (end - begin) / 2U;
  const uint32_t left = build_balanced_nodes(decomposition, leaf_nodes, begin, mid, next_join);
  const uint32_t right = build_balanced_nodes(decomposition, leaf_nodes, mid, end, next_join);
  const uint32_t node = (*next_join)++;
  decomposition->nodes[node] = (rw_node_t){
      .kind = RW_NODE_JOIN,
      .left = left,
      .right = right,
  };
  return node;
}

static void range_bits(const uint64_t *prefix_masks, size_t words, uint32_t begin, uint32_t end,
                       uint64_t *out) {
  const uint64_t *begin_bits = qsop_bitset_const_row(prefix_masks, words, begin);
  const uint64_t *end_bits = qsop_bitset_const_row(prefix_masks, words, end);
  for (size_t w = 0; w < words; w++) {
    out[w] = end_bits[w] & ~begin_bits[w];
  }
}

static bool choose_cut_rank_split(uint32_t nvars, const uint64_t *adj,
                                  const uint64_t *prefix_masks, const uint64_t *all,
                                  size_t words, uint32_t begin, uint32_t end, uint32_t *out,
                                  qsop_error_t *error) {
  uint64_t *left = calloc(words == 0 ? 1U : words, sizeof(*left));
  uint64_t *right = calloc(words == 0 ? 1U : words, sizeof(*right));
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  if (left == NULL || right == NULL || outside == NULL) {
    free(left);
    free(right);
    free(outside);
    set_error(error, "out of memory while choosing rankwidth cut-rank split");
    return false;
  }

  bool found = false;
  uint32_t best_split = begin + 1U;
  uint32_t best_rank = UINT32_MAX;
  uint32_t best_balance = UINT32_MAX;
  uint32_t best_rank_sum = UINT32_MAX;

  for (uint32_t split = begin + 1U; split < end; split++) {
    range_bits(prefix_masks, words, begin, split, left);
    range_bits(prefix_masks, words, split, end, right);
    qsop_bitset_copy(outside, all, words);
    qsop_bitset_and_not(outside, left, words);
    const uint32_t left_rank = cut_rank_bitsets(nvars, adj, left, outside, words, error);
    if (left_rank == UINT32_MAX) {
      free(left);
      free(right);
      free(outside);
      return false;
    }
    qsop_bitset_copy(outside, all, words);
    qsop_bitset_and_not(outside, right, words);
    const uint32_t right_rank = cut_rank_bitsets(nvars, adj, right, outside, words, error);
    if (right_rank == UINT32_MAX) {
      free(left);
      free(right);
      free(outside);
      return false;
    }
    const uint32_t rank = left_rank > right_rank ? left_rank : right_rank;
    const uint32_t rank_sum = left_rank + right_rank;
    const uint32_t left_size = split - begin;
    const uint32_t right_size = end - split;
    const uint32_t balance =
        left_size > right_size ? left_size - right_size : right_size - left_size;

    if (!found || rank < best_rank ||
        (rank == best_rank && balance < best_balance) ||
        (rank == best_rank && balance == best_balance && rank_sum < best_rank_sum)) {
      found = true;
      best_split = split;
      best_rank = rank;
      best_balance = balance;
      best_rank_sum = rank_sum;
    }
  }

  *out = best_split;
  free(left);
  free(right);
  free(outside);
  return true;
}

static uint32_t build_cut_rank_nodes(qsop_rankwidth_decomposition_t *decomposition,
                                     const uint32_t *leaf_nodes,
                                     const uint64_t *prefix_masks, const uint64_t *all,
                                     const uint64_t *adj,
                                     uint32_t begin, uint32_t end, uint32_t *next_join,
                                     qsop_error_t *error) {
  if (end - begin == 1U) {
    return leaf_nodes[begin];
  }

  uint32_t split = 0;
  if (!choose_cut_rank_split(decomposition->nvars, adj, prefix_masks, all, decomposition->words,
                             begin, end, &split, error)) {
    return UINT32_MAX;
  }
  const uint32_t left =
      build_cut_rank_nodes(decomposition, leaf_nodes, prefix_masks, all, adj, begin, split,
                           next_join, error);
  if (left == UINT32_MAX) {
    return UINT32_MAX;
  }
  const uint32_t right =
      build_cut_rank_nodes(decomposition, leaf_nodes, prefix_masks, all, adj, split, end,
                           next_join, error);
  if (right == UINT32_MAX) {
    return UINT32_MAX;
  }
  const uint32_t node = (*next_join)++;
  decomposition->nodes[node] = (rw_node_t){
      .kind = RW_NODE_JOIN,
      .left = left,
      .right = right,
  };
  return node;
}

/* Fill nodes[] for a left-deep decomposition from the given ordering, then
 * call validate_decomposition to (re)compute node_vars and postorder.
 * The decomposition must already be fully allocated (nnodes = 2*nvars-1). */
static bool fill_left_deep_from_order(qsop_rankwidth_decomposition_t *decomposition,
                                      const uint32_t *order, qsop_error_t *error) {
  const uint32_t nvars = decomposition->nvars;
  for (uint32_t i = 0; i < nvars; i++) {
    decomposition->nodes[i] = (rw_node_t){.kind = RW_NODE_LEAF, .var = order[i]};
  }
  if (nvars == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t current = 0;
    uint32_t next_join = nvars;
    for (uint32_t i = 1; i < nvars; i++) {
      decomposition->nodes[next_join] = (rw_node_t){
          .kind = RW_NODE_JOIN, .left = current, .right = i};
      current = next_join++;
    }
    decomposition->root = current;
  }
  return validate_decomposition(decomposition, error);
}

/* Adjacent-swap hill climbing on a left-deep ordering.
 * Starts from initial_order, iteratively tries all adjacent transpositions,
 * keeps those that reduce (max_table_entries, join_pairs) forecast. */
static bool generate_left_deep_search(const qsop_instance_t *qsop,
                                      const uint32_t *initial_order,
                                      qsop_rankwidth_decomposition_t **out,
                                      qsop_error_t *error) {
  const uint32_t nvars = qsop->nvars;
  const size_t words = qsop_bitset_words(nvars);

  qsop_rankwidth_decomposition_t *decomp = calloc(1, sizeof(*decomp));
  uint32_t *order = calloc(nvars == 0 ? 1U : nvars, sizeof(*order));
  if (decomp == NULL || order == NULL) {
    free(decomp);
    free(order);
    set_error(error, "out of memory in left-deep search");
    return false;
  }
  decomp->nvars = nvars;
  decomp->words = words;
  decomp->nnodes = nvars == 0U ? 0U : 2U * nvars - 1U;
  decomp->nodes = calloc(decomp->nnodes == 0U ? 1U : decomp->nnodes, sizeof(*decomp->nodes));
  decomp->node_vars = calloc(
      (decomp->nnodes == 0U ? 1U : decomp->nnodes) * (words == 0U ? 1U : words),
      sizeof(*decomp->node_vars));
  decomp->postorder = calloc(decomp->nnodes == 0U ? 1U : decomp->nnodes,
                             sizeof(*decomp->postorder));
  if (decomp->nodes == NULL || decomp->node_vars == NULL || decomp->postorder == NULL) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    set_error(error, "out of memory in left-deep search nodes");
    return false;
  }

  if (nvars == 0U) {
    free(order);
    *out = decomp;
    return true;
  }

  for (uint32_t i = 0; i < nvars; i++) {
    order[i] = initial_order[i];
  }

  if (!fill_left_deep_from_order(decomp, order, error)) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    return false;
  }

  uint64_t best_max = 0, best_pairs = 0;
  if (!qsop_rankwidth_decomposition_forecast(qsop, decomp, &best_max, &best_pairs, error)) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    return false;
  }

  bool improved = true;
  while (improved) {
    improved = false;
    for (uint32_t i = 0; i + 1U < nvars; i++) {
      uint32_t tmp = order[i];
      order[i] = order[i + 1U];
      order[i + 1U] = tmp;
      if (!fill_left_deep_from_order(decomp, order, error)) {
        qsop_rankwidth_decomposition_free(decomp);
        free(order);
        return false;
      }
      uint64_t cand_max = 0, cand_pairs = 0;
      if (!qsop_rankwidth_decomposition_forecast(qsop, decomp, &cand_max, &cand_pairs, error)) {
        qsop_rankwidth_decomposition_free(decomp);
        free(order);
        return false;
      }
      if (cand_max < best_max || (cand_max == best_max && cand_pairs < best_pairs)) {
        best_max = cand_max;
        best_pairs = cand_pairs;
        improved = true;
      } else {
        tmp = order[i];
        order[i] = order[i + 1U];
        order[i + 1U] = tmp;
      }
    }
  }

  if (!fill_left_deep_from_order(decomp, order, error)) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    return false;
  }

  free(order);
  *out = decomp;
  return true;
}

static bool make_left_deep_generated_decomposition(const qsop_instance_t *qsop,
                                                   qsop_rankwidth_decomposition_t **out,
                                                   qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    set_error(error, "internal error: null left-deep rankwidth generation argument");
    return false;
  }
  *out = NULL;

  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    set_error(error, "out of memory while allocating left-deep rankwidth decomposition");
    return false;
  }

  const size_t words = qsop_bitset_words(qsop->nvars);
  decomposition->nvars = qsop->nvars;
  decomposition->words = words;
  decomposition->nnodes = 2U * qsop->nvars - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * decomposition->words,
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    qsop_rankwidth_decomposition_free(decomposition);
    set_error(error, "out of memory while allocating left-deep rankwidth decomposition nodes");
    return false;
  }

  for (uint32_t i = 0; i < qsop->nvars; i++) {
    decomposition->nodes[i] = (rw_node_t){
        .kind = RW_NODE_LEAF,
        .var = i,
    };
  }
  if (qsop->nvars == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t current = 0;
    uint32_t next_join = qsop->nvars;
    for (uint32_t i = 1; i < qsop->nvars; i++) {
      const uint32_t node = next_join++;
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = current,
          .right = i,
      };
      current = node;
    }
    decomposition->root = current;
  }

  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  *out = decomposition;
  return true;
}

/* Replay the min-fill elimination order to build the fill-in (chordal) graph,
 * then find each position's parent in the elimination tree (the earliest
 * subsequently-eliminated fill-in neighbor).  The fill array is n*words bitsets;
 * parent_pos[pos] = UINT32_MAX for the root (last eliminated). */
static bool make_fill_in_and_parents(const qsop_instance_t *qsop, const uint32_t *order,
                                     uint64_t *fill, uint32_t *parent_pos,
                                     qsop_error_t *error) {
  const uint32_t n = qsop->nvars;
  const size_t words = qsop_bitset_words(n);
  uint64_t *active = calloc(words == 0 ? 1U : words, sizeof(*active));
  uint64_t *scratch = calloc(words == 0 ? 1U : words, sizeof(*scratch));
  if (active == NULL || scratch == NULL) {
    free(active);
    free(scratch);
    set_error(error, "out of memory building fill-in graph");
    return false;
  }

  for (uint32_t v = 0; v < n; v++) {
    qsop_bitset_set(active, v);
  }

  /* Replay elimination: at each step, clique-ify active neighbors. */
  for (uint32_t pos = 0; pos < n; pos++) {
    const uint32_t v = order[pos];
    uint64_t *v_row = qsop_bitset_row(fill, words, v);

    /* Collect active neighbors into scratch. */
    for (size_t w = 0; w < words; w++) {
      scratch[w] = v_row[w] & active[w];
    }

    /* Add fill edges: connect all pairs of active neighbors. */
    for (uint32_t u = 0; u < n; u++) {
      if (!qsop_bitset_get(scratch, u)) {
        continue;
      }
      uint64_t *u_row = qsop_bitset_row(fill, words, u);
      for (size_t w = 0; w < words; w++) {
        u_row[w] |= scratch[w];
      }
      qsop_bitset_clear(u_row, u); /* no self-loop */
    }

    /* Remove v from active. */
    qsop_bitset_clear(active, v);
  }

  /* Build pos_of inverse map. */
  uint32_t *pos_of = calloc(n == 0 ? 1U : n, sizeof(*pos_of));
  if (pos_of == NULL) {
    free(active);
    free(scratch);
    set_error(error, "out of memory building pos_of map");
    return false;
  }
  for (uint32_t pos = 0; pos < n; pos++) {
    pos_of[order[pos]] = pos;
  }

  /* For each pos, parent = fill-in neighbor with smallest pos > pos. */
  for (uint32_t pos = 0; pos < n; pos++) {
    const uint32_t v = order[pos];
    const uint64_t *v_row = qsop_bitset_const_row(fill, words, v);
    uint32_t best = UINT32_MAX;
    for (uint32_t u = 0; u < n; u++) {
      if (!qsop_bitset_get(v_row, u)) {
        continue;
      }
      const uint32_t q = pos_of[u];
      if (q > pos && (best == UINT32_MAX || q < best)) {
        best = q;
      }
    }
    parent_pos[pos] = best;
  }

  free(active);
  free(scratch);
  free(pos_of);
  return true;
}

/* Build rank decomposition subtree for elimination-tree node at position pos.
 * child_subtrees[pos] must already hold the subtree root ids for each child.
 * Returns the root node id of the subtree rooted at pos. */
typedef struct etree_dfs_frame {
  uint32_t pos;
  uint32_t child_idx; /* which child we're processing next */
} etree_dfs_frame_t;

static bool build_etree_subtrees(qsop_rankwidth_decomposition_t *d, uint32_t root_pos,
                                 uint32_t **children_arr, uint32_t *children_cnt,
                                 uint32_t *subtree_root, uint32_t n, uint32_t *next_join,
                                 qsop_error_t *error) {
  /* Iterative post-order DFS to avoid stack overflow on deep trees. */
  uint32_t *stack = calloc(n == 0 ? 1U : n, sizeof(*stack));
  uint32_t *result = calloc(n == 0 ? 1U : n, sizeof(*result));
  if (stack == NULL || result == NULL) {
    free(stack);
    free(result);
    set_error(error, "out of memory building from-treewidth subtrees");
    return false;
  }
  uint32_t sp = 0;
  stack[sp++] = root_pos;

  /* First pass: topological sort (post-order) via DFS. */
  uint32_t *visit_order = calloc(n == 0 ? 1U : n, sizeof(*visit_order));
  if (visit_order == NULL) {
    free(stack);
    free(result);
    set_error(error, "out of memory building from-treewidth visit order");
    return false;
  }
  uint32_t visit_len = 0;

  /* Iterative DFS for post-order: push right-to-left so left is processed first. */
  while (sp > 0) {
    const uint32_t pos = stack[--sp];
    visit_order[visit_len++] = pos;
    /* Push children right-to-left so they're processed before this node. */
    for (uint32_t i = children_cnt[pos]; i > 0U; i--) {
      stack[sp++] = children_arr[pos][i - 1U];
    }
  }

  /* Second pass: build rank decomposition nodes in reverse visit order (post-order). */
  /* But visit_order above is pre-order; reverse it for post-order. */
  for (uint32_t i = visit_len; i > 0U; i--) {
    const uint32_t pos = visit_order[i - 1U];
    const uint32_t own_leaf = pos; /* node index = pos for leaves */
    const uint32_t k = children_cnt[pos];
    if (k == 0U) {
      result[pos] = own_leaf;
    } else {
      /* Collect child subtree roots. */
      uint32_t current = result[children_arr[pos][0]];
      for (uint32_t j = 1U; j < k; j++) {
        const uint32_t node = (*next_join)++;
        d->nodes[node] = (rw_node_t){
            .kind = RW_NODE_JOIN,
            .left = current,
            .right = result[children_arr[pos][j]],
        };
        current = node;
      }
      /* Join with own leaf. */
      const uint32_t node = (*next_join)++;
      d->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = current,
          .right = own_leaf,
      };
      result[pos] = node;
    }
  }

  *subtree_root = result[root_pos];
  free(stack);
  free(result);
  free(visit_order);
  return true;
}

static bool make_from_treewidth_decomposition(const qsop_instance_t *qsop,
                                              qsop_rankwidth_decomposition_t **out,
                                              qsop_error_t *error) {
  const uint32_t n = qsop->nvars;
  const size_t words = qsop_bitset_words(n);

  /* Step 1: compute min-fill elimination order. */
  uint32_t *order = calloc(n == 0 ? 1U : n, sizeof(*order));
  if (order == NULL) {
    set_error(error, "out of memory in from-treewidth generator");
    return false;
  }
  for (uint32_t v = 0; v < n; v++) {
    order[v] = v;
  }
  if (!make_min_fill_order(qsop, order, error)) {
    free(order);
    return false;
  }

  /* Step 2: replay elimination to get fill-in graph + elimination tree parents. */
  uint64_t *fill = calloc((n == 0 ? 1U : n) * (words == 0 ? 1U : words), sizeof(*fill));
  uint32_t *parent_pos = calloc(n == 0 ? 1U : n, sizeof(*parent_pos));
  if (fill == NULL || parent_pos == NULL) {
    free(order);
    free(fill);
    free(parent_pos);
    set_error(error, "out of memory in from-treewidth fill-in allocation");
    return false;
  }
  /* Initialize fill from original graph. */
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  if (!make_fill_in_and_parents(qsop, order, fill, parent_pos, error)) {
    free(order);
    free(fill);
    free(parent_pos);
    return false;
  }
  free(fill); /* no longer needed */

  /* Step 3: build children lists for elimination tree. */
  uint32_t *children_flat = calloc(n == 0 ? 1U : n, sizeof(*children_flat));
  uint32_t **children_arr = calloc(n == 0 ? 1U : n, sizeof(*children_arr));
  uint32_t *children_cnt = calloc(n == 0 ? 1U : n, sizeof(*children_cnt));
  if (children_flat == NULL || children_arr == NULL || children_cnt == NULL) {
    free(order);
    free(parent_pos);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    set_error(error, "out of memory building elimination tree children");
    return false;
  }

  uint32_t root_pos = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] == UINT32_MAX) {
      root_pos = pos;
    } else {
      children_cnt[parent_pos[pos]]++;
    }
  }

  /* Allocate children_arr[pos] using offsets into children_flat. */
  uint32_t offset = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    children_arr[pos] = children_flat + offset;
    offset += children_cnt[pos];
    children_cnt[pos] = 0; /* reset for fill pass */
  }
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] != UINT32_MAX) {
      const uint32_t par = parent_pos[pos];
      children_arr[par][children_cnt[par]++] = pos;
    }
  }
  free(parent_pos);

  /* Step 4: allocate decomposition (2n-1 nodes). */
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    free(order);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    set_error(error, "out of memory allocating from-treewidth decomposition");
    return false;
  }
  decomposition->nvars = n;
  decomposition->words = words;
  decomposition->nnodes = n <= 1U ? 1U : 2U * n - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * (words == 0 ? 1U : words),
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    free(order);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    qsop_rankwidth_decomposition_free(decomposition);
    set_error(error, "out of memory allocating from-treewidth decomposition nodes");
    return false;
  }

  /* Leaf nodes: node[pos].var = order[pos] (same layout as other generators). */
  for (uint32_t pos = 0; pos < n; pos++) {
    decomposition->nodes[pos] = (rw_node_t){.kind = RW_NODE_LEAF, .var = order[pos]};
  }
  free(order);

  if (n == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t next_join = n;
    uint32_t subtree_root = 0;
    if (!build_etree_subtrees(decomposition, root_pos, children_arr, children_cnt,
                              &subtree_root, n, &next_join, error)) {
      free(children_flat);
      free(children_arr);
      free(children_cnt);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    decomposition->root = subtree_root;
  }

  free(children_flat);
  free(children_arr);
  free(children_cnt);

  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  *out = decomposition;
  return true;
}

/* Build a from-treewidth decomposition using a caller-supplied elimination order,
 * avoiding a second min-fill run when the caller already holds one from treewidth solving. */
bool qsop_rankwidth_decomposition_from_order(const qsop_instance_t *qsop,
                                             const uint32_t *order,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    set_error(error, "internal error: null argument to rankwidth decomposition from-order");
    return false;
  }
  *out = NULL;
  if (qsop->nvars == 0) {
    qsop_rankwidth_decomposition_t *empty = calloc(1, sizeof(*empty));
    if (empty == NULL) {
      set_error(error, "out of memory while allocating empty rankwidth decomposition");
      return false;
    }
    *out = empty;
    return true;
  }
  if (order == NULL) {
    set_error(error, "internal error: null order for rankwidth decomposition from-order");
    return false;
  }
  /* Copy the provided order into a writable buffer (make_fill_in_and_parents needs a uint32_t[]). */
  const uint32_t n = qsop->nvars;
  uint32_t *order_copy = malloc(n * sizeof(*order_copy));
  if (order_copy == NULL) {
    set_error(error, "out of memory while copying order for rankwidth from-order");
    return false;
  }
  memcpy(order_copy, order, n * sizeof(*order_copy));

  /* Delegate to the internal from-treewidth builder, which owns order_copy. */
  const size_t words = qsop_bitset_words(n);
  uint64_t *fill = calloc((size_t)n * (words == 0 ? 1U : words), sizeof(*fill));
  uint32_t *parent_pos = calloc(n, sizeof(*parent_pos));
  if (fill == NULL || parent_pos == NULL) {
    free(order_copy);
    free(fill);
    free(parent_pos);
    set_error(error, "out of memory in from-order fill-in allocation");
    return false;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  if (!make_fill_in_and_parents(qsop, order_copy, fill, parent_pos, error)) {
    free(order_copy);
    free(fill);
    free(parent_pos);
    return false;
  }
  free(fill);
  free(order_copy);

  /* Build children lists, then the decomposition tree (same as make_from_treewidth_decomposition). */
  uint32_t *children_flat = calloc(n, sizeof(*children_flat));
  uint32_t **children_arr = calloc(n, sizeof(*children_arr));
  uint32_t *children_cnt  = calloc(n, sizeof(*children_cnt));
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (children_flat == NULL || children_arr == NULL || children_cnt == NULL ||
      decomposition == NULL) {
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    free(parent_pos);
    qsop_rankwidth_decomposition_free(decomposition);
    set_error(error, "out of memory in from-order children allocation");
    return false;
  }
  decomposition->nvars = n;
  decomposition->words = words;
  decomposition->nnodes = 2U * n - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * words,
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    free(parent_pos);
    qsop_rankwidth_decomposition_free(decomposition);
    set_error(error, "out of memory in from-order decomposition nodes");
    return false;
  }

  /* Build children lists from parent_pos (same logic as make_from_treewidth_decomposition). */
  uint32_t root_pos = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] == UINT32_MAX) {
      root_pos = pos;
    } else {
      children_cnt[parent_pos[pos]]++;
    }
  }
  uint32_t offset = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    children_arr[pos] = children_flat + offset;
    offset += children_cnt[pos];
    children_cnt[pos] = 0;
  }
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] != UINT32_MAX) {
      const uint32_t par = parent_pos[pos];
      children_arr[par][children_cnt[par]++] = pos;
    }
  }
  free(parent_pos);

  for (uint32_t i = 0; i < n; i++) {
    decomposition->nodes[i] = (rw_node_t){.kind = RW_NODE_LEAF, .var = order[i]};
  }
  if (n == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t next_join = n;
    uint32_t subtree_root = 0;
    if (!build_etree_subtrees(decomposition, root_pos, children_arr, children_cnt,
                              &subtree_root, n, &next_join, error)) {
      free(children_flat);
      free(children_arr);
      free(children_cnt);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    decomposition->root = subtree_root;
  }
  free(children_flat);
  free(children_arr);
  free(children_cnt);

  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  *out = decomposition;
  return true;
}

bool qsop_rankwidth_decomposition_generate(const qsop_instance_t *qsop,
                                           qsop_rankwidth_generator_t generator,
                                           qsop_rankwidth_decomposition_t **out,
                                           qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    set_error(error, "internal error: null rankwidth decomposition generation argument");
    return false;
  }
  *out = NULL;
  if (qsop->nvars == 0) {
    qsop_rankwidth_decomposition_t *empty = calloc(1, sizeof(*empty));
    if (empty == NULL) {
      set_error(error, "out of memory while allocating empty rankwidth decomposition");
      return false;
    }
    *out = empty;
    return true;
  }

  if (generator == QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH) {
    return make_from_treewidth_decomposition(qsop, out, error);
  }

  /* BEST: generate all base generators (including search), return lowest forecast. */
  if (generator == QSOP_RANKWIDTH_GENERATOR_BEST) {
    static const qsop_rankwidth_generator_t kCandidates[] = {
        QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP,
        QSOP_RANKWIDTH_GENERATOR_BALANCED,
        QSOP_RANKWIDTH_GENERATOR_MIN_FILL,
        QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
        QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH,
    };
    const size_t ncandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);
    qsop_rankwidth_decomposition_t *winner = NULL;
    uint64_t winner_max = UINT64_MAX, winner_pairs = UINT64_MAX;
    for (size_t k = 0; k < ncandidates; k++) {
      qsop_rankwidth_decomposition_t *cand = NULL;
      if (!qsop_rankwidth_decomposition_generate(qsop, kCandidates[k], &cand, error)) {
        qsop_rankwidth_decomposition_free(winner);
        return false;
      }
      uint64_t cand_max = 0, cand_pairs = 0;
      if (!qsop_rankwidth_decomposition_forecast(qsop, cand, &cand_max, &cand_pairs, error)) {
        qsop_rankwidth_decomposition_free(cand);
        qsop_rankwidth_decomposition_free(winner);
        return false;
      }
      if (winner == NULL || cand_max < winner_max ||
          (cand_max == winner_max && cand_pairs < winner_pairs)) {
        qsop_rankwidth_decomposition_free(winner);
        winner = cand;
        winner_max = cand_max;
        winner_pairs = cand_pairs;
      } else {
        qsop_rankwidth_decomposition_free(cand);
      }
    }
    *out = winner;
    return true;
  }

  /* MIN_FILL_SEARCH: min-fill ordering refined by adjacent-swap hill climbing. */
  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH) {
    uint32_t *mfs_order = calloc(qsop->nvars, sizeof(*mfs_order));
    if (mfs_order == NULL) {
      set_error(error, "out of memory while allocating min-fill-search order");
      return false;
    }
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      mfs_order[v] = v;
    }
    if (!make_min_fill_order(qsop, mfs_order, error)) {
      free(mfs_order);
      return false;
    }
    const bool ok = generate_left_deep_search(qsop, mfs_order, out, error);
    free(mfs_order);
    return ok;
  }

  uint32_t *order = calloc(qsop->nvars, sizeof(*order));
  uint32_t *leaf_nodes = calloc(qsop->nvars, sizeof(*leaf_nodes));
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (order == NULL || leaf_nodes == NULL || decomposition == NULL) {
    free(order);
    free(leaf_nodes);
    free(decomposition);
    set_error(error, "out of memory while allocating generated rankwidth decomposition");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    order[v] = v;
  }
  if ((generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL ||
       generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT) &&
      !make_min_fill_order(qsop, order, error)) {
    free(order);
    free(leaf_nodes);
    free(decomposition);
    return false;
  }

  const size_t words = qsop_bitset_words(qsop->nvars);
  uint64_t *adj = NULL;
  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT && qsop->nvars > 1U) {
    adj = adjacency_bitsets(qsop, words, error);
    if (adj == NULL) {
      free(order);
      free(leaf_nodes);
      free(decomposition);
      return false;
    }
  }

  decomposition->nvars = qsop->nvars;
  decomposition->words = words;
  decomposition->nnodes = 2U * qsop->nvars - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * decomposition->words,
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    free(adj);
    free(order);
    free(leaf_nodes);
    qsop_rankwidth_decomposition_free(decomposition);
    set_error(error, "out of memory while allocating generated rankwidth decomposition nodes");
    return false;
  }

  for (uint32_t i = 0; i < qsop->nvars; i++) {
    decomposition->nodes[i] = (rw_node_t){
        .kind = RW_NODE_LEAF,
        .var = order[i],
    };
    leaf_nodes[i] = i;
  }

  if (qsop->nvars == 1U) {
    decomposition->root = 0;
  } else if (generator == QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP) {
    uint32_t current = leaf_nodes[0];
    uint32_t next_join = qsop->nvars;
    for (uint32_t i = 1; i < qsop->nvars; i++) {
      const uint32_t node = next_join++;
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = current,
          .right = leaf_nodes[i],
      };
      current = node;
    }
    decomposition->root = current;
  } else if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT) {
    uint64_t *prefix_masks =
        calloc(((size_t)qsop->nvars + 1U) * words, sizeof(*prefix_masks));
    uint64_t *all = calloc(words == 0 ? 1U : words, sizeof(*all));
    if (prefix_masks == NULL || all == NULL) {
      free(prefix_masks);
      free(all);
      free(adj);
      free(order);
      free(leaf_nodes);
      qsop_rankwidth_decomposition_free(decomposition);
      set_error(error, "out of memory while building rankwidth cut-rank prefix masks");
      return false;
    }
    for (uint32_t i = 0; i < qsop->nvars; i++) {
      uint64_t *next = qsop_bitset_row(prefix_masks, words, i + 1U);
      const uint64_t *prev = qsop_bitset_const_row(prefix_masks, words, i);
      qsop_bitset_copy(next, prev, words);
      qsop_bitset_set(next, order[i]);
      qsop_bitset_set(all, order[i]);
    }

    uint32_t next_join = qsop->nvars;
    decomposition->root = build_cut_rank_nodes(decomposition, leaf_nodes, prefix_masks, all, adj,
                                               0, qsop->nvars, &next_join, error);
    free(prefix_masks);
    free(all);
    if (decomposition->root == UINT32_MAX) {
      free(adj);
      free(order);
      free(leaf_nodes);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
  } else {
    uint32_t next_join = qsop->nvars;
    decomposition->root =
        build_balanced_nodes(decomposition, leaf_nodes, 0, qsop->nvars, &next_join);
  }

  free(order);
  free(leaf_nodes);
  if (!validate_decomposition(decomposition, error)) {
    free(adj);
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }

  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT && adj != NULL) {
    uint32_t *score_coeffs = NULL;
    if (!qsop_is_sign_edge_instance(qsop)) {
      score_coeffs = coefficient_matrix(qsop, error);
      if (score_coeffs == NULL) {
        free(adj);
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
    }

    rw_decomposition_score_t selected_score = {0};
    if (!decomposition_score(qsop, decomposition, adj, score_coeffs, NULL, &selected_score,
                             error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }

    qsop_rankwidth_decomposition_t *min_fill = NULL;
    if (!qsop_rankwidth_decomposition_generate(qsop, QSOP_RANKWIDTH_GENERATOR_MIN_FILL, &min_fill,
                                               error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    rw_decomposition_score_t min_fill_score = {0};
    if (!decomposition_score(qsop, min_fill, adj, score_coeffs, NULL, &min_fill_score,
                             error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(min_fill);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (compare_decomposition_scores(min_fill_score, selected_score) < 0) {
      qsop_rankwidth_decomposition_free(decomposition);
      decomposition = min_fill;
      min_fill = NULL;
      selected_score = min_fill_score;
    }
    qsop_rankwidth_decomposition_free(min_fill);

    qsop_rankwidth_decomposition_t *left_deep = NULL;
    if (!make_left_deep_generated_decomposition(qsop, &left_deep, error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    rw_decomposition_score_t left_deep_score = {0};
    if (!decomposition_score(qsop, left_deep, adj, score_coeffs, NULL, &left_deep_score,
                             error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(left_deep);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (compare_decomposition_scores(left_deep_score, selected_score) <= 0) {
      qsop_rankwidth_decomposition_free(decomposition);
      decomposition = left_deep;
      left_deep = NULL;
    }
    qsop_rankwidth_decomposition_free(left_deep);
    free(score_coeffs);
  }

  free(adj);
  *out = decomposition;
  return true;
}

void qsop_rankwidth_decomposition_free(qsop_rankwidth_decomposition_t *decomposition) {
  if (decomposition == NULL) {
    return;
  }
  free(decomposition->nodes);
  free(decomposition->node_vars);
  free(decomposition->postorder);
  free(decomposition);
}

static bool qsop_is_sign_edge_instance(const qsop_instance_t *qsop) {
  const uint32_t sign = qsop->r / 2U;
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (qsop->edge_q[e] != sign) {
      return false;
    }
  }
  return true;
}

static uint64_t *adjacency_bitsets(const qsop_instance_t *qsop, size_t words,
                                   qsop_error_t *error) {
  uint64_t *adj = calloc((qsop->nvars == 0 ? 1U : qsop->nvars) * words, sizeof(*adj));
  if (adj == NULL) {
    set_error(error, "out of memory while allocating rankwidth adjacency bitsets");
    return NULL;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(adj, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(adj, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  return adj;
}

static uint32_t *coefficient_matrix(const qsop_instance_t *qsop, qsop_error_t *error) {
  const size_t nvars = qsop->nvars == 0 ? 1U : (size_t)qsop->nvars;
  if (nvars > SIZE_MAX / nvars / sizeof(uint32_t)) {
    set_error(error, "rankwidth coefficient matrix is too large");
    return NULL;
  }
  uint32_t *coeffs = calloc(nvars * nvars, sizeof(*coeffs));
  if (coeffs == NULL) {
    set_error(error, "out of memory while allocating rankwidth coefficient matrix");
    return NULL;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint32_t u = qsop->edge_u[e];
    const uint32_t v = qsop->edge_v[e];
    const uint32_t q = qsop->edge_q[e] % qsop->r;
    coeffs[(size_t)u * qsop->nvars + v] = q;
    coeffs[(size_t)v * qsop->nvars + u] = q;
  }
  return coeffs;
}

static uint32_t cut_rank_bitsets(uint32_t nvars, const uint64_t *adj, const uint64_t *left,
                                 const uint64_t *right, size_t words, qsop_error_t *error) {
  uint64_t *rows = calloc((nvars == 0 ? 1U : nvars) * words, sizeof(*rows));
  if (rows == NULL) {
    set_error(error, "out of memory while computing rankwidth cut rank");
    return UINT32_MAX;
  }
  uint32_t nrows = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_bitset_get(left, v)) {
      const uint64_t *source = qsop_bitset_const_row(adj, words, v);
      uint64_t *target = qsop_bitset_row(rows, words, nrows++);
      for (size_t w = 0; w < words; w++) {
        target[w] = source[w] & right[w];
      }
    }
  }
  const uint32_t rank = qsop_gf2_rank_bitsets(rows, nrows, nvars, words);
  free(rows);
  return rank;
}

static uint32_t decomposition_width(const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_error_t *error) {
  uint64_t *all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  uint64_t *right = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*right));
  if (all == NULL || right == NULL) {
    free(all);
    free(right);
    set_error(error, "out of memory while computing rankwidth decomposition width");
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
    const uint32_t rank =
        cut_rank_bitsets(decomposition->nvars, adj, left, right, decomposition->words, error);
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

static uint32_t labelled_cut_signature_proxy_width(const qsop_instance_t *qsop,
                                                   const uint32_t *coeffs,
                                                   const uint64_t *left,
                                                   const uint64_t *right,
                                                   qsop_error_t *error) {
  const uint32_t nvars = qsop->nvars;
  if (nvars == 0) {
    return 0;
  }
  if ((size_t)nvars > SIZE_MAX / (size_t)nvars / sizeof(uint32_t)) {
    set_error(error, "labelled rankwidth cut-signature proxy is too large");
    return UINT32_MAX;
  }

  uint32_t *rows = calloc((size_t)nvars * nvars, sizeof(*rows));
  uint32_t *row = calloc(nvars, sizeof(*row));
  uint64_t *fingerprints = calloc(nvars, sizeof(*fingerprints));
  if (rows == NULL || row == NULL || fingerprints == NULL) {
    free(rows);
    free(row);
    free(fingerprints);
    set_error(error, "out of memory while computing labelled rankwidth cut-signature proxy");
    return UINT32_MAX;
  }

  uint32_t distinct_rows = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if (!qsop_bitset_get(left, v)) {
      continue;
    }
    memset(row, 0, (size_t)nvars * sizeof(*row));
    bool any = false;
    for (uint32_t u = 0; u < nvars; u++) {
      if (!qsop_bitset_get(right, u)) {
        continue;
      }
      const uint32_t q = coeffs[(size_t)v * nvars + u] % qsop->r;
      if (q != 0) {
        row[u] = q;
        any = true;
      }
    }
    if (!any) {
      continue;
    }

    const uint64_t fingerprint = label_signature_fingerprint(row, nvars);
    bool seen = false;
    for (uint32_t i = 0; i < distinct_rows; i++) {
      const uint32_t *candidate = rows + (size_t)i * nvars;
      if (fingerprints[i] == fingerprint &&
          memcmp(candidate, row, (size_t)nvars * sizeof(*row)) == 0) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      uint32_t *dst = rows + (size_t)distinct_rows * nvars;
      memcpy(dst, row, (size_t)nvars * sizeof(*dst));
      fingerprints[distinct_rows] = fingerprint;
      distinct_rows++;
    }
  }

  free(rows);
  free(row);
  free(fingerprints);
  return ceil_log2_u64((uint64_t)distinct_rows + 1U);
}

static bool labelled_cut_signature_exact_width(const qsop_instance_t *qsop, const uint32_t *coeffs,
                                               const uint64_t *left, const uint64_t *right,
                                               uint32_t *width_out,
                                               uint64_t *signature_count_out,
                                               uint64_t *assignments_out,
                                               bool *computed_out,
                                               qsop_error_t *error) {
  if (computed_out == NULL) {
    set_error(error, "internal error: null labelled rankwidth exact-estimator flag");
    return false;
  }
  *computed_out = false;
  const uint32_t nleft = qsop_bitset_popcount(left, qsop_bitset_words(qsop->nvars));
  const uint64_t assignment_space = nleft >= 64U ? UINT64_MAX : (UINT64_C(1) << nleft);
  uint32_t *row = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*row));
  uint32_t *signature = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*signature));
  rw_label_signature_pool_t pool = {0};
  if (row == NULL || signature == NULL || !label_signature_pool_init(&pool, qsop->nvars, error)) {
    free(row);
    free(signature);
    label_signature_pool_free(&pool);
    set_error(error, "out of memory while exactly estimating labelled rankwidth cut");
    return false;
  }

  uint32_t zero_signature = 0;
  if (!label_signature_pool_intern(&pool, signature, &zero_signature, error)) {
    free(row);
    free(signature);
    label_signature_pool_free(&pool);
    return false;
  }

  /* Exact frontier over distinct signatures; avoids enumerating duplicate assignments. */
  bool ok = true;
  uint64_t transitions = 0;
  for (uint32_t v = 0; ok && v < qsop->nvars; v++) {
    if (!qsop_bitset_get(left, v)) {
      continue;
    }

    memset(row, 0, (size_t)qsop->nvars * sizeof(*row));
    bool any = false;
    for (uint32_t u = 0; u < qsop->nvars; u++) {
      if (!qsop_bitset_get(right, u)) {
        continue;
      }
      row[u] = coeffs[(size_t)v * qsop->nvars + u] % qsop->r;
      any = any || row[u] != 0;
    }
    if (!any) {
      continue;
    }

    const size_t current_len = pool.len;
    if (current_len > RW_LABELLED_EXACT_SIGNATURE_MAX_TRANSITIONS - transitions) {
      ok = false;
      break;
    }
    transitions += current_len;
    for (size_t i = 0; i < current_len; i++) {
      const uint32_t *base = label_signature_coeffs_const(&pool, (uint32_t)i);
      for (uint32_t u = 0; u < qsop->nvars; u++) {
        signature[u] = (uint32_t)(((uint64_t)base[u] + row[u]) % qsop->r);
      }
      uint32_t signature_id = 0;
      if (!label_signature_pool_intern(&pool, signature, &signature_id, error)) {
        free(row);
        free(signature);
        label_signature_pool_free(&pool);
        return false;
      }
      if (pool.len > RW_LABELLED_EXACT_SIGNATURE_MAX_SIGNATURES) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    *width_out = ceil_log2_u64((uint64_t)pool.len);
    *signature_count_out = (uint64_t)pool.len;
    *assignments_out = assignment_space;
    *computed_out = true;
  }
  free(row);
  free(signature);
  label_signature_pool_free(&pool);
  return true;
}

static uint32_t labelled_cut_signature_width(const qsop_instance_t *qsop, const uint32_t *coeffs,
                                             const uint64_t *left, const uint64_t *right,
                                             uint64_t *signature_count_out,
                                             rw_labelled_cut_stats_t *stats,
                                             qsop_error_t *error) {
  uint32_t exact_width = 0;
  uint64_t exact_signatures = 0;
  uint64_t exact_assignments = 0;
  bool exact_computed = false;
  if (!labelled_cut_signature_exact_width(qsop, coeffs, left, right, &exact_width,
                                          &exact_signatures, &exact_assignments,
                                          &exact_computed, error)) {
    return UINT32_MAX;
  }
  if (exact_computed) {
    if (signature_count_out != NULL) {
      *signature_count_out = exact_signatures;
    }
    if (stats != NULL) {
      stats->exact_cuts++;
      stats->exact_assignments =
          saturating_add_u64(stats->exact_assignments, exact_assignments);
    }
    return exact_width;
  }

  if (stats != NULL) {
    stats->proxy_cuts++;
  }
  const uint32_t proxy_width = labelled_cut_signature_proxy_width(qsop, coeffs, left, right,
                                                                 error);
  if (proxy_width != UINT32_MAX && signature_count_out != NULL) {
    *signature_count_out = binary_signature_bound(proxy_width);
  }
  return proxy_width;
}

static uint32_t labelled_decomposition_width(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, rw_labelled_cut_stats_t *stats, qsop_error_t *error) {
  uint64_t *all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  uint64_t *right = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*right));
  if (all == NULL || right == NULL) {
    free(all);
    free(right);
    set_error(error, "out of memory while computing labelled rankwidth decomposition proxy");
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
    const uint32_t cut_width = labelled_cut_signature_width(qsop, coeffs, left, right, NULL, stats,
                                                            error);
    if (cut_width == UINT32_MAX) {
      free(all);
      free(right);
      return UINT32_MAX;
    }
    if (cut_width > width) {
      width = cut_width;
    }
  }

  free(all);
  free(right);
  return width;
}

static bool decomposition_score(const qsop_instance_t *qsop,
                                const qsop_rankwidth_decomposition_t *decomposition,
                                const uint64_t *adj, const uint32_t *coeffs,
                                rw_labelled_cut_stats_t *cut_stats,
                                rw_decomposition_score_t *out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null rankwidth decomposition score output");
    return false;
  }
  const uint32_t support_width = decomposition_width(decomposition, adj, error);
  if (support_width == UINT32_MAX) {
    return false;
  }
  uint32_t labelled_width = support_width;
  if (coeffs != NULL) {
    labelled_width = labelled_decomposition_width(qsop, decomposition, coeffs, cut_stats,
                                                  error);
    if (labelled_width == UINT32_MAX) {
      return false;
    }
  }
  uint64_t table_forecast = 0;
  uint64_t join_pair_forecast = 0;
  if (!qsop_rankwidth_decomposition_forecast(qsop, decomposition, &table_forecast,
                                             &join_pair_forecast, error)) {
    return false;
  }
  *out = (rw_decomposition_score_t){
      .labelled_width = labelled_width,
      .support_width = support_width,
      .table_forecast = table_forecast,
      .join_pair_forecast = join_pair_forecast,
      .labelled_exact_cuts = cut_stats == NULL ? 0 : cut_stats->exact_cuts,
      .labelled_proxy_cuts = cut_stats == NULL ? 0 : cut_stats->proxy_cuts,
      .labelled_exact_assignments = cut_stats == NULL ? 0 : cut_stats->exact_assignments,
  };
  return true;
}

bool qsop_rankwidth_decomposition_support_width(
    const qsop_instance_t *qsop, qsop_rankwidth_decomposition_t *decomposition,
    uint32_t *out, qsop_error_t *error) {
  return qsop_rankwidth_decomposition_widths(qsop, decomposition, out, NULL, error);
}

bool qsop_rankwidth_decomposition_widths(
    const qsop_instance_t *qsop, qsop_rankwidth_decomposition_t *decomposition,
    uint32_t *support_width_out, uint32_t *labelled_width_out, qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL ||
      (support_width_out == NULL && labelled_width_out == NULL)) {
    set_error(error, "internal error: null rankwidth width argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }

  uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
  if (adj == NULL) {
    return false;
  }

  uint32_t *coeffs = NULL;
  if (labelled_width_out != NULL && !qsop_is_sign_edge_instance(qsop)) {
    coeffs = coefficient_matrix(qsop, error);
    if (coeffs == NULL) {
      free(adj);
      return false;
    }
  }

  rw_labelled_cut_stats_t cut_stats = {0};
  rw_decomposition_score_t score = {0};
  const bool ok = decomposition_score(qsop, decomposition, adj, coeffs, &cut_stats, &score,
                                      error);
  free(coeffs);
  free(adj);
  if (!ok) {
    return false;
  }
  if (support_width_out != NULL) {
    *support_width_out = score.support_width;
  }
  if (labelled_width_out != NULL) {
    *labelled_width_out = score.labelled_width;
  }
  /* Cache the full score so rankwidth_record_decomposition_diagnostics can skip
   * recomputing decomposition_score when the decomposition is immediately solved. */
  decomposition->score_cached              = true;
  decomposition->cached_support_width      = score.support_width;
  decomposition->cached_labelled_width     = score.labelled_width;
  decomposition->cached_table_forecast     = score.table_forecast;
  decomposition->cached_join_pair_forecast = score.join_pair_forecast;
  decomposition->cached_exact_cuts         = score.labelled_exact_cuts;
  decomposition->cached_proxy_cuts         = score.labelled_proxy_cuts;
  decomposition->cached_exact_assignments  = score.labelled_exact_assignments;
  return true;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t saturating_mul_u64(uint64_t left, uint64_t right) {
  if (left != 0 && right > UINT64_MAX / left) {
    return UINT64_MAX;
  }
  return left * right;
}

static uint64_t binary_signature_bound(uint32_t width) {
  if (width >= 64U) {
    return UINT64_MAX;
  }
  return UINT64_C(1) << width;
}

static uint64_t min_u64(uint64_t left, uint64_t right) {
  return left < right ? left : right;
}

bool qsop_rankwidth_decomposition_forecast(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint64_t *max_table_entries_out, uint64_t *join_pairs_out, qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL ||
      (max_table_entries_out == NULL && join_pairs_out == NULL)) {
    set_error(error, "internal error: null rankwidth forecast argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    set_error(error, "rankwidth decomposition variable count does not match QSOP");
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

  uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
  uint32_t *coeffs = NULL;
  uint64_t *all = NULL;
  uint64_t *right = NULL;
  uint64_t *signature_counts = NULL;
  if (adj == NULL) {
    return false;
  }
  if (!qsop_is_sign_edge_instance(qsop)) {
    coeffs = coefficient_matrix(qsop, error);
    if (coeffs == NULL) {
      free(adj);
      return false;
    }
  }

  all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  right = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*right));
  signature_counts =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*signature_counts));
  if (all == NULL || right == NULL || signature_counts == NULL) {
    free(adj);
    free(coeffs);
    free(all);
    free(right);
    free(signature_counts);
    set_error(error, "out of memory while forecasting rankwidth table pressure");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(all, v);
  }

  rw_labelled_cut_stats_t cut_stats = {0};
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
      uint32_t width = 0;
      if (coeffs != NULL) {
        width = labelled_cut_signature_width(qsop, coeffs, left, right, &signature_cap,
                                             &cut_stats, error);
      } else {
        width = cut_rank_bitsets(decomposition->nvars, adj, left, right,
                                 decomposition->words, error);
      }
      if (width == UINT32_MAX) {
        ok = false;
        break;
      }
      if (coeffs == NULL) {
        signature_cap = binary_signature_bound(width);
      }
    }

    uint64_t signatures = 0;
    if (node->kind == RW_NODE_LEAF) {
      signatures = min_u64(2U, signature_cap);
    } else {
      const uint64_t pair_count =
          saturating_mul_u64(signature_counts[node->left], signature_counts[node->right]);
      join_pairs = saturating_add_u64(join_pairs, pair_count);
      signatures = min_u64(pair_count, signature_cap);
    }
    signature_counts[node_id] = signatures;
    const uint64_t table_entries = saturating_mul_u64(signatures, qsop->r);
    if (table_entries > max_table_entries) {
      max_table_entries = table_entries;
    }
  }

  free(adj);
  free(coeffs);
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

static bool rankwidth_record_decomposition_diagnostics(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats == NULL && trace == NULL) {
    return true;
  }

  const uint64_t start = qsop_trace_begin(trace);

  rw_decomposition_score_t score = {0};
  if (decomposition->score_cached) {
    /* Reuse the score computed by qsop_rankwidth_decomposition_widths; avoids
     * rebuilding adj + coeffs and re-running labelled_decomposition_width. */
    score.support_width              = decomposition->cached_support_width;
    score.labelled_width             = decomposition->cached_labelled_width;
    score.table_forecast             = decomposition->cached_table_forecast;
    score.join_pair_forecast         = decomposition->cached_join_pair_forecast;
    score.labelled_exact_cuts        = decomposition->cached_exact_cuts;
    score.labelled_proxy_cuts        = decomposition->cached_proxy_cuts;
    score.labelled_exact_assignments = decomposition->cached_exact_assignments;
  } else {
    uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
    uint32_t *coeffs = NULL;
    if (!qsop_is_sign_edge_instance(qsop)) {
      coeffs = coefficient_matrix(qsop, error);
      if (coeffs == NULL) {
        free(adj);
        return false;
      }
    }
    rw_labelled_cut_stats_t cut_stats = {0};
    const bool ok = decomposition_score(qsop, decomposition, adj, coeffs, &cut_stats, &score,
                                        error);
    free(coeffs);
    free(adj);
    if (!ok) {
      return false;
    }
  }

  if (stats != NULL) {
    stats->rankwidth_support_width = score.support_width;
    stats->rankwidth_labelled_width = score.labelled_width;
    stats->rankwidth_table_forecast = score.table_forecast;
    stats->rankwidth_join_pair_forecast = score.join_pair_forecast;
    stats->rankwidth_labelled_exact_cuts = score.labelled_exact_cuts;
    stats->rankwidth_labelled_proxy_cuts = score.labelled_proxy_cuts;
    stats->rankwidth_labelled_exact_assignments = score.labelled_exact_assignments;
  }
  qsop_trace_emit_elapsed(trace, "rankwidth.width_probe", 0, score.labelled_width, start);
  qsop_trace_emit(trace, "rankwidth.support_width_probe", 0, score.support_width, 0);
  qsop_trace_emit(trace, "rankwidth.table_forecast", 0, score.table_forecast, 0);
  qsop_trace_emit(trace, "rankwidth.join_pair_forecast", 0, score.join_pair_forecast, 0);
  return true;
}

static uint32_t cross_parity_bitsets(uint32_t nvars, const uint64_t *adj,
                                     const uint64_t *left_assignment,
                                     const uint64_t *right_assignment, size_t words) {
  uint32_t parity = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_bitset_get(left_assignment, v)) {
      parity ^= qsop_bitset_popcount_intersection(qsop_bitset_const_row(adj, words, v),
                                                  right_assignment, words) &
                1U;
    }
  }
  return parity;
}

static uint32_t cross_labelled_residue(const qsop_instance_t *qsop, const uint32_t *coeffs,
                                       const uint64_t *left_assignment,
                                       const uint64_t *right_assignment) {
  uint32_t residue = 0;
  for (uint32_t u = 0; u < qsop->nvars; u++) {
    if (!qsop_bitset_get(left_assignment, u)) {
      continue;
    }
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if (!qsop_bitset_get(right_assignment, v)) {
        continue;
      }
      residue = (uint32_t)(((uint64_t)residue + coeffs[(size_t)u * qsop->nvars + v]) %
                           qsop->r);
    }
  }
  return residue;
}

static void labelled_parent_signature(const qsop_instance_t *qsop, const uint32_t *left,
                                      const uint32_t *right, const uint64_t *outside,
                                      uint32_t *out) {
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (qsop_bitset_get(outside, v)) {
      out[v] = (uint32_t)(((uint64_t)left[v] + right[v]) % qsop->r);
    } else {
      out[v] = 0;
    }
  }
}

static uint32_t ceil_log2_u64(uint64_t value) {
  if (value <= 1U) {
    return 0;
  }
  uint32_t width = 0;
  uint64_t threshold = 1;
  while (threshold < value && threshold <= UINT64_MAX / 2U) {
    threshold <<= 1U;
    width++;
  }
  return width;
}

/* D2.1: Pure transition helper for sign-edge (non-labelled) joins.
 * outside must be precomputed as (all_node_vars AND NOT node_vars[node_id]).
 * scratch_sig must be a caller-provided buffer of `words` uint64_t values (overwritten).
 * Always returns valid=true for sign-edge instances (no incompatible pairs). */
static bool rw_compute_join_transition_sign(
    uint32_t nvars, const uint64_t *adj, rw_signature_pool_t *pool,
    const uint64_t *outside, size_t words, uint32_t r,
    uint32_t left_signature, const uint64_t *left_rep,
    uint32_t right_signature, const uint64_t *right_rep,
    uint64_t *scratch_sig, rw_transition_eval_t *out, qsop_error_t *error) {
  const uint32_t sign = r / 2U;
  const uint32_t parity = cross_parity_bitsets(nvars, adj, left_rep, right_rep, words);
  qsop_bitset_copy(scratch_sig, signature_bits(pool, left_signature), words);
  qsop_bitset_xor(scratch_sig, signature_bits(pool, right_signature), words);
  qsop_bitset_and(scratch_sig, outside, words);
  uint32_t parent_sig = 0;
  if (!signature_pool_intern(pool, scratch_sig, &parent_sig, error)) {
    return false;
  }
  *out = (rw_transition_eval_t){
      .valid = true,
      .left_signature = left_signature,
      .right_signature = right_signature,
      .parent_signature = parent_sig,
      .residue_shift = (uint32_t)(((uint64_t)sign * parity) % r),
  };
  return true;
}

/* D4.1: Build CSR transition table for sign-edge joins (two-pass: count then fill).
 * outside and scratch_sig are caller-provided scratch (see build_join_map_arena).
 * Accumulates layout events into *u16_events and *u32_events when non-NULL. */
static bool rw_transition_csr_build_sign(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t node_id __attribute__((unused)), const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_table_t *left, const rw_table_t *right,
    const uint64_t *outside, uint64_t *scratch_sig,
    rw_transition_csr_t *out,
    uint64_t *u16_events, uint64_t *u32_events,
    qsop_error_t *error) {
  const size_t words = decomposition->words;
  const uint32_t r = qsop->r;
  const size_t lreps = left->reps_len;
  const size_t rreps = right->reps_len;
  if (lreps == 0 || rreps == 0) {
    return true;
  }

  /* Pass 1: count transitions per left rep and track max parent sig id. */
  uint64_t *counts = calloc(lreps, sizeof(*counts));
  if (counts == NULL) {
    set_error(error, "out of memory building CSR transition counts");
    return false;
  }
  uint32_t max_rsig = 0;
  uint32_t max_psig = 0;
  uint64_t total = 0;
  for (uint32_t i = 0; i < (uint32_t)lreps; i++) {
    const uint64_t *lrep = table_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)rreps; j++) {
      const uint64_t *rrep = table_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(qsop->nvars, adj, pool, outside, words, r,
                                           left->reps[i].signature, lrep,
                                           right->reps[j].signature, rrep,
                                           scratch_sig, &eval, error)) {
        free(counts);
        return false;
      }
      if (!eval.valid) continue;
      counts[i]++;
      total++;
      if (right->reps[j].signature > max_rsig) max_rsig = right->reps[j].signature;
      if (eval.parent_signature > max_psig) max_psig = eval.parent_signature;
    }
  }

  /* Determine layout. */
  const uint32_t left_sig_count = (uint32_t)lreps;
  rw_transition_layout_kind_t kind;
  if (left_sig_count <= UINT16_MAX && max_rsig <= UINT16_MAX &&
      max_psig <= UINT16_MAX && r <= UINT16_MAX) {
    kind = RW_TRANSITION_LAYOUT_U16;
    if (u16_events != NULL) (*u16_events)++;
  } else {
    kind = RW_TRANSITION_LAYOUT_U32;
    if (u32_events != NULL) (*u32_events)++;
  }

  /* Build offsets from counts. */
  uint32_t *left_signatures = malloc(left_sig_count * sizeof(*left_signatures));
  uint32_t *offsets = malloc((left_sig_count + 1U) * sizeof(*offsets));
  if (left_signatures == NULL || offsets == NULL) {
    free(counts); free(left_signatures); free(offsets);
    set_error(error, "out of memory building CSR offsets");
    return false;
  }
  for (uint32_t i = 0; i < left_sig_count; i++) {
    left_signatures[i] = left->reps[i].signature;
  }
  offsets[0] = 0;
  for (uint32_t i = 0; i < left_sig_count; i++) {
    offsets[i + 1U] = offsets[i] + (uint32_t)counts[i];
  }
  free(counts);

  /* Allocate items. */
  void *items = NULL;
  if (total > 0) {
    if (kind == RW_TRANSITION_LAYOUT_U16) {
      items = calloc(total, sizeof(rw_transition16_t));
    } else {
      items = calloc(total, sizeof(rw_transition32_t));
    }
    if (items == NULL) {
      free(left_signatures); free(offsets);
      set_error(error, "out of memory building CSR transition items");
      return false;
    }
  }

  /* Pass 2: fill items using cursor array. */
  uint32_t *cursors = malloc(left_sig_count * sizeof(*cursors));
  if (cursors == NULL) {
    free(left_signatures); free(offsets); free(items);
    set_error(error, "out of memory building CSR cursors");
    return false;
  }
  memcpy(cursors, offsets, left_sig_count * sizeof(*cursors));

  for (uint32_t i = 0; i < (uint32_t)lreps; i++) {
    const uint64_t *lrep = table_assignment(left, i, words);
    for (uint32_t j = 0; j < (uint32_t)rreps; j++) {
      const uint64_t *rrep = table_assignment(right, j, words);
      rw_transition_eval_t eval;
      if (!rw_compute_join_transition_sign(qsop->nvars, adj, pool, outside, words, r,
                                           left->reps[i].signature, lrep,
                                           right->reps[j].signature, rrep,
                                           scratch_sig, &eval, error)) {
        free(left_signatures); free(offsets); free(items); free(cursors);
        return false;
      }
      if (!eval.valid) continue;
      const uint32_t pos = cursors[i]++;
      if (kind == RW_TRANSITION_LAYOUT_U16) {
        rw_transition16_t *t = (rw_transition16_t *)items + pos;
        t->right_signature = (uint16_t)eval.right_signature;
        t->parent_signature = (uint16_t)eval.parent_signature;
        t->residue_shift = (uint16_t)eval.residue_shift;
        t->flags = 0;
      } else {
        rw_transition32_t *t = (rw_transition32_t *)items + pos;
        t->right_signature = eval.right_signature;
        t->parent_signature = eval.parent_signature;
        t->residue_shift = eval.residue_shift;
        t->flags = 0;
      }
    }
  }
  free(cursors);

  *out = (rw_transition_csr_t){
      .kind = kind,
      .left_sig_count = left_sig_count,
      .transition_count = total,
      .left_signatures = left_signatures,
      .offsets = offsets,
      .items = {.raw = items},
  };
  return true;
}

/* D4.1: CSR-based accumulator join for sign-edge count-table path.
 * Replaces solve_join_acc for the CSR materialized path.
 * D3.1: No join-map assignments; parent rep is reconstructed from left|right table reps. */
static bool rw_execute_csr_join_sign(
    const qsop_instance_t *qsop, const rw_transition_csr_t *csr,
    const rw_table_t *left, const rw_table_t *right, rw_table_t *out,
    size_t words, uint64_t *join_pairs_out, rw_join_workspace_t *ws,
    qsop_error_t *error) {
  if (csr->transition_count == 0) {
    return true;
  }

  /* Find max parent sig across all items. */
  uint32_t max_psig = 0;
  uint32_t max_lsig = 0;
  uint32_t max_rsig = 0;
  if (csr->kind == RW_TRANSITION_LAYOUT_U16) {
    for (uint64_t p = 0; p < csr->transition_count; p++) {
      const rw_transition16_t *t = csr->items.t16 + p;
      if (t->parent_signature > max_psig) max_psig = t->parent_signature;
      if (t->right_signature  > max_rsig) max_rsig  = t->right_signature;
    }
  } else {
    for (uint64_t p = 0; p < csr->transition_count; p++) {
      const rw_transition32_t *t = csr->items.t32 + p;
      if (t->parent_signature > max_psig) max_psig = t->parent_signature;
      if (t->right_signature  > max_rsig) max_rsig  = t->right_signature;
    }
  }
  for (uint32_t i = 0; i < csr->left_sig_count; i++) {
    if (csr->left_signatures[i] > max_lsig) max_lsig = csr->left_signatures[i];
  }

  const size_t n_psigs = (size_t)max_psig + 1U;
  const uint32_t r = qsop->r;
  const uint32_t r_mask = r - 1U;
  const bool r_pow2 = (r & r_mask) == 0;

  /* Allocate or reuse workspace. */
  const bool use_ws = ws != NULL && n_psigs <= ws->cap_sigs &&
                      (size_t)max_lsig < ws->cap_sigs &&
                      (size_t)max_rsig < ws->cap_sigs;
  uint64_t *acc;
  uint32_t *sig_map_left;    /* parent_sig -> left_rep_index for witness */
  uint32_t *sig_map_right;   /* parent_sig -> right_rep_index for witness */
  uint32_t *left_starts, *left_ends, *right_starts, *right_ends;
  if (use_ws) {
    acc          = ws->acc;
    left_starts  = ws->left_starts;
    left_ends    = ws->left_ends;
    right_starts = ws->right_starts;
    right_ends   = ws->right_ends;
    memset(acc, 0, n_psigs * r * sizeof(*acc));
    build_sig_range_index_into(left,  max_lsig, left_starts,  left_ends);
    build_sig_range_index_into(right, max_rsig, right_starts, right_ends);
  } else {
    acc = calloc(n_psigs * r, sizeof(*acc));
    left_starts = right_starts = left_ends = right_ends = NULL;
    if (acc == NULL ||
        !build_sig_range_index(left,  max_lsig, &left_starts,  &left_ends,  error) ||
        !build_sig_range_index(right, max_rsig, &right_starts, &right_ends, error)) {
      free(acc); free(left_starts); free(left_ends);
      free(right_starts); free(right_ends);
      if (acc == NULL) set_error(error, "out of memory in CSR join accumulator");
      return false;
    }
  }
  /* Per-parent-sig witness: store (left_rep_idx, right_rep_idx) for table_add_rep. */
  sig_map_left  = malloc(n_psigs * sizeof(*sig_map_left));
  sig_map_right = malloc(n_psigs * sizeof(*sig_map_right));
  if (sig_map_left == NULL || sig_map_right == NULL) {
    if (!use_ws) { free(acc); free(left_starts); free(left_ends); free(right_starts); free(right_ends); }
    free(sig_map_left); free(sig_map_right);
    set_error(error, "out of memory in CSR join witness map");
    return false;
  }
  memset(sig_map_left,  0xFF, n_psigs * sizeof(*sig_map_left));
  memset(sig_map_right, 0xFF, n_psigs * sizeof(*sig_map_right));

  /* Build rep-index lookup: sig -> rep index in left/right tables (one per sig). */
  uint32_t *left_rep_idx  = malloc(((size_t)max_lsig + 1U) * sizeof(*left_rep_idx));
  uint32_t *right_rep_idx = malloc(((size_t)max_rsig + 1U) * sizeof(*right_rep_idx));
  if (left_rep_idx == NULL || right_rep_idx == NULL) {
    if (!use_ws) { free(acc); free(left_starts); free(left_ends); free(right_starts); free(right_ends); }
    free(sig_map_left); free(sig_map_right); free(left_rep_idx); free(right_rep_idx);
    set_error(error, "out of memory building rep index for CSR join");
    return false;
  }
  memset(left_rep_idx,  0xFF, ((size_t)max_lsig + 1U) * sizeof(*left_rep_idx));
  memset(right_rep_idx, 0xFF, ((size_t)max_rsig + 1U) * sizeof(*right_rep_idx));
  for (size_t k = 0; k < left->reps_len; k++) {
    left_rep_idx[left->reps[k].signature] = (uint32_t)k;
  }
  for (size_t k = 0; k < right->reps_len; k++) {
    right_rep_idx[right->reps[k].signature] = (uint32_t)k;
  }

#define CSR_JOIN_CLEANUP() do {                                             \
    if (!use_ws) { free(acc); free(left_starts); free(left_ends);          \
                   free(right_starts); free(right_ends); }                 \
    free(sig_map_left); free(sig_map_right);                               \
    free(left_rep_idx); free(right_rep_idx);                               \
  } while (0)

  /* Main accumulation loop over CSR rows (left signatures). */
  uint64_t join_pairs = 0;
  for (uint32_t ci = 0; ci < csr->left_sig_count; ci++) {
    const uint32_t lsig = csr->left_signatures[ci];
    const uint32_t l_ri = left_rep_idx[lsig]; /* rep index in left table */
    if (l_ri == UINT32_MAX) continue;
    const size_t l_start = (left_starts[lsig] != UINT32_MAX) ? left_starts[lsig] : 0;
    const size_t l_end   = (left_starts[lsig] != UINT32_MAX) ? left_ends[lsig]   : 0;
    const uint32_t begin = csr->offsets[ci];
    const uint32_t end   = csr->offsets[ci + 1U];

    for (uint32_t p = begin; p < end; p++) {
      uint32_t rsig, psig, shift;
      if (csr->kind == RW_TRANSITION_LAYOUT_U16) {
        const rw_transition16_t *t = csr->items.t16 + p;
        rsig = t->right_signature; psig = t->parent_signature; shift = t->residue_shift;
      } else {
        const rw_transition32_t *t = csr->items.t32 + p;
        rsig = t->right_signature; psig = t->parent_signature; shift = t->residue_shift;
      }
      if (sig_map_left[psig] == UINT32_MAX) {
        sig_map_left[psig]  = l_ri;
        sig_map_right[psig] = (rsig <= max_rsig) ? right_rep_idx[rsig] : UINT32_MAX;
      }
      const size_t r_start = (right_starts[rsig] != UINT32_MAX) ? right_starts[rsig] : 0;
      const size_t r_end   = (right_starts[rsig] != UINT32_MAX) ? right_ends[rsig]   : 0;

      for (size_t i = l_start; i < l_end; i++) {
        const uint32_t l_res = left->entries[i].residue;
        const uint64_t l_cnt = left->entries[i].count;
        for (size_t j = r_start; j < r_end; j++) {
          const uint64_t rsum = (uint64_t)l_res + right->entries[j].residue + shift;
          const uint32_t res = r_pow2 ? (uint32_t)(rsum & r_mask) : (uint32_t)(rsum % r);
          uint64_t product = 0;
          if (!qsop_count_mul(l_cnt, right->entries[j].count, &product, error) ||
              !qsop_count_add(&acc[(size_t)psig * r + res], product, error)) {
            CSR_JOIN_CLEANUP();
            return false;
          }
          join_pairs++;
        }
      }
    }
  }

  /* Flush accumulator to output table. */
  size_t nonzero_count = 0;
  for (size_t i = 0; i < n_psigs * r; i++) {
    if (acc[i] != 0) nonzero_count++;
  }
  if (!reserve_entries(out, out->len + nonzero_count, error)) {
    CSR_JOIN_CLEANUP();
    return false;
  }

  /* Temp buffer for parent rep (left_rep | right_rep). D3.1: reconstructed, not stored. */
  const size_t w = words == 0 ? 1U : words;
  uint64_t *parent_rep = calloc(w, sizeof(*parent_rep));
  if (parent_rep == NULL) {
    CSR_JOIN_CLEANUP();
    set_error(error, "out of memory for CSR join parent rep");
    return false;
  }

  for (uint32_t s = 0; s < (uint32_t)n_psigs; s++) {
    if (sig_map_left[s] == UINT32_MAX) continue;
    const uint32_t li = sig_map_left[s];
    const uint32_t ri = sig_map_right[s];
    /* Reconstruct parent representative: left_rep | right_rep. */
    if (li != UINT32_MAX && ri != UINT32_MAX) {
      qsop_bitset_copy(parent_rep, table_assignment(left,  li, words), words);
      qsop_bitset_or  (parent_rep, table_assignment(right, ri, words), words);
    } else {
      memset(parent_rep, 0, w * sizeof(*parent_rep));
    }
    if (!table_add_rep(out, s, parent_rep, words, error)) {
      free(parent_rep);
      CSR_JOIN_CLEANUP();
      return false;
    }
    for (uint32_t res = 0; res < r; res++) {
      const uint64_t cnt = acc[(size_t)s * r + res];
      if (cnt == 0) continue;
      out->entries[out->len++] = (rw_entry_t){.signature = s, .residue = res, .count = cnt};
    }
  }
  free(parent_rep);
#undef CSR_JOIN_CLEANUP

  if (!use_ws) {
    free(acc); free(left_starts); free(left_ends);
    free(right_starts); free(right_ends);
  }
  free(sig_map_left);
  free(sig_map_right);
  free(left_rep_idx);
  free(right_rep_idx);

  if (join_pairs_out != NULL) *join_pairs_out += join_pairs;
  return true;
}

/* D2.2: Streaming join for sign-edge count-table path.
 * Iterates all (left_rep, right_rep) pairs, computes transitions on demand,
 * accumulates immediately without materializing the full transition table. */
static bool rw_join_count_table_streaming_sign(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition __attribute__((unused)),
    uint32_t node_id __attribute__((unused)), const uint64_t *adj, rw_signature_pool_t *pool,
    const rw_table_t *left, const rw_table_t *right, rw_table_t *out,
    const uint64_t *outside, uint64_t *scratch_sig,
    size_t words, uint64_t *candidate_pairs_out, uint64_t *emitted_pairs_out,
    qsop_error_t *error) {
  const uint32_t r = qsop->r;
  const uint32_t r_mask = r - 1U;
  const bool r_pow2 = (r & r_mask) == 0;
  const size_t lreps = left->reps_len;
  const size_t rreps = right->reps_len;
  if (lreps == 0 || rreps == 0) {
    return true;
  }

  uint32_t max_lsig = 0, max_rsig = 0;
  for (size_t i = 0; i < lreps; i++) {
    if (left->reps[i].signature > max_lsig) max_lsig = left->reps[i].signature;
  }
  for (size_t j = 0; j < rreps; j++) {
    if (right->reps[j].signature > max_rsig) max_rsig = right->reps[j].signature;
  }

  uint32_t *left_starts = NULL, *left_ends = NULL;
  uint32_t *right_starts = NULL, *right_ends = NULL;
  if (!build_sig_range_index(left,  max_lsig, &left_starts,  &left_ends,  error) ||
      !build_sig_range_index(right, max_rsig, &right_starts, &right_ends, error)) {
    free(left_starts); free(left_ends); free(right_starts); free(right_ends);
    return false;
  }

  uint64_t candidate_pairs = 0;
  uint64_t emitted_pairs = 0;

  /* We need a dynamic accumulator (parent_sig may grow during iteration). */
  /* Use a simple per-(sig,res) accumulator map built on-the-fly. */
  /* For correctness we accumulate into the output table directly using table_add_entry.
   * This is less efficient than the CSR path but avoids pre-knowing max_psig. */
  for (size_t i = 0; i < lreps; i++) {
    const uint64_t *lrep = table_assignment(left, i, words);
    const uint32_t lsig  = left->reps[i].signature;
    const size_t l_start = (left_starts[lsig] != UINT32_MAX) ? left_starts[lsig] : 0;
    const size_t l_end   = (left_starts[lsig] != UINT32_MAX) ? left_ends[lsig]   : 0;

    for (size_t j = 0; j < rreps; j++) {
      const uint64_t *rrep = table_assignment(right, j, words);
      rw_transition_eval_t eval;
      candidate_pairs++;
      if (!rw_compute_join_transition_sign(qsop->nvars, adj, pool, outside, words, r,
                                           lsig, lrep, right->reps[j].signature, rrep,
                                           scratch_sig, &eval, error)) {
        free(left_starts); free(left_ends); free(right_starts); free(right_ends);
        return false;
      }
      if (!eval.valid) continue;
      emitted_pairs++;

      const uint32_t rsig   = eval.right_signature;
      const uint32_t psig   = eval.parent_signature;
      const uint32_t shift  = eval.residue_shift;

      /* Add representative for parent signature (once per unique psig). */
      {
        uint64_t *parent_rep = scratch_sig; /* reuse scratch after transition is computed */
        const size_t w = words == 0 ? 1U : words;
        qsop_bitset_copy(parent_rep, lrep, words);
        qsop_bitset_or  (parent_rep, rrep, words);
        if (!table_add_rep(out, psig, parent_rep, words, error)) {
          free(left_starts); free(left_ends); free(right_starts); free(right_ends);
          return false;
        }
        /* scratch_sig is now clobbered — restore clean zeroes for next transition call */
        memset(scratch_sig, 0, w * sizeof(*scratch_sig));
      }

      const size_t r_start = (right_starts[rsig] != UINT32_MAX) ? right_starts[rsig] : 0;
      const size_t r_end   = (right_starts[rsig] != UINT32_MAX) ? right_ends[rsig]   : 0;

      for (size_t li = l_start; li < l_end; li++) {
        const uint32_t l_res = left->entries[li].residue;
        const uint64_t l_cnt = left->entries[li].count;
        for (size_t rj = r_start; rj < r_end; rj++) {
          const uint64_t rsum = (uint64_t)l_res + right->entries[rj].residue + shift;
          const uint32_t res  = r_pow2 ? (uint32_t)(rsum & r_mask) : (uint32_t)(rsum % r);
          uint64_t product = 0;
          if (!qsop_count_mul(l_cnt, right->entries[rj].count, &product, error) ||
              !table_add_entry(out, psig, res, product, error)) {
            free(left_starts); free(left_ends); free(right_starts); free(right_ends);
            return false;
          }
        }
      }
    }
  }

  free(left_starts); free(left_ends); free(right_starts); free(right_ends);
  if (candidate_pairs_out != NULL) *candidate_pairs_out += candidate_pairs;
  if (emitted_pairs_out   != NULL) *emitted_pairs_out   += emitted_pairs;
  return true;
}

static bool build_join_map(const qsop_instance_t *qsop,
                           const qsop_rankwidth_decomposition_t *decomposition, uint32_t node_id,
                           const uint64_t *adj, rw_signature_pool_t *pool,
                           const rw_table_t *left, const rw_table_t *right, rw_join_map_t *map,
                           qsop_error_t *error) {
  const uint32_t sign = qsop->r / 2U;
  if (left->reps_len > 0 && right->reps_len > SIZE_MAX / left->reps_len) {
    set_error(error, "rankwidth join map is too large");
    return false;
  }
  const size_t words = decomposition->words;
  if (!reserve_join_map(map, left->reps_len * right->reps_len, words, error)) {
    return false;
  }
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  uint64_t *signature = calloc(words == 0 ? 1U : words, sizeof(*signature));
  if (outside == NULL || signature == NULL) {
    free(outside);
    free(signature);
    set_error(error, "out of memory while building rankwidth join map");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(outside, v);
  }
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->reps_len; i++) {
    for (size_t j = 0; j < right->reps_len; j++) {
      const uint64_t *left_rep = table_assignment(left, i, words);
      const uint64_t *right_rep = table_assignment(right, j, words);
      const uint32_t parity =
          cross_parity_bitsets(qsop->nvars, adj, left_rep, right_rep, words);
      qsop_bitset_copy(signature, signature_bits(pool, left->reps[i].signature), words);
      qsop_bitset_xor(signature, signature_bits(pool, right->reps[j].signature), words);
      qsop_bitset_and(signature, outside, words);
      uint32_t parent_signature = 0;
      if (!signature_pool_intern(pool, signature, &parent_signature, error)) {
        free(outside);
        free(signature);
        return false;
      }

      const size_t index = map->len++;
      uint64_t *assignment = join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or(assignment, right_rep, words);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->reps[i].signature,
          .right_signature = right->reps[j].signature,
          .parent_signature = parent_signature,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  free(outside);
  free(signature);
  return join_map_build_sorted_idx(map, error);
}

/* scratch must be 3 * max(1, words) uint64_t words:
 *   [0 .. words-1]      outside bitset  (fully reinitialized on entry, no pre-zero needed)
 *   [words .. 2w-1]     signature        (overwritten on entry, no init needed)
 * map must be pre-allocated (len reset to 0 by caller before each call).           */
static bool build_join_map_arena(const qsop_instance_t *qsop,
                                  const qsop_rankwidth_decomposition_t *decomposition,
                                  uint32_t node_id, const uint64_t *adj,
                                  rw_signature_pool_t *pool, const rw_table_t *left,
                                  const rw_table_t *right, rw_join_map_t *map,
                                  uint64_t *scratch, qsop_error_t *error) {
  const uint32_t sign = qsop->r / 2U;
  if (left->reps_len > 0 && right->reps_len > SIZE_MAX / left->reps_len) {
    set_error(error, "rankwidth join map is too large");
    return false;
  }
  const size_t words = decomposition->words;
  const size_t w = words == 0 ? 1U : words;
  if (!reserve_join_map(map, left->reps_len * right->reps_len, words, error)) {
    return false;
  }
  uint64_t *outside = scratch;
  uint64_t *signature = scratch + w;
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(outside, v);
  }
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->reps_len; i++) {
    for (size_t j = 0; j < right->reps_len; j++) {
      const uint64_t *left_rep = table_assignment(left, i, words);
      const uint64_t *right_rep = table_assignment(right, j, words);
      const uint32_t parity =
          cross_parity_bitsets(qsop->nvars, adj, left_rep, right_rep, words);
      qsop_bitset_copy(signature, signature_bits(pool, left->reps[i].signature), words);
      qsop_bitset_xor(signature, signature_bits(pool, right->reps[j].signature), words);
      qsop_bitset_and(signature, outside, words);
      uint32_t parent_signature = 0;
      if (!signature_pool_intern(pool, signature, &parent_signature, error)) {
        return false;
      }

      const size_t index = map->len++;
      uint64_t *assignment = join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or(assignment, right_rep, words);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->reps[i].signature,
          .right_signature = right->reps[j].signature,
          .parent_signature = parent_signature,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  return true;
}

static const rw_join_map_entry_t *join_map_get(const rw_join_map_t *map, uint32_t left_signature,
                                               uint32_t right_signature, size_t *index_out) {
  if (map->sorted_keys != NULL) {
    const uint64_t target = ((uint64_t)left_signature << 32) | right_signature;
    size_t lo = 0, hi = map->len;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (map->sorted_keys[mid] < target) {
        lo = mid + 1;
      } else if (map->sorted_keys[mid] > target) {
        hi = mid;
      } else {
        if (index_out != NULL) {
          *index_out = map->sorted_idx[mid];
        }
        return &map->entries[map->sorted_idx[mid]];
      }
    }
    return NULL;
  }
  for (size_t i = 0; i < map->len; i++) {
    if (map->entries[i].left_signature == left_signature &&
        map->entries[i].right_signature == right_signature) {
      if (index_out != NULL) {
        *index_out = i;
      }
      return &map->entries[i];
    }
  }
  return NULL;
}

/* scratch must be 3 * max(1, words) uint64_t words:
 *   [0 .. words-1]      zero bitset  (must be all-zeros on entry, never written)
 *   [words .. 2w-1]     assignment   (must be all-zeros on entry, modified)
 *   [2w .. 3w-1]        signature    (overwritten on entry, no init needed)      */
static bool solve_leaf_arena(const qsop_instance_t *qsop, const uint64_t *adj,
                              const rw_node_t *node, size_t words, rw_signature_pool_t *pool,
                              rw_table_t *table, uint64_t *scratch, qsop_error_t *error) {
  const size_t w = words == 0 ? 1U : words;
  uint64_t *zero = scratch;
  uint64_t *assignment = scratch + w;
  uint64_t *signature = scratch + 2U * w;

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature, node->var);
  qsop_bitset_set(assignment, node->var);
  return signature_pool_intern(pool, zero, &zero_signature, error) &&
         signature_pool_intern(pool, signature, &one_signature, error) &&
         table_add_rep(table, zero_signature, zero, words, error) &&
         table_add_entry(table, zero_signature, 0, 1, error) &&
         table_add_rep(table, one_signature, assignment, words, error) &&
         table_add_entry(table, one_signature, qsop->unary[node->var] % qsop->r, 1, error);
}

static bool solve_leaf_mod(const qsop_instance_t *qsop, const uint64_t *adj, const rw_node_t *node,
                           size_t words, rw_signature_pool_t *pool, uint64_t modulus,
                           rw_table_t *table, qsop_error_t *error) {
  uint64_t *zero = calloc(words == 0 ? 1U : words, sizeof(*zero));
  uint64_t *assignment = calloc(words == 0 ? 1U : words, sizeof(*assignment));
  uint64_t *signature = calloc(words == 0 ? 1U : words, sizeof(*signature));
  if (zero == NULL || assignment == NULL || signature == NULL) {
    free(zero);
    free(assignment);
    free(signature);
    set_error(error, "out of memory while solving modular rankwidth leaf");
    return false;
  }

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature, node->var);
  qsop_bitset_set(assignment, node->var);
  const bool ok =
      signature_pool_intern(pool, zero, &zero_signature, error) &&
      signature_pool_intern(pool, signature, &one_signature, error) &&
      table_add_rep(table, zero_signature, zero, words, error) &&
      table_add_entry_mod(table, zero_signature, 0, 1, modulus, error) &&
      table_add_rep(table, one_signature, assignment, words, error) &&
      table_add_entry_mod(table, one_signature, qsop->unary[node->var] % qsop->r, 1, modulus,
                          error);
  free(zero);
  free(assignment);
  free(signature);
  return ok;
}

static bool solve_join_mod(const qsop_instance_t *qsop, const rw_join_map_t *map,
                           const rw_table_t *left, const rw_table_t *right, uint64_t modulus,
                           rw_table_t *out, size_t words, uint64_t *join_pairs,
                           qsop_error_t *error) {
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      size_t map_index = 0;
      const rw_join_map_entry_t *mapped =
          join_map_get(map, left->entries[i].signature, right->entries[j].signature, &map_index);
      if (mapped == NULL) {
        set_error(error, "internal error: missing modular rankwidth join-map entry");
        return false;
      }
      const uint32_t residue =
          (uint32_t)(((uint64_t)left->entries[i].residue + right->entries[j].residue +
                      mapped->residue_shift) %
                     qsop->r);
      const uint64_t product =
          qsop_mod_mul_u64(left->entries[i].count, right->entries[j].count, modulus);
      if (!table_add_rep(out, mapped->parent_signature,
                         join_map_assignment(map, map_index, words), words, error) ||
          !table_add_entry_mod(out, mapped->parent_signature, residue, product, modulus, error)) {
        return false;
      }
      (*join_pairs)++;
    }
  }
  return true;
}

/* Accumulator-based join: avoids the O(out.len) linear scan in table_add_entry by
 * accumulating residue counts per signature into a direct-indexed array, then
 * flushing once per (signature, residue) pair. Reduces table-operation cost from
 * O(left_len * right_len * out_len) to O(map.len * r + n_parent_sigs * r).       */
static bool solve_join_acc(const qsop_instance_t *qsop, const rw_join_map_t *map,
                               const rw_table_t *left, const rw_table_t *right, rw_table_t *out,
                               size_t words, uint64_t *join_pairs,
                               rw_join_workspace_t *ws, qsop_error_t *error) {
  if (map->len == 0) {
    return true;
  }

  uint32_t max_parent_sig = 0, max_left_sig = 0, max_right_sig = 0;
  for (size_t m = 0; m < map->len; m++) {
    const rw_join_map_entry_t *me = &map->entries[m];
    if (me->parent_signature > max_parent_sig) max_parent_sig = me->parent_signature;
    if (me->left_signature   > max_left_sig)   max_left_sig   = me->left_signature;
    if (me->right_signature  > max_right_sig)  max_right_sig  = me->right_signature;
  }
  const size_t n_sigs = (size_t)max_parent_sig + 1U;
  const uint32_t r = qsop->r;
  const uint32_t r_mask = r - 1U;
  const bool r_pow2 = (r & r_mask) == 0;

  /* Use pre-allocated workspace buffers when available and large enough. */
  const bool use_ws = ws != NULL && n_sigs <= ws->cap_sigs &&
                      (size_t)max_left_sig  < ws->cap_sigs &&
                      (size_t)max_right_sig < ws->cap_sigs;
  uint64_t *acc;
  uint32_t *sig_map_idx;
  uint32_t *left_starts, *left_ends;
  uint32_t *right_starts, *right_ends;
  if (use_ws) {
    acc          = ws->acc;
    sig_map_idx  = ws->sig_map_idx;
    left_starts  = ws->left_starts;
    left_ends    = ws->left_ends;
    right_starts = ws->right_starts;
    right_ends   = ws->right_ends;
    memset(acc, 0, n_sigs * r * sizeof(*acc));
    memset(sig_map_idx, 0xFF, n_sigs * sizeof(*sig_map_idx));
    build_sig_range_index_into(left,  max_left_sig,  left_starts,  left_ends);
    build_sig_range_index_into(right, max_right_sig, right_starts, right_ends);
  } else {
    acc = calloc(n_sigs * r, sizeof(*acc));
    sig_map_idx = malloc(n_sigs * sizeof(*sig_map_idx));
    left_starts = NULL; left_ends = NULL;
    right_starts = NULL; right_ends = NULL;
    if (acc == NULL || sig_map_idx == NULL ||
        !build_sig_range_index(left,  max_left_sig,  &left_starts,  &left_ends,  error) ||
        !build_sig_range_index(right, max_right_sig, &right_starts, &right_ends, error)) {
      if (!use_ws) { free(acc); free(sig_map_idx); free(left_starts); free(left_ends);
                     free(right_starts); free(right_ends); }
      if (acc == NULL || sig_map_idx == NULL) {
        set_error(error, "out of memory while allocating rankwidth join accumulator");
      }
      return false;
    }
    memset(sig_map_idx, 0xFF, n_sigs * sizeof(*sig_map_idx));
  }

#define JOIN_CLEANUP() do { if (!use_ws) { free(acc); free(sig_map_idx); \
    free(left_starts); free(left_ends); free(right_starts); free(right_ends); } } while (0)

  for (size_t m = 0; m < map->len; m++) {
    const rw_join_map_entry_t *me = &map->entries[m];
    const uint32_t lsig  = me->left_signature;
    const uint32_t rsig  = me->right_signature;
    const uint32_t ps    = me->parent_signature;
    const uint32_t shift = me->residue_shift;
    size_t l_start = 0, l_end = 0;
    if (left_starts[lsig] != UINT32_MAX) {
      l_start = left_starts[lsig];
      l_end   = left_ends[lsig];
    }
    size_t r_start = 0, r_end = 0;
    if (right_starts[rsig] != UINT32_MAX) {
      r_start = right_starts[rsig];
      r_end   = right_ends[rsig];
    }
    if (sig_map_idx[ps] == UINT32_MAX) {
      sig_map_idx[ps] = (uint32_t)m;
    }
    for (size_t i = l_start; i < l_end; i++) {
      const uint32_t l_res = left->entries[i].residue;
      const uint64_t l_cnt = left->entries[i].count;
      for (size_t j = r_start; j < r_end; j++) {
        const uint64_t rsum = (uint64_t)l_res + right->entries[j].residue + shift;
        const uint32_t res = r_pow2 ? (uint32_t)(rsum & r_mask) : (uint32_t)(rsum % r);
        uint64_t product = 0;
        if (!qsop_count_mul(l_cnt, right->entries[j].count, &product, error) ||
            !qsop_count_add(&acc[ps * r + res], product, error)) {
          JOIN_CLEANUP();
          return false;
        }
        (*join_pairs)++;
      }
    }
  }

  /* Flush accumulator: pre-reserve output capacity once to avoid per-entry realloc. */
  size_t nonzero_count = 0;
  for (size_t i = 0; i < n_sigs * r; i++) {
    if (acc[i] != 0) nonzero_count++;
  }
  if (!reserve_entries(out, out->len + nonzero_count, error)) {
    JOIN_CLEANUP();
    return false;
  }
  for (uint32_t s = 0; s < (uint32_t)n_sigs; s++) {
    if (sig_map_idx[s] == UINT32_MAX) {
      continue;
    }
    const size_t m = sig_map_idx[s];
    if (!table_add_rep(out, s, join_map_assignment(map, m, words), words, error)) {
      JOIN_CLEANUP();
      return false;
    }
    for (uint32_t res = 0; res < r; res++) {
      const uint64_t cnt = acc[(size_t)s * r + res];
      if (cnt == 0) {
        continue;
      }
      out->entries[out->len++] = (rw_entry_t){ .signature = s, .residue = res, .count = cnt };
    }
  }

#undef JOIN_CLEANUP
  if (!use_ws) {
    free(acc); free(sig_map_idx);
    free(left_starts); free(left_ends);
    free(right_starts); free(right_ends);
  }
  return true;
}

/* Modular accumulator join for the labelled CRT path (counts are reduced mod modulus). */
static bool solve_join_acc_mod(const qsop_instance_t *qsop, const rw_join_map_t *map,
                                   const rw_table_t *left, const rw_table_t *right,
                                   uint64_t modulus, rw_table_t *out,
                                   size_t words, uint64_t *join_pairs, qsop_error_t *error) {
  if (map->len == 0) {
    return true;
  }

  uint32_t max_parent_sig = 0, max_left_sig = 0, max_right_sig = 0;
  for (size_t m = 0; m < map->len; m++) {
    const rw_join_map_entry_t *me = &map->entries[m];
    if (me->parent_signature > max_parent_sig) max_parent_sig = me->parent_signature;
    if (me->left_signature   > max_left_sig)   max_left_sig   = me->left_signature;
    if (me->right_signature  > max_right_sig)  max_right_sig  = me->right_signature;
  }
  const size_t n_sigs = (size_t)max_parent_sig + 1U;
  const uint32_t r = qsop->r;

  uint64_t *acc = calloc(n_sigs * r, sizeof(*acc));
  uint32_t *sig_map_idx = malloc(n_sigs * sizeof(*sig_map_idx));
  uint32_t *left_starts = NULL, *left_ends = NULL;
  uint32_t *right_starts = NULL, *right_ends = NULL;
  if (acc == NULL || sig_map_idx == NULL ||
      !build_sig_range_index(left,  max_left_sig,  &left_starts,  &left_ends,  error) ||
      !build_sig_range_index(right, max_right_sig, &right_starts, &right_ends, error)) {
    free(acc); free(sig_map_idx);
    free(left_starts); free(left_ends);
    free(right_starts); free(right_ends);
    if (acc == NULL || sig_map_idx == NULL) {
      set_error(error, "out of memory while allocating rankwidth labelled join accumulator");
    }
    return false;
  }
  memset(sig_map_idx, 0xFF, n_sigs * sizeof(*sig_map_idx));

  for (size_t m = 0; m < map->len; m++) {
    const rw_join_map_entry_t *me = &map->entries[m];
    const uint32_t lsig  = me->left_signature;
    const uint32_t rsig  = me->right_signature;
    const uint32_t ps    = me->parent_signature;
    size_t l_start = 0, l_end = 0;
    if (left_starts[lsig] != UINT32_MAX) {
      l_start = left_starts[lsig];
      l_end   = left_ends[lsig];
    }
    size_t r_start = 0, r_end = 0;
    if (right_starts[rsig] != UINT32_MAX) {
      r_start = right_starts[rsig];
      r_end   = right_ends[rsig];
    }
    if (sig_map_idx[ps] == UINT32_MAX) {
      sig_map_idx[ps] = (uint32_t)m;
    }
    for (size_t i = l_start; i < l_end; i++) {
      const uint32_t l_res = left->entries[i].residue;
      const uint64_t l_cnt = left->entries[i].count;
      for (size_t j = r_start; j < r_end; j++) {
        const uint32_t res =
            (uint32_t)(((uint64_t)l_res + right->entries[j].residue + me->residue_shift) % r);
        const uint64_t product = qsop_mod_mul_u64(l_cnt, right->entries[j].count, modulus);
        acc[(size_t)ps * r + res] = qsop_mod_add_u64(acc[(size_t)ps * r + res], product, modulus);
        (*join_pairs)++;
      }
    }
  }

  size_t nonzero_count = 0;
  for (size_t i = 0; i < n_sigs * r; i++) {
    if (acc[i] != 0) nonzero_count++;
  }
  if (!reserve_entries(out, out->len + nonzero_count, error)) {
    free(acc); free(sig_map_idx);
    free(left_starts); free(left_ends);
    free(right_starts); free(right_ends);
    return false;
  }
  for (uint32_t s = 0; s < (uint32_t)n_sigs; s++) {
    if (sig_map_idx[s] == UINT32_MAX) {
      continue;
    }
    const size_t m = sig_map_idx[s];
    if (!table_add_rep(out, s, join_map_assignment(map, m, words), words, error)) {
      free(acc); free(sig_map_idx);
      free(left_starts); free(left_ends);
      free(right_starts); free(right_ends);
      return false;
    }
    for (uint32_t res = 0; res < r; res++) {
      const uint64_t cnt = acc[(size_t)s * r + res];
      if (cnt == 0) {
        continue;
      }
      out->entries[out->len++] = (rw_entry_t){ .signature = s, .residue = res, .count = cnt };
    }
  }

  free(acc); free(sig_map_idx);
  free(left_starts); free(left_ends);
  free(right_starts); free(right_ends);
  return true;
}

static bool build_labelled_join_map(const qsop_instance_t *qsop,
                                    const qsop_rankwidth_decomposition_t *decomposition,
                                    uint32_t node_id, const uint32_t *coeffs,
                                    rw_label_signature_pool_t *pool, const rw_table_t *left,
                                    const rw_table_t *right, rw_join_map_t *map,
                                    qsop_error_t *error) {
  if (left->reps_len > 0 && right->reps_len > SIZE_MAX / left->reps_len) {
    set_error(error, "labelled rankwidth join map is too large");
    return false;
  }
  const size_t words = decomposition->words;
  if (!reserve_join_map(map, left->reps_len * right->reps_len, words, error)) {
    return false;
  }
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  uint32_t *signature = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*signature));
  if (outside == NULL || signature == NULL) {
    free(outside);
    free(signature);
    set_error(error, "out of memory while building labelled rankwidth join map");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(outside, v);
  }
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->reps_len; i++) {
    for (size_t j = 0; j < right->reps_len; j++) {
      const uint64_t *left_rep = table_assignment(left, i, words);
      const uint64_t *right_rep = table_assignment(right, j, words);
      const uint32_t residue_shift =
          cross_labelled_residue(qsop, coeffs, left_rep, right_rep);
      labelled_parent_signature(qsop, label_signature_coeffs_const(pool, left->reps[i].signature),
                                label_signature_coeffs_const(pool, right->reps[j].signature),
                                outside, signature);
      uint32_t parent_signature = 0;
      if (!label_signature_pool_intern(pool, signature, &parent_signature, error)) {
        free(outside);
        free(signature);
        return false;
      }

      const size_t index = map->len++;
      uint64_t *assignment = join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or(assignment, right_rep, words);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->reps[i].signature,
          .right_signature = right->reps[j].signature,
          .parent_signature = parent_signature,
          .residue_shift = residue_shift,
      };
    }
  }
  free(outside);
  free(signature);
  return join_map_build_sorted_idx(map, error);
}

static bool solve_labelled_leaf(const qsop_instance_t *qsop, const uint32_t *coeffs,
                                const rw_node_t *node, size_t words,
                                rw_label_signature_pool_t *pool, rw_table_t *table,
                                qsop_error_t *error) {
  uint64_t *zero = calloc(words == 0 ? 1U : words, sizeof(*zero));
  uint64_t *assignment = calloc(words == 0 ? 1U : words, sizeof(*assignment));
  uint32_t *zero_signature_coeffs = calloc(qsop->nvars == 0 ? 1U : qsop->nvars,
                                           sizeof(*zero_signature_coeffs));
  uint32_t *one_signature_coeffs = calloc(qsop->nvars == 0 ? 1U : qsop->nvars,
                                          sizeof(*one_signature_coeffs));
  if (zero == NULL || assignment == NULL || zero_signature_coeffs == NULL ||
      one_signature_coeffs == NULL) {
    free(zero);
    free(assignment);
    free(zero_signature_coeffs);
    free(one_signature_coeffs);
    set_error(error, "out of memory while solving labelled rankwidth leaf");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    one_signature_coeffs[v] = coeffs[(size_t)node->var * qsop->nvars + v] % qsop->r;
  }
  one_signature_coeffs[node->var] = 0;
  qsop_bitset_set(assignment, node->var);

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  const bool ok =
      label_signature_pool_intern(pool, zero_signature_coeffs, &zero_signature, error) &&
      label_signature_pool_intern(pool, one_signature_coeffs, &one_signature, error) &&
      table_add_rep(table, zero_signature, zero, words, error) &&
      table_add_entry(table, zero_signature, 0, 1, error) &&
      table_add_rep(table, one_signature, assignment, words, error) &&
      table_add_entry(table, one_signature, qsop->unary[node->var] % qsop->r, 1, error);
  free(zero);
  free(assignment);
  free(zero_signature_coeffs);
  free(one_signature_coeffs);
  return ok;
}

static bool solve_labelled_leaf_mod(const qsop_instance_t *qsop, const uint32_t *coeffs,
                                    const rw_node_t *node, size_t words,
                                    rw_label_signature_pool_t *pool, uint64_t modulus,
                                    rw_table_t *table, qsop_error_t *error) {
  uint64_t *zero = calloc(words == 0 ? 1U : words, sizeof(*zero));
  uint64_t *assignment = calloc(words == 0 ? 1U : words, sizeof(*assignment));
  uint32_t *zero_signature_coeffs = calloc(qsop->nvars == 0 ? 1U : qsop->nvars,
                                           sizeof(*zero_signature_coeffs));
  uint32_t *one_signature_coeffs = calloc(qsop->nvars == 0 ? 1U : qsop->nvars,
                                          sizeof(*one_signature_coeffs));
  if (zero == NULL || assignment == NULL || zero_signature_coeffs == NULL ||
      one_signature_coeffs == NULL) {
    free(zero);
    free(assignment);
    free(zero_signature_coeffs);
    free(one_signature_coeffs);
    set_error(error, "out of memory while solving modular labelled rankwidth leaf");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    one_signature_coeffs[v] = coeffs[(size_t)node->var * qsop->nvars + v] % qsop->r;
  }
  one_signature_coeffs[node->var] = 0;
  qsop_bitset_set(assignment, node->var);

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  const bool ok =
      label_signature_pool_intern(pool, zero_signature_coeffs, &zero_signature, error) &&
      label_signature_pool_intern(pool, one_signature_coeffs, &one_signature, error) &&
      table_add_rep(table, zero_signature, zero, words, error) &&
      table_add_entry_mod(table, zero_signature, 0, 1, modulus, error) &&
      table_add_rep(table, one_signature, assignment, words, error) &&
      table_add_entry_mod(table, one_signature, qsop->unary[node->var] % qsop->r, 1, modulus,
                          error);
  free(zero);
  free(assignment);
  free(zero_signature_coeffs);
  free(one_signature_coeffs);
  return ok;
}

static bool solve_labelled_join_mod(const qsop_instance_t *qsop, const rw_join_map_t *map,
                                    const rw_table_t *left, const rw_table_t *right,
                                    uint64_t modulus, rw_table_t *out, size_t words,
                                    uint64_t *join_pairs, qsop_error_t *error) {
  return solve_join_mod(qsop, map, left, right, modulus, out, words, join_pairs, error);
}

static bool fourier_table_signature_index(rw_fourier_table_t *table, uint32_t signature,
                                          const uint64_t *assignment, uint32_t r, size_t words,
                                          size_t *out, qsop_error_t *error) {
  for (size_t i = 0; i < table->len; i++) {
    if (table->signatures[i] == signature) {
      *out = i;
      return true;
    }
  }
  if (!reserve_fourier_table(table, table->len + 1U, r, words, error)) {
    return false;
  }
  const size_t index = table->len++;
  table->signatures[index] = signature;
  qsop_bitset_copy(fourier_assignment(table, index, words), assignment, words);
  memset(&table->values[index * (size_t)r], 0, (size_t)r * sizeof(*table->values));
  *out = index;
  return true;
}

static bool solve_fourier_leaf(const qsop_instance_t *qsop, const uint64_t *adj,
                               const rw_node_t *node, const uint64_t *powers,
                               uint64_t prime, size_t words, rw_signature_pool_t *pool,
                               rw_fourier_table_t *table, qsop_error_t *error) {
  uint64_t *zero_bits = calloc(words == 0 ? 1U : words, sizeof(*zero_bits));
  uint64_t *one_bits = calloc(words == 0 ? 1U : words, sizeof(*one_bits));
  uint64_t *signature_bits_buffer = calloc(words == 0 ? 1U : words, sizeof(*signature_bits_buffer));
  if (zero_bits == NULL || one_bits == NULL || signature_bits_buffer == NULL) {
    free(zero_bits);
    free(one_bits);
    free(signature_bits_buffer);
    set_error(error, "out of memory while solving rankwidth Fourier leaf");
    return false;
  }

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  qsop_bitset_copy(signature_bits_buffer, qsop_bitset_const_row(adj, words, node->var), words);
  qsop_bitset_clear(signature_bits_buffer, node->var);
  qsop_bitset_set(one_bits, node->var);
  size_t zero = 0;
  size_t one = 0;
  if (!signature_pool_intern(pool, zero_bits, &zero_signature, error) ||
      !signature_pool_intern(pool, signature_bits_buffer, &one_signature, error) ||
      !fourier_table_signature_index(table, zero_signature, zero_bits, qsop->r, words, &zero,
                                     error) ||
      !fourier_table_signature_index(table, one_signature, one_bits, qsop->r, words, &one,
                                     error)) {
    free(zero_bits);
    free(one_bits);
    free(signature_bits_buffer);
    return false;
  }
  for (uint32_t mode = 0; mode < qsop->r; mode++) {
    table->values[zero * (size_t)qsop->r + mode] =
        qsop_mod_add_u64(table->values[zero * (size_t)qsop->r + mode], 1, prime);
    table->values[one * (size_t)qsop->r + mode] = qsop_mod_add_u64(
        table->values[one * (size_t)qsop->r + mode],
        powers[(size_t)mode * qsop->r + (qsop->unary[node->var] % qsop->r)], prime);
  }
  free(zero_bits);
  free(one_bits);
  free(signature_bits_buffer);
  return true;
}

static bool solve_labelled_fourier_leaf(const qsop_instance_t *qsop, const uint32_t *coeffs,
                                        const rw_node_t *node, const uint64_t *powers,
                                        uint64_t prime, size_t words,
                                        rw_label_signature_pool_t *pool,
                                        rw_fourier_table_t *table, qsop_error_t *error) {
  uint64_t *zero_bits = calloc(words == 0 ? 1U : words, sizeof(*zero_bits));
  uint64_t *one_bits = calloc(words == 0 ? 1U : words, sizeof(*one_bits));
  uint32_t *zero_signature_coeffs = calloc(qsop->nvars == 0 ? 1U : qsop->nvars,
                                           sizeof(*zero_signature_coeffs));
  uint32_t *one_signature_coeffs = calloc(qsop->nvars == 0 ? 1U : qsop->nvars,
                                          sizeof(*one_signature_coeffs));
  if (zero_bits == NULL || one_bits == NULL || zero_signature_coeffs == NULL ||
      one_signature_coeffs == NULL) {
    free(zero_bits);
    free(one_bits);
    free(zero_signature_coeffs);
    free(one_signature_coeffs);
    set_error(error, "out of memory while solving labelled rankwidth Fourier leaf");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    one_signature_coeffs[v] = coeffs[(size_t)node->var * qsop->nvars + v] % qsop->r;
  }
  one_signature_coeffs[node->var] = 0;
  qsop_bitset_set(one_bits, node->var);

  uint32_t zero_signature = 0;
  uint32_t one_signature = 0;
  size_t zero = 0;
  size_t one = 0;
  if (!label_signature_pool_intern(pool, zero_signature_coeffs, &zero_signature, error) ||
      !label_signature_pool_intern(pool, one_signature_coeffs, &one_signature, error) ||
      !fourier_table_signature_index(table, zero_signature, zero_bits, qsop->r, words, &zero,
                                     error) ||
      !fourier_table_signature_index(table, one_signature, one_bits, qsop->r, words, &one,
                                     error)) {
    free(zero_bits);
    free(one_bits);
    free(zero_signature_coeffs);
    free(one_signature_coeffs);
    return false;
  }
  for (uint32_t mode = 0; mode < qsop->r; mode++) {
    table->values[zero * (size_t)qsop->r + mode] =
        qsop_mod_add_u64(table->values[zero * (size_t)qsop->r + mode], 1, prime);
    table->values[one * (size_t)qsop->r + mode] = qsop_mod_add_u64(
        table->values[one * (size_t)qsop->r + mode],
        powers[(size_t)mode * qsop->r + (qsop->unary[node->var] % qsop->r)], prime);
  }
  free(zero_bits);
  free(one_bits);
  free(zero_signature_coeffs);
  free(one_signature_coeffs);
  return true;
}

static bool build_fourier_join_map(const qsop_instance_t *qsop,
                                   const qsop_rankwidth_decomposition_t *decomposition,
                                   uint32_t node_id, const uint64_t *adj,
                                   rw_signature_pool_t *pool, const rw_fourier_table_t *left,
                                   const rw_fourier_table_t *right, rw_join_map_t *map,
                                   qsop_error_t *error) {
  const uint32_t sign = qsop->r / 2U;
  if (left->len > 0 && right->len > SIZE_MAX / left->len) {
    set_error(error, "rankwidth Fourier join map is too large");
    return false;
  }
  const size_t words = decomposition->words;
  if (!reserve_join_map(map, left->len * right->len, words, error)) {
    return false;
  }
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  uint64_t *signature = calloc(words == 0 ? 1U : words, sizeof(*signature));
  if (outside == NULL || signature == NULL) {
    free(outside);
    free(signature);
    set_error(error, "out of memory while building rankwidth Fourier join map");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(outside, v);
  }
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      const uint64_t *left_rep = fourier_assignment(left, i, words);
      const uint64_t *right_rep = fourier_assignment(right, j, words);
      const uint32_t parity =
          cross_parity_bitsets(qsop->nvars, adj, left_rep, right_rep, words);
      qsop_bitset_copy(signature, signature_bits(pool, left->signatures[i]), words);
      qsop_bitset_xor(signature, signature_bits(pool, right->signatures[j]), words);
      qsop_bitset_and(signature, outside, words);
      uint32_t parent_signature = 0;
      if (!signature_pool_intern(pool, signature, &parent_signature, error)) {
        free(outside);
        free(signature);
        return false;
      }
      const size_t index = map->len++;
      uint64_t *assignment = join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or(assignment, right_rep, words);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->signatures[i],
          .right_signature = right->signatures[j],
          .parent_signature = parent_signature,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  free(outside);
  free(signature);
  return true;
}

static bool build_labelled_fourier_join_map(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t node_id, const uint32_t *coeffs, rw_label_signature_pool_t *pool,
    const rw_fourier_table_t *left, const rw_fourier_table_t *right, rw_join_map_t *map,
    qsop_error_t *error) {
  if (left->len > 0 && right->len > SIZE_MAX / left->len) {
    set_error(error, "labelled rankwidth Fourier join map is too large");
    return false;
  }
  const size_t words = decomposition->words;
  if (!reserve_join_map(map, left->len * right->len, words, error)) {
    return false;
  }
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  uint32_t *signature = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*signature));
  if (outside == NULL || signature == NULL) {
    free(outside);
    free(signature);
    set_error(error, "out of memory while building labelled rankwidth Fourier join map");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(outside, v);
  }
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), words);

  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      const uint64_t *left_rep = fourier_assignment(left, i, words);
      const uint64_t *right_rep = fourier_assignment(right, j, words);
      const uint32_t residue_shift =
          cross_labelled_residue(qsop, coeffs, left_rep, right_rep);
      labelled_parent_signature(qsop, label_signature_coeffs_const(pool, left->signatures[i]),
                                label_signature_coeffs_const(pool, right->signatures[j]), outside,
                                signature);
      uint32_t parent_signature = 0;
      if (!label_signature_pool_intern(pool, signature, &parent_signature, error)) {
        free(outside);
        free(signature);
        return false;
      }

      const size_t index = map->len++;
      uint64_t *assignment = join_map_assignment(map, index, words);
      qsop_bitset_copy(assignment, left_rep, words);
      qsop_bitset_or(assignment, right_rep, words);
      map->entries[index] = (rw_join_map_entry_t){
          .left_signature = left->signatures[i],
          .right_signature = right->signatures[j],
          .parent_signature = parent_signature,
          .residue_shift = residue_shift,
      };
    }
  }
  free(outside);
  free(signature);
  return join_map_build_sorted_idx(map, error);
}

static bool solve_fourier_join(const qsop_instance_t *qsop, const rw_join_map_t *map,
                               const rw_fourier_table_t *left,
                               const rw_fourier_table_t *right, const uint64_t *powers,
                               uint64_t prime, rw_fourier_table_t *out,
                               size_t words, uint64_t *join_signature_pairs,
                               qsop_error_t *error) {
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      size_t map_index = 0;
      const rw_join_map_entry_t *mapped =
          join_map_get(map, left->signatures[i], right->signatures[j], &map_index);
      if (mapped == NULL) {
        set_error(error, "internal error: missing rankwidth Fourier join-map entry");
        return false;
      }
      size_t out_index = 0;
      if (!fourier_table_signature_index(out, mapped->parent_signature,
                                         join_map_assignment(map, map_index, words), qsop->r,
                                         words, &out_index, error)) {
        return false;
      }
      for (uint32_t mode = 0; mode < qsop->r; mode++) {
        const uint64_t left_value = left->values[i * (size_t)qsop->r + mode];
        const uint64_t right_value = right->values[j * (size_t)qsop->r + mode];
        uint64_t value = qsop_mod_mul_u64(left_value, right_value, prime);
        value = qsop_mod_mul_u64(value, powers[(size_t)mode * qsop->r + mapped->residue_shift], prime);
        out->values[out_index * (size_t)qsop->r + mode] =
            qsop_mod_add_u64(out->values[out_index * (size_t)qsop->r + mode], value, prime);
      }
      (*join_signature_pairs)++;
    }
  }
  return true;
}

static bool fourier_table_find_signature(const rw_fourier_table_t *table, uint32_t signature,
                                         size_t *out) {
  for (size_t i = 0; i < table->len; i++) {
    if (table->signatures[i] == signature) {
      *out = i;
      return true;
    }
  }
  return false;
}

static bool fourier_root_counts_to_result(const qsop_instance_t *qsop,
                                          const rw_fourier_table_t *root_table,
                                          const uint64_t *powers, const uint64_t *inv_powers,
                                          uint64_t prime, uint64_t *counts,
                                          qsop_error_t *error) {
  size_t root_index = 0;
  if (!fourier_table_find_signature(root_table, 0, &root_index)) {
    return true;
  }
  return qsop_fourier_inverse_counts(qsop->r, &root_table->values[root_index * (size_t)qsop->r],
                                     qsop->constant, powers, inv_powers, prime, counts, error);
}

static bool solve_rankwidth_count_table_mod_once(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, uint64_t modulus, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  rw_signature_pool_t pool = {0};
  if (tables == NULL || !signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    set_error(error, "out of memory while allocating modular rankwidth solve state");
    return false;
  }

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_mod(qsop, adj, node, decomposition->words, &pool, modulus, &tables[node_id],
                          error);
      qsop_trace_emit_elapsed(trace, "rankwidth.crt_leaf", 0, tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_join_map(qsop, decomposition, node_id, adj, &pool, &tables[node->left],
                          &tables[node->right], &map, error);
      if (ok) {
        join_signature_pairs += map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join_mod(qsop, &map, &tables[node->left], &tables[node->right], modulus,
                            &tables[node_id], decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join", 0, tables[node_id].len,
                                join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
  }

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  signature_pool_free(&pool);
  return true;
}

/* Sign-edge CRT: build transition cache on the first prime.
 * Runs solve_leaf_mod for leaves and build_join_map_arena + solve_join_acc_mod for joins.
 * Per-node maps are stored in maps[] and kept alive; caller frees them. */
static bool solve_sign_edge_crt_build_maps(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, uint64_t modulus, uint64_t *counts,
    rw_signature_pool_t *pool, rw_join_map_t *maps, uint64_t *scratch,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_table_t *tables = calloc(nnodes, sizeof(*tables));
  if (tables == NULL) {
    set_error(error, "out of memory in sign-edge CRT transition build");
    return false;
  }
  uint64_t join_pairs = 0, join_signature_pairs = 0;
  uint64_t table_entries = 0, signature_entries = 0;
  uint64_t max_table_entries = 0, max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_mod(qsop, adj, node, decomposition->words, pool, modulus,
                          &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.crt_leaf", 0, tables[node_id].len, start);
    } else {
      maps[node_id].len = 0;
      ok = build_join_map_arena(qsop, decomposition, node_id, adj, pool,
                                &tables[node->left], &tables[node->right],
                                &maps[node_id], scratch, error);
      if (ok) {
        join_signature_pairs += maps[node_id].len;
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join_map", 0, maps[node_id].len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join_acc_mod(qsop, &maps[node_id], &tables[node->left],
                                   &tables[node->right], modulus, &tables[node_id],
                                   decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.crt_join", 0, tables[node_id].len,
                                join_start);
      }
    }
    if (!ok) {
      for (uint32_t t = 0; t < nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
  }
  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }
  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  return true;
}

/* Sign-edge CRT: subsequent primes reuse maps cached by solve_sign_edge_crt_build_maps. */
static bool solve_sign_edge_crt_use_maps(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, uint64_t modulus, uint64_t *counts,
    rw_signature_pool_t *pool, const rw_join_map_t *maps,
    qsop_error_t *error) {
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_table_t *tables = calloc(nnodes, sizeof(*tables));
  if (tables == NULL) {
    set_error(error, "out of memory in sign-edge CRT cached pass");
    return false;
  }
  uint64_t join_pairs = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf_mod(qsop, adj, node, decomposition->words, pool, modulus,
                          &tables[node_id], error);
    } else {
      ok = solve_join_acc_mod(qsop, &maps[node_id], &tables[node->left], &tables[node->right],
                                 modulus, &tables[node_id], decomposition->words, &join_pairs,
                                 error);
    }
    if (!ok) {
      for (uint32_t t = 0; t < nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
  }
  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  return true;
}

/* Sign-edge CRT: full solve for nvars >= 64 using accumulator + per-node transition cache. */
static bool solve_rankwidth_count_table_crt(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "rankwidth CRT count table is too large");
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
    set_error(error, "out of memory for rankwidth CRT state");
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
    set_error(error, "out of memory for rankwidth CRT result strings");
    return false;
  }
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  rw_signature_pool_t pool = {0};
  rw_join_map_t *maps = calloc(nnodes, sizeof(*maps));
  uint64_t *scratch = calloc(3U * w, sizeof(*scratch));
  if (maps == NULL || scratch == NULL ||
      !signature_pool_init(&pool, decomposition->words, error)) {
    free(scratch);
    free(maps);
    if (pool.bits != NULL) {
      signature_pool_free(&pool);
    }
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory for rankwidth CRT transition cache");
    return false;
  }
  bool ok = solve_sign_edge_crt_build_maps(qsop, decomposition, adj, primes[0],
                                              &all_counts[0], &pool, maps, scratch,
                                              stats, trace, error);
  for (size_t p = 1; p < nprimes && ok; p++) {
    ok = solve_sign_edge_crt_use_maps(qsop, decomposition, adj, primes[p],
                                         &all_counts[p * (size_t)qsop->r], &pool, maps, error);
  }
  if (!ok) {
    for (uint32_t t = 0; t < nnodes; t++) {
      join_map_free(&maps[t]);
    }
    free(maps);
    free(scratch);
    signature_pool_free(&pool);
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    return false;
  }
  for (uint32_t residue = 0; residue < qsop->r; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)qsop->r + residue];
    }
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes,
                                      &result->count_strings[residue], error)) {
      for (uint32_t t = 0; t < nnodes; t++) {
        join_map_free(&maps[t]);
      }
      free(maps);
      free(scratch);
      signature_pool_free(&pool);
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    join_map_free(&maps[t]);
  }
  free(maps);
  free(scratch);
  signature_pool_free(&pool);
  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}

static bool solve_rankwidth_count_table(const qsop_instance_t *qsop,
                                           const qsop_rankwidth_decomposition_t *decomposition,
                                           const uint64_t *adj,
                                           qsop_rankwidth_join_strategy_t join_strategy,
                                           uint64_t materialize_join_max_pairs,
                                           qsop_result_t **out,
                                           qsop_solve_stats_t *stats,
                                           qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (qsop->nvars >= 64U) {
    return solve_rankwidth_count_table_crt(qsop, decomposition, adj, out, stats, trace, error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  rw_signature_pool_t pool = {0};
  if (!signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    qsop_result_free(result);
    return false;
  }

  /* Arena: 3 scratch bitsets (zero | assignment | sig_temp) shared across all nodes. */
  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  uint64_t *scratch = calloc(3U * w, sizeof(*scratch));
  if (scratch == NULL) {
    for (uint32_t t = 0; t < decomposition->nnodes; t++) table_free(&tables[t]);
    free(tables);
    signature_pool_free(&pool);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth scratch");
    return false;
  }

  /* Precompute per-node outside bitsets and scratch_sig buffer (reuse scratch[2w]). */
  uint64_t *scratch_sig = scratch + 2U * w; /* scratch[2w..3w-1]: sig temp for transitions */

  /* Pre-allocate join workspace for CSR path (amortizes malloc over all join nodes). */
  const size_t ws_cap_sigs = decomposition->score_cached && decomposition->cached_table_forecast > 0
      ? (size_t)(decomposition->cached_table_forecast / qsop->r) + 1U
      : 0U;
  rw_join_workspace_t join_ws = {0};
  if (ws_cap_sigs > 0 &&
      !join_workspace_alloc(ws_cap_sigs, qsop->r, &join_ws, error)) {
    free(scratch);
    for (uint32_t t = 0; t < decomposition->nnodes; t++) table_free(&tables[t]);
    free(tables);
    signature_pool_free(&pool);
    qsop_result_free(result);
    return false;
  }

  /* Outside bitset per join node (scratch[0..w-1] reused as temp; allocated per node). */
  uint64_t *outside = calloc(w, sizeof(*outside));
  if (outside == NULL) {
    join_workspace_free(&join_ws);
    free(scratch);
    for (uint32_t t = 0; t < decomposition->nnodes; t++) table_free(&tables[t]);
    free(tables);
    signature_pool_free(&pool);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth outside bitset");
    return false;
  }

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  uint64_t transition_bytes = 0;
  uint64_t u16_events = 0;
  uint64_t u32_events = 0;
  uint64_t materialized_join_events = 0;
  uint64_t streaming_join_events = 0;
  uint64_t streaming_candidate_pairs = 0;
  uint64_t streaming_emitted_pairs = 0;

  const uint64_t max_pairs = (materialize_join_max_pairs > 0)
      ? materialize_join_max_pairs : RW_MATERIALIZE_JOIN_MAX_PAIRS_DEFAULT;

  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      /* Zero scratch[0] (zero bitset) and scratch[w] (assignment) before each leaf. */
      memset(scratch, 0, 2U * w * sizeof(*scratch));
      ok = solve_leaf_arena(qsop, adj, node, decomposition->words, &pool, &tables[node_id],
                            scratch, error);
      qsop_trace_emit_elapsed(trace, "rankwidth.leaf", 0, tables[node_id].len, start);
    } else {
      /* Build outside bitset for this join node. */
      for (uint32_t v = 0; v < decomposition->nvars; v++) qsop_bitset_set(outside, v);
      qsop_bitset_and_not(outside, node_vars_const(decomposition, node_id), decomposition->words);

      /* Forecast pair count: left_reps × right_reps. */
      const size_t lreps = tables[node->left].reps_len;
      const size_t rreps = tables[node->right].reps_len;
      const uint64_t pair_forecast = (lreps > 0 && rreps > UINT64_MAX / lreps)
          ? UINT64_MAX : (uint64_t)lreps * rreps;

      /* Select strategy for this node. */
      const bool do_streaming = (join_strategy == QSOP_RANKWIDTH_JOIN_STREAMING) ||
          (join_strategy == QSOP_RANKWIDTH_JOIN_AUTO && pair_forecast > max_pairs);

      if (do_streaming) {
        streaming_join_events++;
        memset(scratch_sig, 0, w * sizeof(*scratch_sig));
        uint64_t cand = 0, emit = 0;
        ok = rw_join_count_table_streaming_sign(qsop, decomposition, node_id, adj, &pool,
                                                &tables[node->left], &tables[node->right],
                                                &tables[node_id], outside, scratch_sig,
                                                decomposition->words, &cand, &emit, error);
        streaming_candidate_pairs += cand;
        streaming_emitted_pairs   += emit;
        join_pairs += emit;
        join_signature_pairs += emit;
        qsop_trace_emit_elapsed(trace, "rankwidth.streaming_join", 0, tables[node_id].len, start);
      } else {
        /* D4.1: CSR materialized path. */
        rw_transition_csr_t csr = {0};
        memset(scratch_sig, 0, w * sizeof(*scratch_sig));
        ok = rw_transition_csr_build_sign(qsop, decomposition, node_id, adj, &pool,
                                          &tables[node->left], &tables[node->right],
                                          outside, scratch_sig, &csr,
                                          &u16_events, &u32_events, error);
        if (ok) {
          join_signature_pairs += csr.transition_count;
          transition_bytes += rw_transition_csr_bytes(&csr);
          materialized_join_events++;
          qsop_trace_emit_elapsed(trace, "rankwidth.join_map", 0, csr.transition_count, start);
          const uint64_t join_start = qsop_trace_begin(trace);
          memset(scratch_sig, 0, w * sizeof(*scratch_sig));
          ok = rw_execute_csr_join_sign(qsop, &csr, &tables[node->left], &tables[node->right],
                                        &tables[node_id], decomposition->words, &join_pairs,
                                        join_ws.acc ? &join_ws : NULL, error);
          qsop_trace_emit_elapsed(trace, "rankwidth.join", 0, tables[node_id].len, join_start);
        }
        rw_transition_csr_free(&csr);
      }
    }
    if (!ok) {
      join_workspace_free(&join_ws);
      free(scratch);
      free(outside);
      for (uint32_t t = 0; t < decomposition->nnodes; t++) table_free(&tables[t]);
      free(tables);
      signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
    table_sort(&tables[node_id]);
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) max_table_entries = tables[node_id].len;
    if (tables[node_id].reps_len > max_signature_entries) max_signature_entries = tables[node_id].reps_len;
  }

  join_workspace_free(&join_ws);
  free(scratch);
  free(outside);

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    if (!qsop_count_add(&result->counts[residue], root->entries[i].count, error)) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->rankwidth_transition_bytes           += transition_bytes;
    stats->rankwidth_transition_layout_u16_events += u16_events;
    stats->rankwidth_transition_layout_u32_events += u32_events;
    stats->rankwidth_materialized_join_events   += materialized_join_events;
    stats->rankwidth_streaming_join_events      += streaming_join_events;
    stats->rankwidth_streaming_join_candidate_pairs += streaming_candidate_pairs;
    stats->rankwidth_streaming_join_emitted_pairs   += streaming_emitted_pairs;
    stats->rankwidth_table_assignment_bytes =
        (uint64_t)signature_entries * decomposition->words * sizeof(uint64_t);
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  signature_pool_free(&pool);
  *out = result;
  return true;
}

static bool solve_labelled_count_table_mod_once(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, uint64_t modulus, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  rw_label_signature_pool_t pool = {0};
  if (tables == NULL || !label_signature_pool_init(&pool, qsop->nvars, error)) {
    free(tables);
    set_error(error, "out of memory while allocating modular labelled rankwidth solve state");
    return false;
  }

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_labelled_leaf_mod(qsop, coeffs, node, decomposition->words, &pool, modulus,
                                   &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.labelled_crt_leaf", 0, tables[node_id].len,
                              start);
    } else {
      rw_join_map_t map = {0};
      ok = build_labelled_join_map(qsop, decomposition, node_id, coeffs, &pool,
                                   &tables[node->left], &tables[node->right], &map, error);
      if (ok) {
        join_signature_pairs += map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_crt_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_labelled_join_mod(qsop, &map, &tables[node->left], &tables[node->right],
                                     modulus, &tables[node_id], decomposition->words,
                                     &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_crt_join", 0, tables[node_id].len,
                                join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      label_signature_pool_free(&pool);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
  }

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = ceil_log2_u64(max_signature_entries);
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  label_signature_pool_free(&pool);
  return true;
}

/* Labelled path: direct big-integer solve for nvars < 64 using shared join map +
 * accumulator join (eliminates O(out_len) linear scan from the hot multiply loop). */
static bool solve_labelled_count_table_direct(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating labelled rankwidth direct solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  rw_label_signature_pool_t pool = {0};
  if (!label_signature_pool_init(&pool, qsop->nvars, error)) {
    free(tables);
    qsop_result_free(result);
    return false;
  }

  const size_t w = decomposition->words == 0 ? 1U : decomposition->words;
  rw_join_map_t shared_map = {0};
  if (!reserve_join_map(&shared_map, RW_JOIN_MAP_INITIAL_CAP, w, error)) {
    free(tables);
    label_signature_pool_free(&pool);
    qsop_result_free(result);
    return false;
  }

  /* Pre-allocate join workspace to amortize malloc/free over all join nodes. */
  const size_t ws_cap_sigs2 = decomposition->score_cached && decomposition->cached_table_forecast > 0
      ? (size_t)(decomposition->cached_table_forecast / qsop->r) + 1U
      : 0U;
  rw_join_workspace_t join_ws = {0};
  if (ws_cap_sigs2 > 0 &&
      !join_workspace_alloc(ws_cap_sigs2, qsop->r, &join_ws, error)) {
    join_map_free(&shared_map);
    free(tables);
    label_signature_pool_free(&pool);
    qsop_result_free(result);
    return false;
  }

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_labelled_leaf(qsop, coeffs, node, decomposition->words, &pool,
                               &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.labelled_leaf", 0, tables[node_id].len, start);
    } else {
      shared_map.len = 0;
      ok = build_labelled_join_map(qsop, decomposition, node_id, coeffs, &pool,
                                   &tables[node->left], &tables[node->right], &shared_map, error);
      if (ok) {
        join_signature_pairs += shared_map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_join_map", 0, shared_map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join_acc(qsop, &shared_map, &tables[node->left], &tables[node->right],
                               &tables[node_id], decomposition->words, &join_pairs,
                               join_ws.acc ? &join_ws : NULL, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_join", 0, tables[node_id].len,
                                join_start);
      }
    }
    if (!ok) {
      join_workspace_free(&join_ws);
      join_map_free(&shared_map);
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      label_signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
  }

  join_workspace_free(&join_ws);

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    if (!qsop_count_add(&result->counts[residue], root->entries[i].count, error)) {
      join_map_free(&shared_map);
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      label_signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = ceil_log2_u64(max_signature_entries);
  }

  join_map_free(&shared_map);
  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  label_signature_pool_free(&pool);
  *out = result;
  return true;
}

/* Labelled CRT: build transition cache on the first prime.
 * Runs solve_labelled_leaf_mod for leaves and build_labelled_join_map +
 * solve_join_acc_mod for joins. Per-node maps stored in maps[]; caller frees. */
static bool solve_labelled_crt_build_maps(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, uint64_t modulus, uint64_t *counts,
    rw_label_signature_pool_t *pool, rw_join_map_t *maps,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_table_t *tables = calloc(nnodes, sizeof(*tables));
  if (tables == NULL) {
    set_error(error, "out of memory in labelled CRT transition build");
    return false;
  }
  uint64_t join_pairs = 0, join_sig_pairs = 0;
  uint64_t table_entries = 0, sig_entries = 0;
  uint64_t max_table_entries = 0, max_sig_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_labelled_leaf_mod(qsop, coeffs, node, decomposition->words, pool, modulus,
                                   &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.labelled_crt_leaf", 0, tables[node_id].len,
                              start);
    } else {
      maps[node_id].len = 0;
      ok = build_labelled_join_map(qsop, decomposition, node_id, coeffs, pool,
                                   &tables[node->left], &tables[node->right],
                                   &maps[node_id], error);
      if (ok) {
        join_sig_pairs += maps[node_id].len;
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_crt_join_map", 0,
                                maps[node_id].len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join_acc_mod(qsop, &maps[node_id], &tables[node->left],
                                   &tables[node->right], modulus, &tables[node_id],
                                   decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_crt_join", 0, tables[node_id].len,
                                join_start);
      }
    }
    if (!ok) {
      for (uint32_t t = 0; t < nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
    table_entries += tables[node_id].len;
    sig_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_sig_entries) {
      max_sig_entries = tables[node_id].reps_len;
    }
  }
  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }
  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = sig_entries;
    stats->max_signature_entries = max_sig_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_sig_pairs;
    stats->decomposition_width = ceil_log2_u64(max_sig_entries);
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  return true;
}

/* Labelled CRT: subsequent primes reuse maps cached by solve_labelled_crt_build_maps. */
static bool solve_labelled_crt_use_maps(
    const qsop_instance_t *qsop,
    const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, uint64_t modulus, uint64_t *counts,
    rw_label_signature_pool_t *pool, const rw_join_map_t *maps,
    qsop_error_t *error) {
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_table_t *tables = calloc(nnodes, sizeof(*tables));
  if (tables == NULL) {
    set_error(error, "out of memory in labelled CRT cached pass");
    return false;
  }
  uint64_t join_pairs = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_labelled_leaf_mod(qsop, coeffs, node, decomposition->words, pool, modulus,
                                   &tables[node_id], error);
    } else {
      ok = solve_join_acc_mod(qsop, &maps[node_id], &tables[node->left], &tables[node->right],
                                 modulus, &tables[node_id], decomposition->words, &join_pairs,
                                 error);
    }
    if (!ok) {
      for (uint32_t t = 0; t < nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      return false;
    }
  }
  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    counts[residue] = qsop_mod_add_u64(counts[residue], root->entries[i].count, modulus);
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  return true;
}

/* CRT with transition cache: builds join maps once (prime 0), reuses for all subsequent
 * primes — eliminating O(nnodes * nprimes) join-map allocs down to O(nnodes). */
static bool solve_labelled_count_table_crt(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "labelled rankwidth CRT count table is too large");
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
    set_error(error, "out of memory for labelled rankwidth CRT state");
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
    set_error(error, "out of memory for labelled rankwidth CRT result strings");
    return false;
  }
  const uint32_t nnodes = decomposition->nnodes == 0 ? 1U : decomposition->nnodes;
  rw_label_signature_pool_t pool = {0};
  rw_join_map_t *maps = calloc(nnodes, sizeof(*maps));
  if (maps == NULL || !label_signature_pool_init(&pool, qsop->nvars, error)) {
    if (pool.coeffs != NULL) {
      label_signature_pool_free(&pool);
    }
    free(maps);
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory for labelled CRT transition cache");
    return false;
  }
  bool ok = solve_labelled_crt_build_maps(qsop, decomposition, coeffs, primes[0],
                                             &all_counts[0], &pool, maps, stats, trace, error);
  for (size_t p = 1; p < nprimes && ok; p++) {
    ok = solve_labelled_crt_use_maps(qsop, decomposition, coeffs, primes[p],
                                        &all_counts[p * (size_t)qsop->r], &pool, maps, error);
  }
  if (!ok) {
    for (uint32_t t = 0; t < nnodes; t++) {
      join_map_free(&maps[t]);
    }
    free(maps);
    label_signature_pool_free(&pool);
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    return false;
  }
  for (uint32_t residue = 0; residue < qsop->r; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)qsop->r + residue];
    }
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                      error)) {
      for (uint32_t t = 0; t < nnodes; t++) {
        join_map_free(&maps[t]);
      }
      free(maps);
      label_signature_pool_free(&pool);
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }
  for (uint32_t t = 0; t < nnodes; t++) {
    join_map_free(&maps[t]);
  }
  free(maps);
  label_signature_pool_free(&pool);
  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}

static bool solve_labelled_count_table(const qsop_instance_t *qsop,
                                           const qsop_rankwidth_decomposition_t *decomposition,
                                           const uint32_t *coeffs, qsop_result_t **out,
                                           qsop_solve_stats_t *stats,
                                           qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (qsop->nvars >= 64U) {
    return solve_labelled_count_table_crt(qsop, decomposition, coeffs, out, stats, trace,
                                             error);
  }
  return solve_labelled_count_table_direct(qsop, decomposition, coeffs, out, stats, trace,
                                              error);
}

static void rankwidth_constant_stats(const qsop_instance_t *qsop, qsop_solve_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  stats->table_entries = 1;
  stats->max_table_entries = 1;
  stats->signature_entries = 1;
  stats->max_signature_entries = 1;
  stats->join_pairs = 0;
  stats->join_signature_pairs = 0;
  stats->rankwidth_table_forecast = 1;
  stats->rankwidth_join_pair_forecast = 0;
  stats->decomposition_width = 0;
  stats->rankwidth_support_width = 0;
  stats->rankwidth_labelled_width = 0;
  (void)qsop;
}

static bool solve_rankwidth_constant_result(const qsop_instance_t *qsop, qsop_result_t **out,
                                            qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error) {
  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth constant result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  result->counts[qsop->constant % qsop->r] = 1;
  rankwidth_constant_stats(qsop, stats);
  qsop_trace_emit(trace, "rankwidth.constant", 0, 1, 0);
  *out = result;
  return true;
}

static bool solve_rankwidth_constant_mod(const qsop_instance_t *qsop, uint64_t count_modulus,
                                         uint64_t *counts, qsop_solve_stats_t *stats,
                                         qsop_solve_trace_t *trace) {
  qsop_counts_clear(qsop->r, counts);
  counts[qsop->constant % qsop->r] = count_modulus == 0 ? 1 : 1 % count_modulus;
  rankwidth_constant_stats(qsop, stats);
  qsop_trace_emit(trace, "rankwidth.constant", 0, 1, 0);
  return true;
}

bool qsop_solve_rankwidth_count_table_mod_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint64_t count_modulus, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || decomposition == NULL || counts == NULL) {
    set_error(error, "internal error: null rankwidth modular solve argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  qsop_counts_clear(qsop->r, counts);
  if (qsop->nvars == 0) {
    return solve_rankwidth_constant_mod(qsop, count_modulus, counts, stats, trace);
  }
  if (!rankwidth_record_decomposition_diagnostics(qsop, decomposition, stats, trace, error)) {
    return false;
  }

  if (count_modulus == 0) {
    if (qsop->nvars >= 64U) {
      set_error(error, "rankwidth exact count-table handoff requires fewer than 64 variables");
      return false;
    }
    qsop_result_t *result = NULL;
    bool ok = false;
    if (qsop_is_sign_edge_instance(qsop)) {
      uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
      if (adj == NULL) {
        return false;
      }
      ok = solve_rankwidth_count_table(qsop, decomposition, adj,
                                          QSOP_RANKWIDTH_JOIN_AUTO, 0, &result, stats, trace, error);
      free(adj);
    } else {
      uint32_t *coeffs = coefficient_matrix(qsop, error);
      if (coeffs == NULL) {
        return false;
      }
      ok = solve_labelled_count_table(qsop, decomposition, coeffs, &result, stats, trace,
                                         error);
      free(coeffs);
    }
    if (!ok) {
      qsop_result_free(result);
      return false;
    }
    memcpy(counts, result->counts, (size_t)qsop->r * sizeof(*counts));
    qsop_result_free(result);
    return true;
  }

  if (qsop_is_sign_edge_instance(qsop)) {
    uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
    const bool ok = solve_rankwidth_count_table_mod_once(
        qsop, decomposition, adj, count_modulus, counts, stats, trace, error);
    free(adj);
    return ok;
  }

  uint32_t *coeffs = coefficient_matrix(qsop, error);
  if (coeffs == NULL) {
    return false;
  }
  const bool ok = solve_labelled_count_table_mod_once(qsop, decomposition, coeffs,
                                                      count_modulus, counts, stats, trace, error);
  free(coeffs);
  return ok;
}

static bool rankwidth_fourier_prime_state(uint32_t r, uint64_t prime, uint64_t *root_out,
                                          uint64_t **powers_out, uint64_t **inv_powers_out,
                                          qsop_error_t *error) {
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;
  if (!qsop_fourier_find_order_root(prime, r, &root, error)) {
    return false;
  }
  inv_root = qsop_mod_pow_u64(root, prime - 2U, prime);
  if (!qsop_fourier_make_root_powers(r, root, prime, &powers, error) ||
      !qsop_fourier_make_root_powers(r, inv_root, prime, &inv_powers, error)) {
    free(powers);
    free(inv_powers);
    return false;
  }
  *root_out = root;
  *powers_out = powers;
  *inv_powers_out = inv_powers;
  return true;
}

static bool solve_rankwidth_fourier_mod_once(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, uint64_t prime, const uint64_t *powers, const uint64_t *inv_powers,
    uint64_t *counts, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  rw_fourier_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (tables == NULL) {
    free(tables);
    set_error(error, "out of memory while allocating rankwidth Fourier solve state");
    return false;
  }

  rw_signature_pool_t pool = {0};
  if (!signature_pool_init(&pool, decomposition->words, error)) {
    free(tables);
    return false;
  }

  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_fourier_leaf(qsop, adj, node, powers, prime, decomposition->words, &pool,
                              &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.fourier_leaf", 0, tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_fourier_join_map(qsop, decomposition, node_id, adj, &pool, &tables[node->left],
                                  &tables[node->right], &map, error);
      if (ok) {
        qsop_trace_emit_elapsed(trace, "rankwidth.fourier_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_fourier_join(qsop, &map, &tables[node->left], &tables[node->right], powers,
                                prime, &tables[node_id], decomposition->words,
                                &join_signature_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.fourier_join", 0, tables[node_id].len,
                                join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        fourier_table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
      return false;
    }
    const uint64_t node_entries = (uint64_t)tables[node_id].len * qsop->r;
    table_entries += node_entries;
    signature_entries += tables[node_id].len;
    if (node_entries > max_table_entries) {
      max_table_entries = node_entries;
    }
    if (tables[node_id].len > max_signature_entries) {
      max_signature_entries = tables[node_id].len;
    }
  }

  const rw_fourier_table_t *root_table = &tables[decomposition->root];
  if (!fourier_root_counts_to_result(qsop, root_table, powers, inv_powers, prime, counts, error)) {
    for (uint32_t t = 0; t < decomposition->nnodes; t++) {
      fourier_table_free(&tables[t]);
    }
    free(tables);
    signature_pool_free(&pool);
    return false;
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_signature_pairs * qsop->r;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        fourier_table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    fourier_table_free(&tables[t]);
  }
  free(tables);
  signature_pool_free(&pool);
  return true;
}

static bool solve_rankwidth_fourier(const qsop_instance_t *qsop,
                                    const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_result_t **out,
                                    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                    qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_fourier_find_ntt_primes_for_nvars(qsop->r, qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "rankwidth Fourier CRT count table is too large");
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  uint64_t *all_counts = calloc(nprimes * (size_t)qsop->r, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  if (result == NULL || all_counts == NULL || residues == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth Fourier CRT solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (nprimes == 1U) {
    if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  } else {
    result->count_strings = calloc(qsop->r, sizeof(*result->count_strings));
    if (result->count_strings == NULL) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      set_error(error, "out of memory while allocating rankwidth Fourier CRT result strings");
      return false;
    }
  }

  for (size_t p = 0; p < nprimes; p++) {
    uint64_t root = 0;
    uint64_t *powers = NULL;
    uint64_t *inv_powers = NULL;
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    const bool ok =
        rankwidth_fourier_prime_state(qsop->r, primes[p], &root, &powers, &inv_powers, error) &&
        solve_rankwidth_fourier_mod_once(qsop, decomposition, adj, primes[p], powers, inv_powers,
                                         &all_counts[p * (size_t)qsop->r], stats_for_prime,
                                         trace_for_prime, error);
    (void)root;
    free(powers);
    free(inv_powers);
    if (!ok) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  if (nprimes == 1U) {
    memcpy(result->counts, all_counts, (size_t)qsop->r * sizeof(*result->counts));
  } else {
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
  }

  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}

static bool solve_rankwidth_labelled_fourier_mod_once(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, uint64_t prime, const uint64_t *powers,
    const uint64_t *inv_powers, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  rw_fourier_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (tables == NULL) {
    free(tables);
    set_error(error, "out of memory while allocating labelled rankwidth Fourier solve state");
    return false;
  }

  rw_label_signature_pool_t pool = {0};
  if (!label_signature_pool_init(&pool, qsop->nvars, error)) {
    free(tables);
    return false;
  }

  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_labelled_fourier_leaf(qsop, coeffs, node, powers, prime, decomposition->words,
                                       &pool, &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.labelled_fourier_leaf", 0,
                              tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_labelled_fourier_join_map(qsop, decomposition, node_id, coeffs, &pool,
                                           &tables[node->left], &tables[node->right], &map,
                                           error);
      if (ok) {
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_fourier_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_fourier_join(qsop, &map, &tables[node->left], &tables[node->right], powers,
                                prime, &tables[node_id], decomposition->words,
                                &join_signature_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_fourier_join", 0,
                                tables[node_id].len, join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        fourier_table_free(&tables[t]);
      }
      free(tables);
      label_signature_pool_free(&pool);
      return false;
    }
    const uint64_t node_entries = (uint64_t)tables[node_id].len * qsop->r;
    table_entries += node_entries;
    signature_entries += tables[node_id].len;
    if (node_entries > max_table_entries) {
      max_table_entries = node_entries;
    }
    if (tables[node_id].len > max_signature_entries) {
      max_signature_entries = tables[node_id].len;
    }
  }

  const rw_fourier_table_t *root_table = &tables[decomposition->root];
  if (!fourier_root_counts_to_result(qsop, root_table, powers, inv_powers, prime, counts, error)) {
    for (uint32_t t = 0; t < decomposition->nnodes; t++) {
      fourier_table_free(&tables[t]);
    }
    free(tables);
    label_signature_pool_free(&pool);
    return false;
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_signature_pairs * qsop->r;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = ceil_log2_u64(max_signature_entries);
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    fourier_table_free(&tables[t]);
  }
  free(tables);
  label_signature_pool_free(&pool);
  return true;
}

static bool solve_rankwidth_labelled_fourier(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_fourier_find_ntt_primes_for_nvars(qsop->r, qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "labelled rankwidth Fourier CRT count table is too large");
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  uint64_t *all_counts = calloc(nprimes * (size_t)qsop->r, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  if (result == NULL || all_counts == NULL || residues == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating labelled rankwidth Fourier CRT solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (nprimes == 1U) {
    if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  } else {
    result->count_strings = calloc(qsop->r, sizeof(*result->count_strings));
    if (result->count_strings == NULL) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      set_error(error, "out of memory while allocating labelled rankwidth Fourier CRT result strings");
      return false;
    }
  }

  for (size_t p = 0; p < nprimes; p++) {
    uint64_t root = 0;
    uint64_t *powers = NULL;
    uint64_t *inv_powers = NULL;
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    const bool ok =
        rankwidth_fourier_prime_state(qsop->r, primes[p], &root, &powers, &inv_powers, error) &&
        solve_rankwidth_labelled_fourier_mod_once(
            qsop, decomposition, coeffs, primes[p], powers, inv_powers,
            &all_counts[p * (size_t)qsop->r], stats_for_prime, trace_for_prime, error);
    (void)root;
    free(powers);
    free(inv_powers);
    if (!ok) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  if (nprimes == 1U) {
    memcpy(result->counts, all_counts, (size_t)qsop->r * sizeof(*result->counts));
  } else {
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
  }

  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}

bool qsop_solve_rankwidth_options_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode,
    const qsop_rankwidth_solve_options_t *options,
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
  if (qsop == NULL || decomposition == NULL) {
    set_error(error, "internal error: null rankwidth solve argument");
    return false;
  }
  if (qsop->nvars != decomposition->nvars) {
    set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (qsop->nvars > max_vars) {
    set_error(error,
              "rankwidth solver refuses %" PRIu32
              " variables; pass a larger --max-vars",
              qsop->nvars);
    return false;
  }
  if (qsop->nvars == 0) {
    return solve_rankwidth_constant_result(qsop, out, stats, trace, error);
  }
  if (!rankwidth_record_decomposition_diagnostics(qsop, decomposition, stats, trace, error)) {
    return false;
  }
  const qsop_rankwidth_join_strategy_t js = (options != NULL)
      ? options->join_strategy : QSOP_RANKWIDTH_JOIN_AUTO;
  const uint64_t mp = (options != NULL) ? options->materialize_join_max_pairs : 0;

  if (qsop_is_sign_edge_instance(qsop)) {
    uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
    /* D2.3: Fourier path does not support streaming; count-table path does. */
    const bool ok =
        mode == QSOP_RANKWIDTH_SOLVE_FOURIER
            ? solve_rankwidth_fourier(qsop, decomposition, adj, out, stats, trace, error)
            : solve_rankwidth_count_table(qsop, decomposition, adj, js, mp, out, stats, trace, error);
    free(adj);
    return ok;
  }

  uint32_t *coeffs = coefficient_matrix(qsop, error);
  if (coeffs == NULL) {
    return false;
  }
  const bool ok =
      mode == QSOP_RANKWIDTH_SOLVE_FOURIER
          ? solve_rankwidth_labelled_fourier(qsop, decomposition, coeffs, out, stats, trace, error)
          : solve_labelled_count_table(qsop, decomposition, coeffs, out, stats, trace, error);
  free(coeffs);
  return ok;
}

bool qsop_solve_rankwidth_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_rankwidth_options_mode_trace_stats(qsop, decomposition, max_vars, mode,
                                                       NULL, out, stats, trace, error);
}


bool qsop_solve_rankwidth_trace_stats(const qsop_instance_t *qsop,
                                      const qsop_rankwidth_decomposition_t *decomposition,
                                      uint32_t max_vars, qsop_result_t **out,
                                      qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                      qsop_error_t *error) {
  return qsop_solve_rankwidth_mode_trace_stats(qsop, decomposition, max_vars,
                                               QSOP_RANKWIDTH_SOLVE_COUNT_TABLE, out, stats,
                                               trace, error);
}

