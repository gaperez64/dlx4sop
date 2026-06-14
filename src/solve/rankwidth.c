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
  size_t len;
  size_t cap;
} rw_join_map_t;

typedef struct rw_fourier_table {
  uint32_t *signatures;
  uint64_t *assignments;
  uint64_t *values;
  size_t len;
  size_t cap;
} rw_fourier_table_t;

typedef struct rw_signature_pool {
  uint64_t *bits;
  uint64_t *fingerprints;
  size_t len;
  size_t cap;
  size_t words;
} rw_signature_pool_t;

typedef struct rw_label_signature_pool {
  uint32_t *coeffs;
  uint64_t *fingerprints;
  size_t len;
  size_t cap;
  uint32_t nvars;
} rw_label_signature_pool_t;

typedef struct rw_decomposition_score {
  uint32_t labelled_width;
  uint32_t support_width;
} rw_decomposition_score_t;

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
  if (!signature_pool_reserve(pool, pool->len + 1U, error)) {
    return false;
  }
  uint64_t *dst = qsop_bitset_row(pool->bits, pool->words, (uint32_t)pool->len);
  qsop_bitset_copy(dst, bits, pool->words);
  pool->fingerprints[pool->len] = fingerprint;
  *out = (uint32_t)pool->len;
  pool->len++;
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
  if (!label_signature_pool_reserve(pool, pool->len + 1U, error)) {
    return false;
  }
  uint32_t *dst = label_signature_coeffs(pool, (uint32_t)pool->len);
  memcpy(dst, coeffs, (size_t)pool->nvars * sizeof(*dst));
  pool->fingerprints[pool->len] = fingerprint;
  *out = (uint32_t)pool->len;
  pool->len++;
  return true;
}

static bool qsop_is_sign_edge_instance(const qsop_instance_t *qsop);
static uint64_t *adjacency_bitsets(const qsop_instance_t *qsop, size_t words,
                                   qsop_error_t *error);
static uint32_t *coefficient_matrix(const qsop_instance_t *qsop, qsop_error_t *error);
static uint32_t cut_rank_bitsets(uint32_t nvars, const uint64_t *adj, const uint64_t *left,
                                 const uint64_t *right, size_t words, qsop_error_t *error);
