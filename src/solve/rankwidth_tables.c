/* Generic table/signature-pool/dense-GF2-basis containers shared by the rankwidth DP families
 * (count-table and single-Fourier-mode complex).
 *
 * Part of the rankwidth.c file split (pure movement, no logic changes) -- see
 * rankwidth_internal.h for the shared types and cross-TU declarations. */
#include "../core/qsop_internal.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "dlx4sop/simd.h"
#include "rankwidth_internal.h"
#include "trace.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t *rw_table_assignment(const rw_table_t *table, size_t index, size_t words) {
  return qsop_bitset_row(table->assignments, words, (uint32_t)index);
}
uint64_t *rw_join_map_assignment(const rw_join_map_t *map, size_t index, size_t words) {
  return qsop_bitset_row(map->assignments, words, (uint32_t)index);
}
uint64_t *rw_fourier_assignment(const rw_fourier_table_t *table, size_t index, size_t words) {
  return qsop_bitset_row(table->assignments, words, (uint32_t)index);
}
uint64_t *rw_complex_assignment(const rw_complex_table_t *table, size_t index, size_t words) {
  return qsop_bitset_row(table->assignments, words, (uint32_t)index);
}
uint64_t *rw_complex64_assignment(const rw_complex64_table_t *table, size_t index, size_t words) {
  return qsop_bitset_row(table->assignments, words, (uint32_t)index);
}
const uint64_t *rw_signature_bits(const rw_signature_pool_t *pool, uint32_t signature) {
  return qsop_bitset_const_row(pool->bits, pool->words, signature);
}
static bool rw_sig_ht_build(rw_sig_ht_t *ht, const uint64_t *fingerprints, uint32_t n,
                            qsop_error_t *error) {
  size_t cap = 64U;
  while (cap < (size_t)n * 2U + 1U)
    cap *= 2U;
  uint32_t *slots = malloc(cap * sizeof(*slots));
  uint64_t *keys = malloc(cap * sizeof(*keys));
  if (slots == NULL || keys == NULL) {
    free(slots);
    free(keys);
    qsop_set_error(error, "out of memory building signature hash table");
    return false;
  }
  memset(slots, 0xFF, cap * sizeof(*slots)); /* UINT32_MAX = empty */
  uint32_t mask = (uint32_t)(cap - 1U);
  for (uint32_t i = 0; i < n; i++) {
    uint64_t fp = fingerprints[i];
    uint32_t h = (uint32_t)(fp ^ (fp >> 32U)) & mask;
    while (slots[h] != UINT32_MAX)
      h = (h + 1U) & mask;
    slots[h] = i;
    keys[h] = fp;
  }
  free(ht->slots);
  free(ht->keys);
  ht->slots = slots;
  ht->keys = keys;
  ht->mask = mask;
  return true;
}
static void rw_sig_ht_free(rw_sig_ht_t *ht) {
  free(ht->slots);
  free(ht->keys);
  *ht = (rw_sig_ht_t){0};
}
bool rw_signature_pool_init(rw_signature_pool_t *pool, size_t words, qsop_error_t *error) {
  if (pool == NULL) {
    qsop_set_error(error, "internal error: null rankwidth signature pool");
    return false;
  }
  *pool = (rw_signature_pool_t){
      .words = words,
  };
  return true;
}
void rw_signature_pool_free(rw_signature_pool_t *pool) {
  if (pool == NULL) {
    return;
  }
  rw_sig_ht_free(&pool->ht);
  free(pool->bits);
  free(pool->fingerprints);
  *pool = (rw_signature_pool_t){0};
}
bool rw_signature_pool_reserve(rw_signature_pool_t *pool, size_t needed, qsop_error_t *error) {
  if (needed <= pool->cap) {
    return true;
  }

  size_t new_cap = pool->cap == 0 ? 8U : pool->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth signature pool is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (pool->words != 0 && new_cap > SIZE_MAX / pool->words / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth signature pool is too large");
    return false;
  }
  uint64_t *bits = calloc(new_cap * pool->words, sizeof(*bits));
  uint64_t *fingerprints = calloc(new_cap, sizeof(*fingerprints));
  if (bits == NULL || fingerprints == NULL) {
    free(bits);
    free(fingerprints);
    qsop_set_error(error, "out of memory while growing rankwidth signature pool");
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
bool rw_signature_pool_intern(rw_signature_pool_t *pool, const uint64_t *bits, uint32_t *out,
                              qsop_error_t *error) {
  if (out == NULL) {
    qsop_set_error(error, "internal error: null rankwidth signature output");
    return false;
  }

  const uint64_t fingerprint = qsop_bitset_fingerprint(bits, pool->words);
  rw_sig_ht_t *ht = &pool->ht;

  if (ht->slots != NULL) {
    /* Hash table fast path: O(1) expected lookup */
    uint32_t h = (uint32_t)(fingerprint ^ (fingerprint >> 32U)) & ht->mask;
    for (;;) {
      if (ht->slots[h] == UINT32_MAX)
        break; /* empty: not found */
      if (ht->keys[h] == fingerprint &&
          qsop_bitset_equal(rw_signature_bits(pool, ht->slots[h]), bits, pool->words)) {
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
      while (ht->slots[h] != UINT32_MAX)
        h = (h + 1U) & ht->mask;
    }
    if (pool->len > UINT32_MAX) {
      qsop_set_error(error, "rankwidth signature pool exceeds uint32 ids");
      return false;
    }
    if (!rw_signature_pool_reserve(pool, pool->len + 1U, error))
      return false;
    uint64_t *dst = qsop_bitset_row(pool->bits, pool->words, (uint32_t)pool->len);
    qsop_bitset_copy(dst, bits, pool->words);
    pool->fingerprints[pool->len] = fingerprint;
    ht->slots[h] = (uint32_t)pool->len;
    ht->keys[h] = fingerprint;
    *out = (uint32_t)pool->len;
    pool->len++;
    return true;
  }

  /* Linear scan below RW_SIG_HT_THRESHOLD */
  for (size_t i = 0; i < pool->len; i++) {
    if (pool->fingerprints[i] == fingerprint &&
        qsop_bitset_equal(rw_signature_bits(pool, (uint32_t)i), bits, pool->words)) {
      *out = (uint32_t)i;
      return true;
    }
  }
  if (pool->len > UINT32_MAX) {
    qsop_set_error(error, "rankwidth signature pool exceeds uint32 ids");
    return false;
  }
  if (!rw_signature_pool_reserve(pool, pool->len + 1U, error))
    return false;
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
bool rw_reserve_entries(rw_table_t *table, size_t needed, qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  rw_entry_t *entries = realloc(table->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    qsop_set_error(error, "out of memory while growing rankwidth table");
    return false;
  }
  table->entries = entries;
  table->cap = new_cap;
  return true;
}
bool rw_reserve_reps(rw_table_t *table, size_t needed, size_t words, qsop_error_t *error) {
  if (needed <= table->reps_cap) {
    return true;
  }

  size_t new_cap = table->reps_cap == 0 ? 8U : table->reps_cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth signature table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth representative assignment table is too large");
    return false;
  }
  rw_signature_rep_t *reps = calloc(new_cap, sizeof(*reps));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  uint32_t *rep_weights = calloc(new_cap, sizeof(*rep_weights));
  if (reps == NULL || assignments == NULL || rep_weights == NULL) {
    free(reps);
    free(assignments);
    free(rep_weights);
    qsop_set_error(error, "out of memory while growing rankwidth signature table");
    return false;
  }
  if (table->reps_len != 0) {
    memcpy(reps, table->reps, table->reps_len * sizeof(*reps));
    memcpy(assignments, table->assignments, table->reps_len * words * sizeof(*assignments));
    memcpy(rep_weights, table->rep_weights, table->reps_len * sizeof(*rep_weights));
  }
  free(table->reps);
  free(table->assignments);
  free(table->rep_weights);
  table->reps = reps;
  table->assignments = assignments;
  table->rep_weights = rep_weights;
  table->reps_cap = new_cap;
  return true;
}
size_t rw_rep_hash(uint32_t signature) {
  return (size_t)(signature * UINT32_C(2654435761)); /* Knuth multiplicative */
}
bool rw_rep_slots_rehash(rw_table_t *table, qsop_error_t *error) {
  size_t new_cap = table->rep_slots_mask != 0 ? (table->rep_slots_mask + 1U) * 2U : 16U;
  const size_t need = (table->reps_len + 1U) * 2U;
  while (new_cap < need) {
    new_cap *= 2U;
  }
  uint32_t *slots = malloc(new_cap * sizeof(*slots));
  if (slots == NULL) {
    qsop_set_error(error, "out of memory while growing rankwidth signature index");
    return false;
  }
  memset(slots, 0xFF, new_cap * sizeof(*slots)); /* UINT32_MAX = empty */
  const size_t mask = new_cap - 1U;
  for (size_t i = 0; i < table->reps_len; i++) {
    size_t s = rw_rep_hash(table->reps[i].signature) & mask;
    while (slots[s] != UINT32_MAX) {
      s = (s + 1U) & mask;
    }
    slots[s] = (uint32_t)i;
  }
  free(table->rep_slots);
  table->rep_slots = slots;
  table->rep_slots_mask = mask;
  return true;
}
bool rw_fourier_slots_rehash(rw_fourier_table_t *table, qsop_error_t *error) {
  size_t new_cap = table->signature_slots_mask != 0 ? (table->signature_slots_mask + 1U) * 2U : 16U;
  const size_t need = (table->len + 1U) * 2U;
  while (new_cap < need) {
    new_cap *= 2U;
  }
  uint32_t *slots = malloc(new_cap * sizeof(*slots));
  if (slots == NULL) {
    qsop_set_error(error, "out of memory while growing rankwidth Fourier signature index");
    return false;
  }
  memset(slots, 0xFF, new_cap * sizeof(*slots));
  const size_t mask = new_cap - 1U;
  for (size_t i = 0; i < table->len; i++) {
    size_t s = rw_rep_hash(table->signatures[i]) & mask;
    while (slots[s] != UINT32_MAX) {
      s = (s + 1U) & mask;
    }
    slots[s] = (uint32_t)i;
  }
  free(table->signature_slots);
  table->signature_slots = slots;
  table->signature_slots_mask = mask;
  return true;
}
bool rw_complex_slots_rehash(rw_complex_table_t *table, qsop_error_t *error) {
  size_t new_cap = table->signature_slots_mask != 0 ? (table->signature_slots_mask + 1U) * 2U : 16U;
  const size_t need = (table->len + 1U) * 2U;
  while (new_cap < need) {
    new_cap *= 2U;
  }
  uint32_t *slots = malloc(new_cap * sizeof(*slots));
  if (slots == NULL) {
    qsop_set_error(error, "out of memory while growing rankwidth single-mode signature index");
    return false;
  }
  memset(slots, 0xFF, new_cap * sizeof(*slots));
  const size_t mask = new_cap - 1U;
  for (size_t i = 0; i < table->len; i++) {
    size_t s = rw_rep_hash(table->signatures[i]) & mask;
    while (slots[s] != UINT32_MAX) {
      s = (s + 1U) & mask;
    }
    slots[s] = (uint32_t)i;
  }
  free(table->signature_slots);
  table->signature_slots = slots;
  table->signature_slots_mask = mask;
  return true;
}
bool rw_complex64_slots_rehash(rw_complex64_table_t *table, qsop_error_t *error) {
  size_t new_cap = table->signature_slots_mask != 0 ? (table->signature_slots_mask + 1U) * 2U : 16U;
  const size_t need = (table->len + 1U) * 2U;
  while (new_cap < need) {
    new_cap *= 2U;
  }
  uint32_t *slots = malloc(new_cap * sizeof(*slots));
  if (slots == NULL) {
    qsop_set_error(error,
                   "out of memory while growing rankwidth double single-mode signature index");
    return false;
  }
  memset(slots, 0xFF, new_cap * sizeof(*slots));
  const size_t mask = new_cap - 1U;
  for (size_t i = 0; i < table->len; i++) {
    size_t s = rw_rep_hash(table->signatures[i]) & mask;
    while (slots[s] != UINT32_MAX) {
      s = (s + 1U) & mask;
    }
    slots[s] = (uint32_t)i;
  }
  free(table->signature_slots);
  table->signature_slots = slots;
  table->signature_slots_mask = mask;
  return true;
}
bool rw_table_find_rep_index(const rw_table_t *table, uint32_t signature, uint32_t *index_out) {
  if (table->rep_slots == NULL || table->rep_slots_mask == 0) {
    return false;
  }
  const size_t mask = table->rep_slots_mask;
  size_t slot = rw_rep_hash(signature) & mask;
  while (table->rep_slots[slot] != UINT32_MAX) {
    const uint32_t index = table->rep_slots[slot];
    if (table->reps[index].signature == signature) {
      if (index_out != NULL) {
        *index_out = index;
      }
      return true;
    }
    slot = (slot + 1U) & mask;
  }
  return false;
}
bool rw_table_rep_index(rw_table_t *table, uint32_t signature, const uint64_t *assignment,
                        size_t words, uint32_t *index_out, qsop_error_t *error) {
  if (table->rep_slots == NULL || (table->reps_len + 1U) * 2U > (table->rep_slots_mask + 1U)) {
    if (!rw_rep_slots_rehash(table, error)) {
      return false;
    }
  }
  const size_t mask = table->rep_slots_mask;
  size_t s = rw_rep_hash(signature) & mask;
  while (table->rep_slots[s] != UINT32_MAX) {
    if (table->reps[table->rep_slots[s]].signature == signature) {
      if (index_out != NULL) {
        *index_out = table->rep_slots[s];
      }
      return true; /* signature already has a representative */
    }
    s = (s + 1U) & mask;
  }
  if (!rw_reserve_reps(table, table->reps_len + 1U, words, error)) {
    return false;
  }
  table->reps[table->reps_len] = (rw_signature_rep_t){
      .signature = signature,
  };
  qsop_bitset_copy(rw_table_assignment(table, table->reps_len, words), assignment, words);
  table->rep_weights[table->reps_len] = qsop_bitset_popcount(assignment, words);
  table->rep_slots[s] = (uint32_t)table->reps_len;
  if (index_out != NULL) {
    *index_out = (uint32_t)table->reps_len;
  }
  table->reps_len++;
  return true;
}
bool rw_table_add_rep(rw_table_t *table, uint32_t signature, const uint64_t *assignment,
                      size_t words, qsop_error_t *error) {
  return rw_table_rep_index(table, signature, assignment, words, NULL, error);
}
bool rw_table_add_entry(rw_table_t *table, uint32_t signature, uint32_t residue, uint64_t count,
                        qsop_error_t *error) {
  if (count == 0) {
    return true;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->entries[i].signature == signature && table->entries[i].residue == residue) {
      return qsop_count_add(&table->entries[i].count, count, error);
    }
  }
  if (!rw_reserve_entries(table, table->len + 1U, error)) {
    return false;
  }
  table->entries[table->len++] = (rw_entry_t){
      .signature = signature,
      .residue = residue,
      .count = count,
  };
  return true;
}
bool rw_table_add_entry_mod(rw_table_t *table, uint32_t signature, uint32_t residue, uint64_t count,
                            uint64_t modulus, qsop_error_t *error) {
  if (count == 0) {
    return true;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->entries[i].signature == signature && table->entries[i].residue == residue) {
      table->entries[i].count = qsop_mod_add_u64(table->entries[i].count, count, modulus);
      return true;
    }
  }
  if (!rw_reserve_entries(table, table->len + 1U, error)) {
    return false;
  }
  table->entries[table->len++] = (rw_entry_t){
      .signature = signature,
      .residue = residue,
      .count = count,
  };
  return true;
}
void rw_table_free(rw_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->entries);
  free(table->reps);
  free(table->assignments);
  free(table->rep_weights);
  free(table->rep_slots);
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
void rw_table_sort(rw_table_t *table) {
  if (table->len <= 1) {
    return;
  }
  qsort(table->entries, table->len, sizeof(*table->entries), compare_entries_sig_residue);
}
bool rw_build_sig_range_index(const rw_table_t *table, uint32_t max_sig, uint32_t **starts_out,
                              uint32_t **ends_out, qsop_error_t *error) {
  const size_t n = (size_t)max_sig + 1U;
  uint32_t *starts = malloc(n * sizeof(*starts));
  uint32_t *ends = malloc(n * sizeof(*ends));
  if (starts == NULL || ends == NULL) {
    free(starts);
    free(ends);
    qsop_set_error(error, "out of memory allocating sig range index");
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
  *ends_out = ends;
  return true;
}
void rw_build_sig_range_index_into(const rw_table_t *table, uint32_t max_sig, uint32_t *starts,
                                   uint32_t *ends) {
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
bool rw_reserve_join_map(rw_join_map_t *map, size_t needed, size_t words, qsop_error_t *error) {
  if (needed <= map->cap) {
    return true;
  }

  size_t new_cap = map->cap == 0 ? 8U : map->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth join map is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth join assignment map is too large");
    return false;
  }
  rw_join_map_entry_t *entries = calloc(new_cap, sizeof(*entries));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  if (entries == NULL || assignments == NULL) {
    free(entries);
    free(assignments);
    qsop_set_error(error, "out of memory while growing rankwidth join map");
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
void rw_join_map_free(rw_join_map_t *map) {
  if (map == NULL) {
    return;
  }
  free(map->entries);
  free(map->assignments);
  *map = (rw_join_map_t){0};
}
bool rw_reserve_fourier_table(rw_fourier_table_t *table, size_t needed, uint32_t value_slots,
                              size_t words, qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth Fourier table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / (value_slots == 0 ? 1U : (size_t)value_slots) / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth Fourier table is too large");
    return false;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth Fourier assignment table is too large");
    return false;
  }

  uint32_t *signatures = calloc(new_cap, sizeof(*signatures));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  uint32_t *assignment_weights = calloc(new_cap, sizeof(*assignment_weights));
  uint64_t *values = calloc(new_cap * (size_t)value_slots, sizeof(*values));
  if (signatures == NULL || assignments == NULL || assignment_weights == NULL || values == NULL) {
    free(signatures);
    free(assignments);
    free(assignment_weights);
    free(values);
    qsop_set_error(error, "out of memory while growing rankwidth Fourier table");
    return false;
  }
  if (table->len != 0) {
    memcpy(signatures, table->signatures, table->len * sizeof(*signatures));
    memcpy(assignments, table->assignments, table->len * words * sizeof(*assignments));
    memcpy(assignment_weights, table->assignment_weights, table->len * sizeof(*assignment_weights));
    memcpy(values, table->values, table->len * (size_t)value_slots * sizeof(*values));
  }
  free(table->signatures);
  free(table->assignments);
  free(table->assignment_weights);
  free(table->values);
  table->signatures = signatures;
  table->assignments = assignments;
  table->assignment_weights = assignment_weights;
  table->values = values;
  table->cap = new_cap;
  return true;
}
void rw_fourier_table_free(rw_fourier_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->signatures);
  free(table->assignments);
  free(table->assignment_weights);
  free(table->values);
  free(table->signature_slots);
  *table = (rw_fourier_table_t){0};
}
bool rw_reserve_complex_table(rw_complex_table_t *table, size_t needed, size_t words,
                              qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth single-mode table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / sizeof(long double)) {
    qsop_set_error(error, "rankwidth single-mode table is too large");
    return false;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth single-mode assignment table is too large");
    return false;
  }

  uint32_t *signatures = calloc(new_cap, sizeof(*signatures));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  uint32_t *assignment_weights = calloc(new_cap, sizeof(*assignment_weights));
  /* malloc, not calloc: complex_table_signature_index is the sole insertion point for new
   * entries and already sets table->re[index]/im[index] = 0.0L explicitly right after growing
   * the table, so zero-initializing the whole (growing) buffer here is redundant work. */
  long double *re = malloc(new_cap * sizeof(*re));
  long double *im = malloc(new_cap * sizeof(*im));
  if (signatures == NULL || assignments == NULL || assignment_weights == NULL || re == NULL ||
      im == NULL) {
    free(signatures);
    free(assignments);
    free(assignment_weights);
    free(re);
    free(im);
    qsop_set_error(error, "out of memory while growing rankwidth single-mode table");
    return false;
  }
  if (table->len != 0) {
    memcpy(signatures, table->signatures, table->len * sizeof(*signatures));
    memcpy(assignments, table->assignments, table->len * words * sizeof(*assignments));
    memcpy(assignment_weights, table->assignment_weights, table->len * sizeof(*assignment_weights));
    memcpy(re, table->re, table->len * sizeof(*re));
    memcpy(im, table->im, table->len * sizeof(*im));
  }
  free(table->signatures);
  free(table->assignments);
  free(table->assignment_weights);
  free(table->re);
  free(table->im);
  table->signatures = signatures;
  table->assignments = assignments;
  table->assignment_weights = assignment_weights;
  table->re = re;
  table->im = im;
  table->cap = new_cap;
  return true;
}
void rw_complex_table_free(rw_complex_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->signatures);
  free(table->assignments);
  free(table->assignment_weights);
  free(table->re);
  free(table->im);
  free(table->signature_slots);
  *table = (rw_complex_table_t){0};
}
bool rw_reserve_complex64_table(rw_complex64_table_t *table, size_t needed, size_t words,
                                qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "rankwidth double single-mode table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / sizeof(double)) {
    qsop_set_error(error, "rankwidth double single-mode table is too large");
    return false;
  }
  if (words != 0 && new_cap > SIZE_MAX / words / sizeof(uint64_t)) {
    qsop_set_error(error, "rankwidth double single-mode assignment table is too large");
    return false;
  }

  uint32_t *signatures = calloc(new_cap, sizeof(*signatures));
  uint64_t *assignments = calloc(new_cap * words, sizeof(*assignments));
  uint32_t *assignment_weights = calloc(new_cap, sizeof(*assignment_weights));
  double *re = malloc(new_cap * sizeof(*re));
  double *im = malloc(new_cap * sizeof(*im));
  if (signatures == NULL || assignments == NULL || assignment_weights == NULL || re == NULL ||
      im == NULL) {
    free(signatures);
    free(assignments);
    free(assignment_weights);
    free(re);
    free(im);
    qsop_set_error(error, "out of memory while growing rankwidth double single-mode table");
    return false;
  }
  if (table->len != 0) {
    memcpy(signatures, table->signatures, table->len * sizeof(*signatures));
    memcpy(assignments, table->assignments, table->len * words * sizeof(*assignments));
    memcpy(assignment_weights, table->assignment_weights, table->len * sizeof(*assignment_weights));
    memcpy(re, table->re, table->len * sizeof(*re));
    memcpy(im, table->im, table->len * sizeof(*im));
  }
  free(table->signatures);
  free(table->assignments);
  free(table->assignment_weights);
  free(table->re);
  free(table->im);
  table->signatures = signatures;
  table->assignments = assignments;
  table->assignment_weights = assignment_weights;
  table->re = re;
  table->im = im;
  table->cap = new_cap;
  return true;
}
void rw_complex64_table_free(rw_complex64_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->signatures);
  free(table->assignments);
  free(table->assignment_weights);
  free(table->re);
  free(table->im);
  free(table->signature_slots);
  *table = (rw_complex64_table_t){0};
}
void rw_fill_all_vars(uint64_t *bits, uint32_t nvars, size_t words) {
  if (words == 0) {
    return;
  }
  memset(bits, 0xFF, words * sizeof(*bits));
  const uint32_t tail = nvars % 64U;
  if (tail != 0) {
    bits[words - 1U] = (UINT64_C(1) << tail) - 1U;
  }
}
bool rw_bitset_first_set_limited(const uint64_t *bits, size_t words, uint32_t nbits,
                                 uint32_t *out) {
  for (size_t w = 0; w < words; w++) {
    uint64_t value = bits[w];
    while (value != 0) {
      const uint32_t bit = rw_ctz_u64(value);
      const uint32_t index = (uint32_t)(w * 64U + bit);
      if (index < nbits) {
        *out = index;
        return true;
      }
      value &= value - 1U;
    }
  }
  return false;
}
bool rw_dense_basis_init(rw_dense_basis_t *basis, uint32_t nbits, size_t words,
                         qsop_error_t *error) {
  const size_t w = words == 0 ? 1U : words;
  basis->pivot_rows = calloc((nbits == 0 ? 1U : (size_t)nbits) * w, sizeof(*basis->pivot_rows));
  basis->pivot_coords = calloc(nbits == 0 ? 1U : (size_t)nbits, sizeof(*basis->pivot_coords));
  if (basis->pivot_rows == NULL || basis->pivot_coords == NULL) {
    free(basis->pivot_rows);
    free(basis->pivot_coords);
    *basis = (rw_dense_basis_t){0};
    qsop_set_error(error, "out of memory while allocating dense rankwidth basis");
    return false;
  }
  basis->nbits = nbits;
  basis->words = words;
  basis->dim = 0;
  return true;
}
void rw_dense_basis_free(rw_dense_basis_t *basis) {
  if (basis == NULL) {
    return;
  }
  free(basis->pivot_rows);
  free(basis->pivot_coords);
  *basis = (rw_dense_basis_t){0};
}
uint64_t *rw_dense_basis_pivot_row(const rw_dense_basis_t *basis, uint32_t pivot) {
  return basis->pivot_rows + (size_t)pivot * (basis->words == 0 ? 1U : basis->words);
}
bool rw_dense_basis_reduce(const rw_dense_basis_t *basis, const uint64_t *bits, uint64_t *scratch,
                           uint64_t *coord_out) {
  const qsop_simd_vtable_t *simd = rankwidth_bitset_simd();
  const size_t w = basis->words == 0 ? 1U : basis->words;
  memcpy(scratch, bits, w * sizeof(*scratch));
  uint64_t coord = 0;
  uint32_t pivot = 0;
  while (rw_bitset_first_set_limited(scratch, basis->words, basis->nbits, &pivot)) {
    const uint64_t pivot_coord = basis->pivot_coords[pivot];
    if (pivot_coord == 0) {
      *coord_out = coord;
      return false;
    }
    qsop_bitset_xor_simd(scratch, rw_dense_basis_pivot_row(basis, pivot), basis->words, simd);
    coord ^= pivot_coord;
  }
  *coord_out = coord;
  return true;
}
bool rw_dense_basis_add(rw_dense_basis_t *basis, const uint64_t *bits, uint64_t *scratch,
                        qsop_error_t *error) {
  uint64_t coord = 0;
  if (rw_dense_basis_reduce(basis, bits, scratch, &coord)) {
    (void)coord;
    return true;
  }

  uint32_t pivot = 0;
  if (!rw_bitset_first_set_limited(scratch, basis->words, basis->nbits, &pivot)) {
    return true;
  }
  if (basis->dim >= RW_DENSE_REFERENCE_MAX_DIM) {
    qsop_set_error(error, "rankwidth dense-reference basis dimension exceeds %" PRIu32,
                   (uint32_t)RW_DENSE_REFERENCE_MAX_DIM);
    return false;
  }
  const uint64_t coord_bit = UINT64_C(1) << basis->dim;
  qsop_bitset_copy(rw_dense_basis_pivot_row(basis, pivot), scratch, basis->words);
  basis->pivot_coords[pivot] = coord ^ coord_bit;
  basis->dim++;
  return true;
}
bool rw_dense_basis_coord(const rw_dense_basis_t *basis, const uint64_t *bits, uint64_t *scratch,
                          uint64_t *coord_out, qsop_error_t *error) {
  if (!rw_dense_basis_reduce(basis, bits, scratch, coord_out)) {
    qsop_set_error(error, "rankwidth dense-reference signature is outside its dense basis");
    return false;
  }
  return true;
}
bool rw_dense_reference_value_count(uint32_t dim, uint32_t slots_per_signature, size_t *out,
                                    qsop_error_t *error) {
  if (dim >= sizeof(size_t) * CHAR_BIT) {
    qsop_set_error(error, "rankwidth dense-reference dimension is too large for this platform");
    return false;
  }
  const size_t signatures = (size_t)1U << dim;
  if (slots_per_signature != 0 && signatures > SIZE_MAX / (size_t)slots_per_signature) {
    qsop_set_error(error, "rankwidth dense-reference table is too large");
    return false;
  }
  const size_t values = signatures * (size_t)slots_per_signature;
  if (values > RW_DENSE_REFERENCE_MAX_VALUES) {
    qsop_set_error(error, "rankwidth dense-reference table exceeds %" PRIu64 " value slots",
                   (uint64_t)RW_DENSE_REFERENCE_MAX_VALUES);
    return false;
  }
  *out = values;
  return true;
}
bool rw_dense_single_pair_count(size_t left_signatures, size_t right_signatures, uint64_t *out,
                                qsop_error_t *error) {
  if (left_signatures != 0 && right_signatures > UINT64_MAX / (uint64_t)left_signatures) {
    qsop_set_error(error, "rankwidth dense single-mode join coordinate space is too large");
    return false;
  }
  const uint64_t pairs = (uint64_t)left_signatures * (uint64_t)right_signatures;
  if (pairs > RW_DENSE_REFERENCE_MAX_VALUES) {
    qsop_set_error(error, "rankwidth dense single-mode join exceeds %" PRIu64 " coordinate pairs",
                   (uint64_t)RW_DENSE_REFERENCE_MAX_VALUES);
    return false;
  }
  if (out != NULL) {
    *out = pairs;
  }
  return true;
}