static uint32_t ceil_log2_u64(uint64_t value);
static uint32_t decomposition_width(const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_error_t *error);
static bool decomposition_score(const qsop_instance_t *qsop,
                                const qsop_rankwidth_decomposition_t *decomposition,
                                const uint64_t *adj, const uint32_t *coeffs,
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
  *map = (rw_join_map_t){0};
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
    set_error(error, "rankwidth decomposition generation requires at least one variable");
    return false;
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
    if (!decomposition_score(qsop, decomposition, adj, score_coeffs, &selected_score, error)) {
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
    if (!decomposition_score(qsop, min_fill, adj, score_coeffs, &min_fill_score, error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(min_fill);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (min_fill_score.labelled_width < selected_score.labelled_width ||
        (min_fill_score.labelled_width == selected_score.labelled_width &&
         min_fill_score.support_width < selected_score.support_width)) {
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
    if (!decomposition_score(qsop, left_deep, adj, score_coeffs, &left_deep_score, error)) {
      free(score_coeffs);
      free(adj);
      qsop_rankwidth_decomposition_free(left_deep);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (left_deep_score.labelled_width < selected_score.labelled_width ||
        (left_deep_score.labelled_width == selected_score.labelled_width &&
         left_deep_score.support_width < selected_score.support_width)) {
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

static uint32_t labelled_decomposition_proxy_width(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint32_t *coeffs, qsop_error_t *error) {
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
    const uint32_t cut_width =
        labelled_cut_signature_proxy_width(qsop, coeffs, left, right, error);
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
    labelled_width = labelled_decomposition_proxy_width(qsop, decomposition, coeffs, error);
    if (labelled_width == UINT32_MAX) {
      return false;
    }
  }
  *out = (rw_decomposition_score_t){
      .labelled_width = labelled_width,
      .support_width = support_width,
  };
  return true;
}

bool qsop_rankwidth_decomposition_support_width(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t *out, qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL || out == NULL) {
    set_error(error, "internal error: null rankwidth support-width argument");
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
  const uint32_t width = decomposition_width(decomposition, adj, error);
  free(adj);
  if (width == UINT32_MAX) {
    return false;
  }
  *out = width;
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
  return true;
}

static const rw_join_map_entry_t *join_map_get(const rw_join_map_t *map, uint32_t left_signature,
                                               uint32_t right_signature, size_t *index_out) {
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

static bool solve_leaf(const qsop_instance_t *qsop, const uint64_t *adj, const rw_node_t *node,
                       size_t words, rw_signature_pool_t *pool, rw_table_t *table,
                       qsop_error_t *error) {
  uint64_t *zero = calloc(words == 0 ? 1U : words, sizeof(*zero));
  uint64_t *assignment = calloc(words == 0 ? 1U : words, sizeof(*assignment));
  uint64_t *signature = calloc(words == 0 ? 1U : words, sizeof(*signature));
  if (zero == NULL || assignment == NULL || signature == NULL) {
    free(zero);
    free(assignment);
    free(signature);
    set_error(error, "out of memory while solving rankwidth leaf");
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
      table_add_entry(table, zero_signature, 0, 1, error) &&
      table_add_rep(table, one_signature, assignment, words, error) &&
      table_add_entry(table, one_signature, qsop->unary[node->var] % qsop->r, 1, error);
  free(zero);
  free(assignment);
  free(signature);
  return ok;
}

static bool solve_join(const qsop_instance_t *qsop, const rw_join_map_t *map,
                       const rw_table_t *left, const rw_table_t *right, rw_table_t *out,
                       size_t words, uint64_t *join_pairs, qsop_error_t *error) {
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      size_t map_index = 0;
      const rw_join_map_entry_t *mapped =
          join_map_get(map, left->entries[i].signature, right->entries[j].signature, &map_index);
      if (mapped == NULL) {
        set_error(error, "internal error: missing rankwidth join-map entry");
        return false;
      }
      const uint32_t residue =
          (uint32_t)(((uint64_t)left->entries[i].residue + right->entries[j].residue +
                      mapped->residue_shift) %
                     qsop->r);
      uint64_t product = 0;
      if (!qsop_count_mul(left->entries[i].count, right->entries[j].count, &product, error) ||
          !table_add_rep(out, mapped->parent_signature,
                         join_map_assignment(map, map_index, words), words, error) ||
          !table_add_entry(out, mapped->parent_signature, residue, product, error)) {
        return false;
      }
      (*join_pairs)++;
    }
  }
  return true;
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
  return true;
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

static bool solve_labelled_join(const qsop_instance_t *qsop, const rw_join_map_t *map,
                                const rw_table_t *left, const rw_table_t *right,
                                rw_table_t *out, size_t words, uint64_t *join_pairs,
                                qsop_error_t *error) {
  return solve_join(qsop, map, left, right, out, words, join_pairs, error);
}

static bool solve_labelled_join_mod(const qsop_instance_t *qsop, const rw_join_map_t *map,
                                    const rw_table_t *left, const rw_table_t *right,
                                    uint64_t modulus, rw_table_t *out, size_t words,
                                    uint64_t *join_pairs, qsop_error_t *error) {
  return solve_join_mod(qsop, map, left, right, modulus, out, words, join_pairs, error);
}

static uint32_t factor_u32(uint32_t value, uint32_t *factors, uint32_t cap) {
  uint32_t len = 0;
  uint32_t remaining = value;
  for (uint32_t p = 2; p <= remaining / p; p++) {
    if (remaining % p != 0) {
      continue;
    }
    if (len < cap) {
      factors[len++] = p;
    }
    while (remaining % p == 0) {
      remaining /= p;
    }
  }
  if (remaining > 1 && len < cap) {
    factors[len++] = remaining;
  }
  return len;
}

static bool find_ntt_prime(uint32_t r, uint32_t nvars, uint64_t *prime, qsop_error_t *error) {
  const uint64_t count_bound = UINT64_C(1) << nvars;
  uint64_t k = (UINT64_MAX - 1U) / r;
  for (uint64_t attempts = 0; attempts < UINT64_C(2000000) && k > 0; attempts++, k--) {
    const uint64_t candidate = k * (uint64_t)r + 1U;
    if (candidate <= count_bound) {
      break;
    }
    if (qsop_mod_is_prime_u64(candidate)) {
      *prime = candidate;
      return true;
    }
  }
  set_error(error, "rankwidth Fourier mode could not find a 64-bit NTT prime for modulus %" PRIu32,
            r);
  return false;
}

static bool find_order_root(uint64_t prime, uint32_t r, uint64_t *root, qsop_error_t *error) {
  uint32_t factors[32] = {0};
  const uint32_t nfactors = factor_u32(r, factors, 32);
  for (uint64_t g = 2; g < UINT64_C(1000000); g++) {
    const uint64_t candidate = qsop_mod_pow_u64(g, (prime - 1U) / r, prime);
    if (candidate == 1) {
      continue;
    }
    bool exact = true;
    for (uint32_t i = 0; i < nfactors; i++) {
      if (qsop_mod_pow_u64(candidate, r / factors[i], prime) == 1) {
        exact = false;
        break;
      }
    }
    if (exact) {
      *root = candidate;
      return true;
    }
  }
  set_error(error, "rankwidth Fourier mode could not find an order-%" PRIu32 " root", r);
  return false;
}

static bool make_root_powers(uint32_t r, uint64_t root, uint64_t prime, uint64_t **out,
                             qsop_error_t *error) {
  if ((size_t)r > SIZE_MAX / (r == 0 ? 1U : (size_t)r) / sizeof(uint64_t)) {
    set_error(error, "rankwidth Fourier modulus is too large for dense mode tables");
    return false;
  }
  uint64_t *powers = calloc((size_t)r * r, sizeof(*powers));
  if (powers == NULL) {
    set_error(error, "out of memory while allocating rankwidth Fourier powers");
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    for (uint32_t residue = 0; residue < r; residue++) {
      powers[(size_t)mode * r + residue] =
          qsop_mod_pow_u64(root, ((uint64_t)mode * residue) % r, prime);
    }
  }
  *out = powers;
  return true;
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

static bool solve_rankwidth_count_table_crt(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    const uint64_t *adj, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
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
    set_error(error, "out of memory while allocating rankwidth CRT solve state");
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
    set_error(error, "out of memory while allocating rankwidth CRT result strings");
    return false;
  }

  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!solve_rankwidth_count_table_mod_once(qsop, decomposition, adj, primes[p],
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
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                 error)) {
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


static bool solve_rankwidth_count_table(const qsop_instance_t *qsop,
                                        const qsop_rankwidth_decomposition_t *decomposition,
                                        const uint64_t *adj, qsop_result_t **out,
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
      ok = solve_leaf(qsop, adj, node, decomposition->words, &pool, &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.leaf", 0, tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_join_map(qsop, decomposition, node_id, adj, &pool, &tables[node->left],
                          &tables[node->right], &map, error);
      if (ok) {
        join_signature_pairs += map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join(qsop, &map, &tables[node->left], &tables[node->right], &tables[node_id],
                        decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.join", 0, tables[node_id].len, join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      signature_pool_free(&pool);
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
    set_error(error, "out of memory while allocating labelled rankwidth CRT solve state");
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
    set_error(error, "out of memory while allocating labelled rankwidth CRT result strings");
    return false;
  }

  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!solve_labelled_count_table_mod_once(qsop, decomposition, coeffs, primes[p],
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
    if (!qsop_crt_reconstruct_decimal(residues, primes, nprimes, &result->count_strings[residue],
                                 error)) {
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

static bool solve_labelled_count_table(const qsop_instance_t *qsop,
                                       const qsop_rankwidth_decomposition_t *decomposition,
                                       const uint32_t *coeffs, qsop_result_t **out,
                                       qsop_solve_stats_t *stats,
                                       qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (qsop->nvars >= 64U) {
    return solve_labelled_count_table_crt(qsop, decomposition, coeffs, out, stats, trace,
                                          error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating labelled rankwidth solve state");
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
      qsop_trace_emit_elapsed(trace, "rankwidth.labelled_leaf", 0, tables[node_id].len,
                              start);
    } else {
      rw_join_map_t map = {0};
      ok = build_labelled_join_map(qsop, decomposition, node_id, coeffs, &pool,
                                   &tables[node->left], &tables[node->right], &map, error);
      if (ok) {
        join_signature_pairs += map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_labelled_join(qsop, &map, &tables[node->left], &tables[node->right],
                                 &tables[node_id], decomposition->words, &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.labelled_join", 0, tables[node_id].len,
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

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  label_signature_pool_free(&pool);
  *out = result;
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
      ok = solve_rankwidth_count_table(qsop, decomposition, adj, &result, stats, trace, error);
      free(adj);
    } else {
      uint32_t *coeffs = coefficient_matrix(qsop, error);
      if (coeffs == NULL) {
        return false;
      }
      ok = solve_labelled_count_table(qsop, decomposition, coeffs, &result, stats, trace, error);
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

static bool solve_rankwidth_fourier(const qsop_instance_t *qsop,
                                    const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_result_t **out,
                                    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                    qsop_error_t *error) {
  if (qsop->nvars >= 64U) {
    set_error(error,
              "rankwidth Fourier mode currently requires fewer than 64 variables; use "
              "count-table mode for CRT-backed larger solves");
    return false;
  }
  uint64_t prime = 0;
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;
  if (!find_ntt_prime(qsop->r, qsop->nvars, &prime, error) ||
      !find_order_root(prime, qsop->r, &root, error)) {
    return false;
  }
  inv_root = qsop_mod_pow_u64(root, prime - 2U, prime);
  if (!make_root_powers(qsop->r, root, prime, &powers, error) ||
      !make_root_powers(qsop->r, inv_root, prime, &inv_powers, error)) {
    free(powers);
    free(inv_powers);
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_fourier_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(powers);
    free(inv_powers);
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth Fourier solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  rw_signature_pool_t pool = {0};
  if (!signature_pool_init(&pool, decomposition->words, error)) {
    free(powers);
    free(inv_powers);
    free(tables);
    qsop_result_free(result);
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
      free(powers);
      free(inv_powers);
      signature_pool_free(&pool);
      qsop_result_free(result);
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
  size_t root_index = 0;
  if (fourier_table_find_signature(root_table, 0, &root_index)) {
    const uint64_t inv_r = qsop_mod_pow_u64(qsop->r, prime - 2U, prime);
    for (uint32_t residue = 0; residue < qsop->r; residue++) {
      uint64_t sum = 0;
      for (uint32_t mode = 0; mode < qsop->r; mode++) {
        uint64_t value = root_table->values[root_index * (size_t)qsop->r + mode];
        value = qsop_mod_mul_u64(value, powers[(size_t)mode * qsop->r + (qsop->constant % qsop->r)],
                            prime);
        value = qsop_mod_mul_u64(value, inv_powers[(size_t)mode * qsop->r + residue], prime);
        sum = qsop_mod_add_u64(sum, value, prime);
      }
      result->counts[residue] = qsop_mod_mul_u64(sum, inv_r, prime);
    }
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
      free(powers);
      free(inv_powers);
      signature_pool_free(&pool);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    fourier_table_free(&tables[t]);
  }
  free(tables);
  free(powers);
  free(inv_powers);
  signature_pool_free(&pool);
  *out = result;
  return true;
}

bool qsop_solve_rankwidth_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
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
  if (qsop_is_sign_edge_instance(qsop)) {
    uint64_t *adj = adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
    const bool ok =
        mode == QSOP_RANKWIDTH_SOLVE_FOURIER
            ? solve_rankwidth_fourier(qsop, decomposition, adj, out, stats, trace, error)
            : solve_rankwidth_count_table(qsop, decomposition, adj, out, stats, trace, error);
    free(adj);
    return ok;
  }

  if (mode == QSOP_RANKWIDTH_SOLVE_FOURIER) {
    set_error(error, "rankwidth Fourier mode currently requires sign-only quadratic coefficients");
    return false;
  }

  uint32_t *coeffs = coefficient_matrix(qsop, error);
  if (coeffs == NULL) {
    return false;
  }
  const bool ok = solve_labelled_count_table(qsop, decomposition, coeffs, out, stats, trace,
                                             error);
  free(coeffs);
  return ok;
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
