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

typedef struct tw_factor {
  uint32_t arity;
  uint32_t *vars;
  size_t assignments;
  uint64_t *counts;
} tw_factor_t;

typedef struct tw_factor_list {
  tw_factor_t *items;
  size_t len;
  size_t cap;
} tw_factor_list_t;

typedef struct tw_context {
  uint32_t r;
  uint32_t max_bag_vars;
  uint64_t count_modulus;
  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
} tw_context_t;

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

static void add_saturating_u64(uint64_t *dst, uint64_t value) {
  if (UINT64_MAX - *dst < value) {
    *dst = UINT64_MAX;
  } else {
    *dst += value;
  }
}

static const char *treewidth_order_trace_phase(qsop_treewidth_order_t order) {
  switch (order) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
    return "treewidth.min_fill_order";
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return "treewidth.min_degree_order";
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return "treewidth.min_fill_max_degree_order";
  }
  return "treewidth.order";
}

static bool tw_count_add(const tw_context_t *ctx, uint64_t *dst, uint64_t value,
                         qsop_error_t *error) {
  if (ctx->count_modulus != 0) {
    *dst = qsop_mod_add_u64(*dst, value % ctx->count_modulus, ctx->count_modulus);
    return true;
  }
  return qsop_count_add(dst, value, error);
}

static bool tw_count_mul(const tw_context_t *ctx, uint64_t left, uint64_t right, uint64_t *out,
                         qsop_error_t *error) {
  if (ctx->count_modulus != 0) {
    *out = qsop_mod_mul_u64(left, right, ctx->count_modulus);
    return true;
  }
  return qsop_count_mul(left, right, out, error);
}

static uint64_t *factor_counts(const tw_factor_t *factor, uint32_t r, size_t assignment) {
  return factor->counts + assignment * (size_t)r;
}

static void factor_free(tw_factor_t *factor) {
  if (factor == NULL) {
    return;
  }
  free(factor->vars);
  free(factor->counts);
  *factor = (tw_factor_t){0};
}

static bool checked_assignment_count(uint32_t arity, size_t *out, qsop_error_t *error) {
  if (arity >= sizeof(size_t) * CHAR_BIT) {
    set_error(error, "treewidth bag has too many variables for dense factor storage");
    return false;
  }
  *out = (size_t)1U << arity;
  return true;
}

static bool factor_alloc_scope(const uint32_t *vars, uint32_t arity, uint32_t r,
                               tw_factor_t *out, qsop_error_t *error) {
  *out = (tw_factor_t){0};

  size_t assignments = 0;
  if (!checked_assignment_count(arity, &assignments, error)) {
    return false;
  }
  if (assignments > SIZE_MAX / (r == 0 ? 1U : (size_t)r) / sizeof(uint64_t)) {
    set_error(error, "treewidth factor table is too large");
    return false;
  }

  uint32_t *scope = malloc((arity == 0 ? 1U : (size_t)arity) * sizeof(*scope));
  uint64_t *counts = calloc(assignments * (size_t)r, sizeof(*counts));
  if (scope == NULL || counts == NULL) {
    free(scope);
    free(counts);
    set_error(error, "out of memory while allocating treewidth factor");
    return false;
  }
  if (arity != 0) {
    memcpy(scope, vars, (size_t)arity * sizeof(*scope));
  }

  *out = (tw_factor_t){
      .arity = arity,
      .vars = scope,
      .assignments = assignments,
      .counts = counts,
  };
  return true;
}

static bool factor_identity(uint32_t r, tw_factor_t *out, qsop_error_t *error) {
  if (!factor_alloc_scope(NULL, 0, r, out, error)) {
    return false;
  }
  out->counts[0] = 1;
  return true;
}

static bool factor_fourier_identity(uint32_t r, tw_factor_t *out, qsop_error_t *error) {
  if (!factor_alloc_scope(NULL, 0, r, out, error)) {
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    out->counts[mode] = 1;
  }
  return true;
}

static bool factor_contains_var(const tw_factor_t *factor, uint32_t var) {
  for (uint32_t i = 0; i < factor->arity; i++) {
    if (factor->vars[i] == var) {
      return true;
    }
  }
  return false;
}

static uint32_t factor_var_pos(const tw_factor_t *factor, uint32_t var) {
  for (uint32_t i = 0; i < factor->arity; i++) {
    if (factor->vars[i] == var) {
      return i;
    }
  }
  return UINT32_MAX;
}

static bool make_scope_union(const tw_factor_t *left, const tw_factor_t *right,
                             uint32_t **vars_out, uint32_t *arity_out, qsop_error_t *error) {
  uint32_t arity = 0;
  uint32_t *vars = malloc(((size_t)left->arity + right->arity + 1U) * sizeof(*vars));
  if (vars == NULL) {
    set_error(error, "out of memory while building treewidth factor scope");
    return false;
  }

  uint32_t i = 0;
  uint32_t j = 0;
  while (i < left->arity || j < right->arity) {
    if (j == right->arity || (i < left->arity && left->vars[i] < right->vars[j])) {
      vars[arity++] = left->vars[i++];
    } else if (i == left->arity || right->vars[j] < left->vars[i]) {
      vars[arity++] = right->vars[j++];
    } else {
      vars[arity++] = left->vars[i];
      i++;
      j++;
    }
  }

  *vars_out = vars;
  *arity_out = arity;
  return true;
}

static bool map_scope_positions(const uint32_t *superset, uint32_t superset_arity,
                                const uint32_t *subset, uint32_t subset_arity, uint32_t *positions,
                                qsop_error_t *error) {
  uint32_t cursor = 0;
  for (uint32_t i = 0; i < subset_arity; i++) {
    while (cursor < superset_arity && superset[cursor] < subset[i]) {
      cursor++;
    }
    if (cursor == superset_arity || superset[cursor] != subset[i]) {
      set_error(error, "internal error: treewidth factor scope is inconsistent");
      return false;
    }
    positions[i] = cursor;
  }
  return true;
}

static uint32_t trailing_zero_size(size_t value) {
  /* Callers pass a single set bit (a power of two), so value != 0. */
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctzll((unsigned long long)value);
#else
  uint32_t index = 0;
  while ((value & 1U) == 0) {
    value >>= 1U;
    index++;
  }
  return index;
#endif
}

static bool projection_map_alloc(size_t assignments, uint32_t superset_arity,
                                 const uint32_t *positions, uint32_t arity, size_t **out,
                                 qsop_error_t *error) {
  size_t *map = calloc(assignments == 0 ? 1U : assignments, sizeof(*map));
  if (map == NULL) {
    set_error(error, "out of memory while allocating treewidth projection map");
    return false;
  }

  size_t projection_bits[sizeof(size_t) * CHAR_BIT] = {0};
  for (uint32_t i = 0; i < arity; i++) {
    if (positions[i] >= superset_arity) {
      free(map);
      set_error(error, "internal error: treewidth projection position is out of range");
      return false;
    }
    projection_bits[positions[i]] = (size_t)1U << i;
  }

  for (size_t assignment = 1; assignment < assignments; assignment++) {
    const size_t bit = assignment & (~assignment + 1U);
    const uint32_t pos = trailing_zero_size(bit);
    map[assignment] = map[assignment ^ bit] | projection_bits[pos];
  }

  *out = map;
  return true;
}

static bool convolve_counts_to(uint64_t *restrict dst, const uint64_t *restrict left,
                               const uint64_t *restrict right, const tw_context_t *ctx,
                               qsop_error_t *error) {
  const uint32_t r = ctx->r;
  const uint64_t modulus = ctx->count_modulus;
  if (modulus != 0) {
    /* Modular counting: reduction is loop-invariant in `modulus`. */
    for (uint32_t a = 0; a < r; a++) {
      const uint64_t la = left[a];
      if (la == 0) {
        continue;
      }
      for (uint32_t b = 0; b < r; b++) {
        const uint64_t rb = right[b];
        if (rb == 0) {
          continue;
        }
        uint32_t residue = a + b;
        if (residue >= r) {
          residue -= r;
        }
        const uint64_t product = qsop_mod_mul_u64(la, rb % modulus, modulus);
        dst[residue] = qsop_mod_add_u64(dst[residue], product, modulus);
      }
    }
    return true;
  }
  /* Exact counting: use the hardware overflow flag instead of a per-element division;
     fall back to tw_count_* only on the rare overflow to reuse its error message. */
  for (uint32_t a = 0; a < r; a++) {
    const uint64_t la = left[a];
    if (la == 0) {
      continue;
    }
    for (uint32_t b = 0; b < r; b++) {
      const uint64_t rb = right[b];
      if (rb == 0) {
        continue;
      }
      uint32_t residue = a + b;
      if (residue >= r) {
        residue -= r;
      }
      uint64_t product;
      uint64_t sum;
      if (__builtin_mul_overflow(la, rb, &product) ||
          __builtin_add_overflow(dst[residue], product, &sum)) {
        /* Genuine overflow: defer to the checked helpers for the canonical error. */
        uint64_t checked = 0;
        if (!tw_count_mul(ctx, la, rb, &checked, error) ||
            !tw_count_add(ctx, &dst[residue], checked, error)) {
          return false;
        }
      } else {
        dst[residue] = sum;
      }
    }
  }
  return true;
}

static void note_factor_table(const tw_context_t *ctx, const tw_factor_t *factor) {
  if (ctx->stats == NULL) {
    return;
  }
  const uint64_t entries =
      factor->assignments > UINT64_MAX / (ctx->r == 0 ? 1U : (uint64_t)ctx->r)
          ? UINT64_MAX
          : (uint64_t)factor->assignments * ctx->r;
  ctx->stats->table_entries = entries;
  if (entries > ctx->stats->max_table_entries) {
    ctx->stats->max_table_entries = entries;
  }
}

static bool factor_multiply(const tw_factor_t *left, const tw_factor_t *right,
                            const tw_context_t *ctx, tw_factor_t *out, qsop_error_t *error) {
  uint32_t *vars = NULL;
  uint32_t arity = 0;
  if (!make_scope_union(left, right, &vars, &arity, error)) {
    return false;
  }
  if (arity > ctx->max_bag_vars) {
    free(vars);
    set_error(error,
              "treewidth backend refuses a %" PRIu32
              "-variable bag; pass a larger --max-vars or use another backend",
              arity);
    return false;
  }

  uint32_t *left_positions = malloc((left->arity == 0 ? 1U : left->arity) * sizeof(*left_positions));
  uint32_t *right_positions =
      malloc((right->arity == 0 ? 1U : right->arity) * sizeof(*right_positions));
  if (left_positions == NULL || right_positions == NULL) {
    free(vars);
    free(left_positions);
    free(right_positions);
    set_error(error, "out of memory while multiplying treewidth factors");
    return false;
  }
  if (!map_scope_positions(vars, arity, left->vars, left->arity, left_positions, error) ||
      !map_scope_positions(vars, arity, right->vars, right->arity, right_positions, error)) {
    free(vars);
    free(left_positions);
    free(right_positions);
    return false;
  }

  const uint64_t start = qsop_trace_begin(ctx->trace);
  bool ok = factor_alloc_scope(vars, arity, ctx->r, out, error);
  free(vars);
  if (!ok) {
    free(left_positions);
    free(right_positions);
    return false;
  }

  size_t *left_map = NULL;
  size_t *right_map = NULL;
  if (!projection_map_alloc(out->assignments, arity, left_positions, left->arity, &left_map,
                            error) ||
      !projection_map_alloc(out->assignments, arity, right_positions, right->arity, &right_map,
                            error)) {
    factor_free(out);
    free(left_positions);
    free(right_positions);
    free(left_map);
    free(right_map);
    return false;
  }

  for (size_t assignment = 0; assignment < out->assignments; assignment++) {
    const size_t left_assignment = left_map[assignment];
    const size_t right_assignment = right_map[assignment];
    uint64_t *dst = factor_counts(out, ctx->r, assignment);
    const uint64_t *left_counts = factor_counts(left, ctx->r, left_assignment);
    const uint64_t *right_counts = factor_counts(right, ctx->r, right_assignment);
    if (!convolve_counts_to(dst, left_counts, right_counts, ctx, error)) {
      factor_free(out);
      free(left_positions);
      free(right_positions);
      free(left_map);
      free(right_map);
      return false;
    }
  }

  free(left_positions);
  free(right_positions);
  free(left_map);
  free(right_map);
  if (ctx->stats != NULL) {
    add_saturating_u64(&ctx->stats->join_pairs, (uint64_t)out->assignments);
  }
  note_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.multiply", arity, out->assignments, start);
  return true;
}

static bool factor_multiply_fourier(const tw_factor_t *left, const tw_factor_t *right,
                                    const tw_context_t *ctx, tw_factor_t *out,
                                    qsop_error_t *error) {
  uint32_t *vars = NULL;
  uint32_t arity = 0;
  if (!make_scope_union(left, right, &vars, &arity, error)) {
    return false;
  }
  if (arity > ctx->max_bag_vars) {
    free(vars);
    set_error(error,
              "treewidth backend refuses a %" PRIu32
              "-variable bag; pass a larger --max-vars or use another backend",
              arity);
    return false;
  }

  uint32_t *left_positions = malloc((left->arity == 0 ? 1U : left->arity) * sizeof(*left_positions));
  uint32_t *right_positions =
      malloc((right->arity == 0 ? 1U : right->arity) * sizeof(*right_positions));
  if (left_positions == NULL || right_positions == NULL) {
    free(vars);
    free(left_positions);
    free(right_positions);
    set_error(error, "out of memory while multiplying treewidth Fourier factors");
    return false;
  }
  if (!map_scope_positions(vars, arity, left->vars, left->arity, left_positions, error) ||
      !map_scope_positions(vars, arity, right->vars, right->arity, right_positions, error)) {
    free(vars);
    free(left_positions);
    free(right_positions);
    return false;
  }

  const uint64_t start = qsop_trace_begin(ctx->trace);
  bool ok = factor_alloc_scope(vars, arity, ctx->r, out, error);
  free(vars);
  if (!ok) {
    free(left_positions);
    free(right_positions);
    return false;
  }

  size_t *left_map = NULL;
  size_t *right_map = NULL;
  if (!projection_map_alloc(out->assignments, arity, left_positions, left->arity, &left_map,
                            error) ||
      !projection_map_alloc(out->assignments, arity, right_positions, right->arity, &right_map,
                            error)) {
    factor_free(out);
    free(left_positions);
    free(right_positions);
    free(left_map);
    free(right_map);
    return false;
  }

  for (size_t assignment = 0; assignment < out->assignments; assignment++) {
    const size_t left_assignment = left_map[assignment];
    const size_t right_assignment = right_map[assignment];
    uint64_t *dst = factor_counts(out, ctx->r, assignment);
    const uint64_t *left_values = factor_counts(left, ctx->r, left_assignment);
    const uint64_t *right_values = factor_counts(right, ctx->r, right_assignment);
    for (uint32_t mode = 0; mode < ctx->r; mode++) {
      uint64_t product = 0;
      if (!tw_count_mul(ctx, left_values[mode], right_values[mode], &product, error) ||
          !tw_count_add(ctx, &dst[mode], product, error)) {
        factor_free(out);
        free(left_positions);
        free(right_positions);
        free(left_map);
        free(right_map);
        return false;
      }
    }
  }

  free(left_positions);
  free(right_positions);
  free(left_map);
  free(right_map);
  if (ctx->stats != NULL) {
    add_saturating_u64(&ctx->stats->join_pairs, (uint64_t)out->assignments);
  }
  note_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.fourier_multiply", arity, out->assignments,
                          start);
  return true;
}

static bool add_counts_to(uint64_t *dst, const uint64_t *src, const tw_context_t *ctx,
                          qsop_error_t *error) {
  for (uint32_t residue = 0; residue < ctx->r; residue++) {
    if (src[residue] != 0 && !tw_count_add(ctx, &dst[residue], src[residue], error)) {
      return false;
    }
  }
  return true;
}

static bool add_fourier_values_to(uint64_t *dst, const uint64_t *src, const tw_context_t *ctx,
                                  qsop_error_t *error) {
  for (uint32_t mode = 0; mode < ctx->r; mode++) {
    if (src[mode] != 0 && !tw_count_add(ctx, &dst[mode], src[mode], error)) {
      return false;
    }
  }
  return true;
}

static size_t remove_assignment_bit(size_t assignment, uint32_t pos) {
  const size_t low_mask = ((size_t)1U << pos) - 1U;
  const size_t low = assignment & low_mask;
  const size_t high = assignment >> (pos + 1U);
  return low | (high << pos);
}

static bool factor_sum_out(const tw_factor_t *input, uint32_t var, const tw_context_t *ctx,
                           tw_factor_t *out, qsop_error_t *error) {
  const uint32_t pos = factor_var_pos(input, var);
  if (pos == UINT32_MAX) {
    set_error(error, "internal error: treewidth factor does not contain eliminated variable");
    return false;
  }

  uint32_t *vars = malloc((input->arity == 0 ? 1U : (size_t)input->arity) * sizeof(*vars));
  if (vars == NULL) {
    set_error(error, "out of memory while projecting treewidth factor");
    return false;
  }
  uint32_t arity = 0;
  for (uint32_t i = 0; i < input->arity; i++) {
    if (i != pos) {
      vars[arity++] = input->vars[i];
    }
  }

  const uint64_t start = qsop_trace_begin(ctx->trace);
  bool ok = factor_alloc_scope(vars, arity, ctx->r, out, error);
  free(vars);
  if (!ok) {
    return false;
  }

  for (size_t assignment = 0; assignment < input->assignments; assignment++) {
    const size_t projected = remove_assignment_bit(assignment, pos);
    if (!add_counts_to(factor_counts(out, ctx->r, projected),
                       factor_counts(input, ctx->r, assignment), ctx, error)) {
      factor_free(out);
      return false;
    }
  }

  note_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.sum_out", arity, out->assignments, start);
  return true;
}

static bool factor_sum_out_fourier(const tw_factor_t *input, uint32_t var,
                                   const tw_context_t *ctx, tw_factor_t *out,
                                   qsop_error_t *error) {
  const uint32_t pos = factor_var_pos(input, var);
  if (pos == UINT32_MAX) {
    set_error(error, "internal error: treewidth factor does not contain eliminated variable");
    return false;
  }

  uint32_t *vars = malloc((input->arity == 0 ? 1U : (size_t)input->arity) * sizeof(*vars));
  if (vars == NULL) {
    set_error(error, "out of memory while projecting treewidth Fourier factor");
    return false;
  }
  uint32_t arity = 0;
  for (uint32_t i = 0; i < input->arity; i++) {
    if (i != pos) {
      vars[arity++] = input->vars[i];
    }
  }

  const uint64_t start = qsop_trace_begin(ctx->trace);
  bool ok = factor_alloc_scope(vars, arity, ctx->r, out, error);
  free(vars);
  if (!ok) {
    return false;
  }

  for (size_t assignment = 0; assignment < input->assignments; assignment++) {
    const size_t projected = remove_assignment_bit(assignment, pos);
    if (!add_fourier_values_to(factor_counts(out, ctx->r, projected),
                               factor_counts(input, ctx->r, assignment), ctx, error)) {
      factor_free(out);
      return false;
    }
  }

  note_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.fourier_sum_out", arity, out->assignments,
                          start);
  return true;
}

static bool factor_list_reserve(tw_factor_list_t *list, size_t needed, qsop_error_t *error) {
  if (needed <= list->cap) {
    return true;
  }

  size_t new_cap = list->cap == 0 ? 16U : list->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "treewidth factor list is too large");
      return false;
    }
    new_cap *= 2U;
  }
  tw_factor_t *items = realloc(list->items, new_cap * sizeof(*items));
  if (items == NULL) {
    set_error(error, "out of memory while growing treewidth factor list");
    return false;
  }
  for (size_t i = list->cap; i < new_cap; i++) {
    items[i] = (tw_factor_t){0};
  }
  list->items = items;
  list->cap = new_cap;
  return true;
}

static bool factor_list_push_take(tw_factor_list_t *list, tw_factor_t *factor,
                                  qsop_error_t *error) {
  if (!factor_list_reserve(list, list->len + 1U, error)) {
    return false;
  }
  list->items[list->len++] = *factor;
  *factor = (tw_factor_t){0};
  return true;
}

static void factor_list_remove_at(tw_factor_list_t *list, size_t index) {
  factor_free(&list->items[index]);
  list->len--;
  if (index != list->len) {
    list->items[index] = list->items[list->len];
    list->items[list->len] = (tw_factor_t){0};
  }
}

static tw_factor_t factor_list_take_at(tw_factor_list_t *list, size_t index) {
  tw_factor_t taken = list->items[index];
  list->len--;
  if (index != list->len) {
    list->items[index] = list->items[list->len];
  }
  list->items[list->len] = (tw_factor_t){0};
  return taken;
}

static void factor_list_free(tw_factor_list_t *list) {
  if (list == NULL) {
    return;
  }
  for (size_t i = 0; i < list->len; i++) {
    factor_free(&list->items[i]);
  }
  free(list->items);
  *list = (tw_factor_list_t){0};
}

static uint64_t *adjacency_bitsets(const qsop_instance_t *qsop, size_t words,
                                   qsop_error_t *error) {
  if (qsop->nvars != 0 && (size_t)qsop->nvars > SIZE_MAX / words / sizeof(uint64_t)) {
    set_error(error, "treewidth adjacency matrix is too large");
    return NULL;
  }
  const size_t cells =
      qsop->nvars == 0 || words == 0 ? 1U : (size_t)qsop->nvars * words;
  uint64_t *adj = calloc(cells, sizeof(*adj));
  if (adj == NULL) {
    set_error(error, "out of memory while allocating treewidth adjacency matrix");
    return NULL;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(adj, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(adj, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  return adj;
}

static uint64_t missing_edges_for(uint32_t v, const uint64_t *work, const uint64_t *active,
                                  uint64_t *remaining, size_t words, uint32_t nvars,
                                  uint64_t stop_after) {
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
      if (fill > stop_after) {
        return fill;
      }
    }
  }
  return fill;
}

static bool treewidth_order_needs_fill(qsop_treewidth_order_t order, bool found, uint32_t degree,
                                       uint32_t best_degree) {
  if (!found) {
    return true;
  }
  switch (order) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return true;
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return degree <= best_degree;
  }
  return true;
}

static uint64_t treewidth_fill_stop_after(qsop_treewidth_order_t order, bool found,
                                          uint32_t degree, uint64_t best_fill,
                                          uint32_t best_degree) {
  if (!found) {
    return UINT64_MAX;
  }
  switch (order) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return best_fill;
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return degree == best_degree ? best_fill : UINT64_MAX;
  }
  return UINT64_MAX;
}

static bool treewidth_candidate_is_better(qsop_treewidth_order_t order, bool found,
                                          uint64_t fill, uint32_t degree, uint64_t best_fill,
                                          uint32_t best_degree) {
  if (!found) {
    return true;
  }
  switch (order) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
    return fill < best_fill || (fill == best_fill && degree < best_degree);
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return degree < best_degree || (degree == best_degree && fill < best_fill);
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return fill < best_fill || (fill == best_fill && degree > best_degree);
  }
  return false;
}

static bool make_treewidth_order(const qsop_instance_t *qsop, qsop_treewidth_order_t order_policy,
                                 uint32_t *order, uint32_t *width_out, qsop_error_t *error) {
  const size_t words = qsop_bitset_words(qsop->nvars);
  uint64_t *work = adjacency_bitsets(qsop, words, error);
  uint64_t *active = calloc(words == 0 ? 1U : words, sizeof(*active));
  uint64_t *scratch = calloc(words == 0 ? 1U : words, sizeof(*scratch));
  if (work == NULL || active == NULL || scratch == NULL) {
    free(work);
    free(active);
    free(scratch);
    set_error(error, "out of memory while building treewidth order");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    qsop_bitset_set(active, v);
  }

  uint32_t width = 0;
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
      if (!treewidth_order_needs_fill(order_policy, found, degree, best_degree)) {
        continue;
      }
      const uint64_t fill =
          missing_edges_for(v, work, active, scratch, words, qsop->nvars,
                            treewidth_fill_stop_after(order_policy, found, degree, best_fill,
                                                      best_degree));
      if (treewidth_candidate_is_better(order_policy, found, fill, degree, best_fill,
                                        best_degree)) {
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
      set_error(error, "internal error: treewidth order stopped early");
      return false;
    }
    order[pos] = best;
    if (best_degree > width) {
      width = best_degree;
    }

    /* Eliminate `best`: connect its neighbours (fill edges) and drop `best` from the graph.
       Adjacency is symmetric, so only `best`'s neighbours hold `best` in their rows — masking
       every row against `active` (the old approach) was O(nvars * words) of redundant work per
       elimination. Clear `best` from `active` first so the per-neighbour mask removes it. */
    const uint64_t *best_row = qsop_bitset_const_row(work, words, best);
    for (size_t w = 0; w < words; w++) {
      scratch[w] = best_row[w] & active[w];
    }
    qsop_bitset_clear(active, best);
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
  }

  free(work);
  free(active);
  free(scratch);
  *width_out = width;
  return true;
}

static bool append_unary_factor(const qsop_instance_t *qsop, uint32_t var, tw_factor_list_t *list,
                                qsop_error_t *error) {
  tw_factor_t factor = {0};
  if (!factor_alloc_scope(&var, 1, qsop->r, &factor, error)) {
    return false;
  }
  factor_counts(&factor, qsop->r, 0)[0] = 1;
  factor_counts(&factor, qsop->r, 1)[qsop->unary[var] % qsop->r] = 1;
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

static bool append_unary_factor_fourier(const qsop_instance_t *qsop, uint32_t var,
                                        const uint64_t *powers, tw_factor_list_t *list,
                                        qsop_error_t *error) {
  tw_factor_t factor = {0};
  if (!factor_alloc_scope(&var, 1, qsop->r, &factor, error)) {
    return false;
  }
  for (uint32_t mode = 0; mode < qsop->r; mode++) {
    factor_counts(&factor, qsop->r, 0)[mode] = 1;
    factor_counts(&factor, qsop->r, 1)[mode] =
        powers[(size_t)mode * qsop->r + (qsop->unary[var] % qsop->r)];
  }
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

static bool append_edge_factor(const qsop_instance_t *qsop, uint32_t edge, tw_factor_list_t *list,
                               qsop_error_t *error) {
  uint32_t vars[2] = {qsop->edge_u[edge], qsop->edge_v[edge]};
  if (vars[0] > vars[1]) {
    const uint32_t tmp = vars[0];
    vars[0] = vars[1];
    vars[1] = tmp;
  }

  tw_factor_t factor = {0};
  if (!factor_alloc_scope(vars, 2, qsop->r, &factor, error)) {
    return false;
  }
  for (size_t assignment = 0; assignment < factor.assignments; assignment++) {
    const bool left = (assignment & 1U) != 0;
    const bool right = (assignment & 2U) != 0;
    const uint32_t residue = left && right ? qsop->edge_q[edge] % qsop->r : 0;
    factor_counts(&factor, qsop->r, assignment)[residue] = 1;
  }
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

static bool append_edge_factor_fourier(const qsop_instance_t *qsop, uint32_t edge,
                                       const uint64_t *powers, tw_factor_list_t *list,
                                       qsop_error_t *error) {
  uint32_t vars[2] = {qsop->edge_u[edge], qsop->edge_v[edge]};
  if (vars[0] > vars[1]) {
    const uint32_t tmp = vars[0];
    vars[0] = vars[1];
    vars[1] = tmp;
  }

  tw_factor_t factor = {0};
  if (!factor_alloc_scope(vars, 2, qsop->r, &factor, error)) {
    return false;
  }
  for (size_t assignment = 0; assignment < factor.assignments; assignment++) {
    const bool left = (assignment & 1U) != 0;
    const bool right = (assignment & 2U) != 0;
    const uint32_t residue = left && right ? qsop->edge_q[edge] % qsop->r : 0;
    for (uint32_t mode = 0; mode < qsop->r; mode++) {
      factor_counts(&factor, qsop->r, assignment)[mode] =
          powers[(size_t)mode * qsop->r + residue];
    }
  }
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

static bool build_initial_factors(const qsop_instance_t *qsop, tw_factor_list_t *list,
                                  qsop_error_t *error) {
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!append_unary_factor(qsop, v, list, error)) {
      return false;
    }
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (!append_edge_factor(qsop, e, list, error)) {
      return false;
    }
  }
  return true;
}

static bool build_initial_factors_fourier(const qsop_instance_t *qsop, const uint64_t *powers,
                                          tw_factor_list_t *list, qsop_error_t *error) {
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!append_unary_factor_fourier(qsop, v, powers, list, error)) {
      return false;
    }
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (!append_edge_factor_fourier(qsop, e, powers, list, error)) {
      return false;
    }
  }
  return true;
}

static bool eliminate_variable(tw_factor_list_t *list, uint32_t var, const tw_context_t *ctx,
                               qsop_error_t *error) {
  tw_factor_t combined = {0};
  bool collected = false;
  for (size_t i = 0; i < list->len;) {
    if (!factor_contains_var(&list->items[i], var)) {
      i++;
      continue;
    }

    if (!collected) {
      combined = factor_list_take_at(list, i);
      collected = true;
      continue;
    }

    tw_factor_t next = {0};
    if (!factor_multiply(&combined, &list->items[i], ctx, &next, error)) {
      factor_free(&combined);
      return false;
    }
    factor_free(&combined);
    combined = next;
    factor_list_remove_at(list, i);
    collected = true;
  }

  if (!collected) {
    set_error(error, "internal error: treewidth elimination found no factor for variable");
    return false;
  }

  tw_factor_t reduced = {0};
  if (!factor_sum_out(&combined, var, ctx, &reduced, error)) {
    factor_free(&combined);
    return false;
  }
  factor_free(&combined);
  if (!factor_list_push_take(list, &reduced, error)) {
    factor_free(&reduced);
    return false;
  }
  return true;
}

static bool eliminate_variable_fourier(tw_factor_list_t *list, uint32_t var,
                                       const tw_context_t *ctx, qsop_error_t *error) {
  tw_factor_t combined = {0};
  bool collected = false;
  for (size_t i = 0; i < list->len;) {
    if (!factor_contains_var(&list->items[i], var)) {
      i++;
      continue;
    }

    if (!collected) {
      combined = factor_list_take_at(list, i);
      collected = true;
      continue;
    }

    tw_factor_t next = {0};
    if (!factor_multiply_fourier(&combined, &list->items[i], ctx, &next, error)) {
      factor_free(&combined);
      return false;
    }
    factor_free(&combined);
    combined = next;
    factor_list_remove_at(list, i);
    collected = true;
  }

  if (!collected) {
    set_error(error, "internal error: treewidth elimination found no factor for variable");
    return false;
  }

  tw_factor_t reduced = {0};
  if (!factor_sum_out_fourier(&combined, var, ctx, &reduced, error)) {
    factor_free(&combined);
    return false;
  }
  factor_free(&combined);
  if (!factor_list_push_take(list, &reduced, error)) {
    factor_free(&reduced);
    return false;
  }
  return true;
}

static bool multiply_remaining_factors(tw_factor_list_t *list, const tw_context_t *ctx,
                                       tw_factor_t *out, qsop_error_t *error) {
  if (list->len == 0) {
    return factor_identity(ctx->r, out, error);
  }

  *out = factor_list_take_at(list, 0);
  while (list->len != 0) {
    tw_factor_t next = {0};
    if (!factor_multiply(out, &list->items[0], ctx, &next, error)) {
      factor_free(out);
      return false;
    }
    factor_free(out);
    *out = next;
    factor_list_remove_at(list, 0);
  }
  return true;
}

static bool multiply_remaining_factors_fourier(tw_factor_list_t *list, const tw_context_t *ctx,
                                               tw_factor_t *out, qsop_error_t *error) {
  if (list->len == 0) {
    return factor_fourier_identity(ctx->r, out, error);
  }

  *out = factor_list_take_at(list, 0);
  while (list->len != 0) {
    tw_factor_t next = {0};
    if (!factor_multiply_fourier(out, &list->items[0], ctx, &next, error)) {
      factor_free(out);
      return false;
    }
    factor_free(out);
    *out = next;
    factor_list_remove_at(list, 0);
  }
  return true;
}

static bool shift_result_counts(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift,
                                const tw_context_t *ctx, qsop_error_t *error) {
  qsop_counts_clear(r, dst);
  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t target = residue + delta;
    if (target >= r) {
      target -= r;
    }
    if (!tw_count_add(ctx, &dst[target], src[residue], error)) {
      return false;
    }
  }
  return true;
}

static bool solve_treewidth_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                 const uint32_t *order, uint32_t order_width,
                                 uint64_t count_modulus, uint64_t *counts,
                                 qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                 qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || order == NULL || counts == NULL) {
    set_error(error, "internal error: null treewidth solve argument");
    return false;
  }
  if (max_bag_vars == 0 && qsop->nvars != 0) {
    set_error(error,
              "treewidth backend refuses non-constant instances with --max-vars 0; pass a larger "
              "--max-vars");
    return false;
  }

  tw_context_t ctx = {
      .r = qsop->r,
      .max_bag_vars = max_bag_vars,
      .count_modulus = count_modulus,
      .stats = stats,
      .trace = trace,
  };

  if (stats != NULL) {
    stats->decomposition_width = order_width;
  }

  tw_factor_list_t factors = {0};
  const uint64_t factors_start = qsop_trace_begin(trace);
  if (!build_initial_factors(qsop, &factors, error)) {
    factor_list_free(&factors);
    return false;
  }
  qsop_trace_emit_elapsed(trace, "treewidth.initial_factors", 0, factors.len, factors_start);

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    if (!eliminate_variable(&factors, order[pos], &ctx, error)) {
      factor_list_free(&factors);
      return false;
    }
  }

  tw_factor_t final = {0};
  if (!multiply_remaining_factors(&factors, &ctx, &final, error)) {
    factor_list_free(&factors);
    return false;
  }
  if (final.arity != 0) {
    factor_list_free(&factors);
    factor_free(&final);
    set_error(error, "internal error: treewidth solve left an uneliminated factor");
    return false;
  }
  if (!shift_result_counts(qsop->r, counts, final.counts, qsop->constant, &ctx, error)) {
    factor_list_free(&factors);
    factor_free(&final);
    return false;
  }

  factor_list_free(&factors);
  factor_free(&final);
  return true;
}

static bool treewidth_fourier_prime_state(uint32_t r, uint64_t prime, uint64_t **powers_out,
                                          uint64_t **inv_powers_out, qsop_error_t *error) {
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
  *powers_out = powers;
  *inv_powers_out = inv_powers;
  return true;
}

static bool solve_treewidth_fourier_mod_once(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, uint64_t prime, const uint64_t *powers,
    const uint64_t *inv_powers, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || order == NULL || counts == NULL) {
    set_error(error, "internal error: null treewidth Fourier solve argument");
    return false;
  }
  if (max_bag_vars == 0 && qsop->nvars != 0) {
    set_error(error,
              "treewidth backend refuses non-constant instances with --max-vars 0; pass a larger "
              "--max-vars");
    return false;
  }

  tw_context_t ctx = {
      .r = qsop->r,
      .max_bag_vars = max_bag_vars,
      .count_modulus = prime,
      .stats = stats,
      .trace = trace,
  };

  if (stats != NULL) {
    stats->decomposition_width = order_width;
  }

  tw_factor_list_t factors = {0};
  const uint64_t factors_start = qsop_trace_begin(trace);
  if (!build_initial_factors_fourier(qsop, powers, &factors, error)) {
    factor_list_free(&factors);
    return false;
  }
  qsop_trace_emit_elapsed(trace, "treewidth.fourier_initial_factors", 0, factors.len,
                          factors_start);

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    if (!eliminate_variable_fourier(&factors, order[pos], &ctx, error)) {
      factor_list_free(&factors);
      return false;
    }
  }

  tw_factor_t final = {0};
  if (!multiply_remaining_factors_fourier(&factors, &ctx, &final, error)) {
    factor_list_free(&factors);
    return false;
  }
  if (final.arity != 0) {
    factor_list_free(&factors);
    factor_free(&final);
    set_error(error, "internal error: treewidth Fourier solve left an uneliminated factor");
    return false;
  }
  const bool ok = qsop_fourier_inverse_counts(qsop->r, final.counts, qsop->constant, powers,
                                              inv_powers, prime, counts, error);

  factor_list_free(&factors);
  factor_free(&final);
  return ok;
}

static bool solve_treewidth_fourier_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                         const uint32_t *order, uint32_t order_width,
                                         qsop_result_t **out, qsop_solve_stats_t *stats,
                                         qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_fourier_find_ntt_primes_for_nvars(qsop->r, qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "treewidth Fourier CRT count table is too large");
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
    set_error(error, "out of memory while allocating treewidth Fourier CRT solve state");
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
      set_error(error, "out of memory while allocating treewidth Fourier CRT result strings");
      return false;
    }
  }

  for (size_t p = 0; p < nprimes; p++) {
    uint64_t *powers = NULL;
    uint64_t *inv_powers = NULL;
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    const bool ok =
        treewidth_fourier_prime_state(qsop->r, primes[p], &powers, &inv_powers, error) &&
        solve_treewidth_fourier_mod_once(
            qsop, max_bag_vars, order, order_width, primes[p], powers, inv_powers,
            &all_counts[p * (size_t)qsop->r], stats_for_prime, trace_for_prime, error);
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

static bool solve_treewidth_order_policy_once(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order_policy,
    uint64_t count_modulus, uint64_t *counts, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint32_t *order = NULL;
  uint32_t width = 0;
  const uint64_t order_start = qsop_trace_begin(trace);
  if (!qsop_treewidth_order_alloc(qsop, order_policy, &order, &width, error)) {
    return false;
  }
  qsop_trace_emit_elapsed(trace, treewidth_order_trace_phase(order_policy), width, qsop->nvars,
                          order_start);
  const bool ok = solve_treewidth_once(qsop, max_bag_vars, order, width, count_modulus, counts,
                                       stats, trace, error);
  free(order);
  return ok;
}

static bool solve_treewidth_fourier_order_policy_once(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order_policy,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  uint32_t *order = NULL;
  uint32_t width = 0;
  const uint64_t order_start = qsop_trace_begin(trace);
  if (!qsop_treewidth_order_alloc(qsop, order_policy, &order, &width, error)) {
    return false;
  }
  qsop_trace_emit_elapsed(trace, treewidth_order_trace_phase(order_policy), width, qsop->nvars,
                          order_start);
  const bool ok =
      solve_treewidth_fourier_once(qsop, max_bag_vars, order, width, out, stats, trace, error);
  free(order);
  return ok;
}

static bool solve_treewidth_crt(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                qsop_treewidth_order_t order_policy, qsop_result_t **out,
                                qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "treewidth CRT count table is too large");
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
    set_error(error, "out of memory while allocating treewidth CRT solve state");
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
    set_error(error, "out of memory while allocating treewidth CRT result strings");
    return false;
  }

  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!solve_treewidth_order_policy_once(qsop, max_bag_vars, order_policy, primes[p],
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

static bool solve_treewidth_precomputed_crt(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                            const uint32_t *order, uint32_t order_width,
                                            qsop_result_t **out, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (qsop->r == 0 ? 1U : (size_t)qsop->r) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "treewidth CRT count table is too large");
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
    set_error(error, "out of memory while allocating treewidth CRT solve state");
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
    set_error(error, "out of memory while allocating treewidth CRT result strings");
    return false;
  }

  for (size_t p = 0; p < nprimes; p++) {
    qsop_solve_stats_t *stats_for_prime = p == 0 ? stats : NULL;
    qsop_solve_trace_t *trace_for_prime = p == 0 ? trace : NULL;
    if (!solve_treewidth_once(qsop, max_bag_vars, order, order_width, primes[p],
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

bool qsop_solve_treewidth(const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_result_t **out,
                          qsop_error_t *error) {
  return qsop_solve_treewidth_stats(qsop, max_bag_vars, out, NULL, error);
}

bool qsop_solve_treewidth_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                qsop_result_t **out, qsop_solve_stats_t *stats,
                                qsop_error_t *error) {
  return qsop_solve_treewidth_trace_stats(qsop, max_bag_vars, out, stats, NULL, error);
}

bool qsop_solve_treewidth_trace_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                      qsop_result_t **out, qsop_solve_stats_t *stats,
                                      qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_treewidth_mode_trace_stats(qsop, max_bag_vars, QSOP_SOLVE_MODE_COUNT_TABLE,
                                               out, stats, trace, error);
}

bool qsop_solve_treewidth_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_solve_mode_t mode,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  return qsop_solve_treewidth_order_mode_trace_stats(
      qsop, max_bag_vars, QSOP_TREEWIDTH_ORDER_MIN_FILL, mode, out, stats, trace, error);
}

bool qsop_treewidth_order_alloc(const qsop_instance_t *qsop, qsop_treewidth_order_t order_policy,
                                uint32_t **order_out, uint32_t *width_out,
                                qsop_error_t *error) {
  if (qsop == NULL || order_out == NULL || width_out == NULL) {
    set_error(error, "internal error: null treewidth order argument");
    return false;
  }
  *order_out = NULL;
  *width_out = 0;

  uint32_t *order = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*order));
  if (order == NULL) {
    set_error(error, "out of memory while allocating treewidth order");
    return false;
  }

  uint32_t width = 0;
  if (!make_treewidth_order(qsop, order_policy, order, &width, error)) {
    free(order);
    return false;
  }

  *order_out = order;
  *width_out = width;
  return true;
}

bool qsop_solve_treewidth_precomputed_order_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_treewidth_precomputed_order_mode_trace_stats(
      qsop, max_bag_vars, order, order_width, QSOP_SOLVE_MODE_COUNT_TABLE, out, stats, trace,
      error);
}

bool qsop_solve_treewidth_precomputed_order_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, qsop_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL || order == NULL) {
    set_error(error, "internal error: null treewidth precomputed-order solve argument");
    return false;
  }
  if (mode != QSOP_SOLVE_MODE_COUNT_TABLE && mode != QSOP_SOLVE_MODE_FOURIER) {
    set_error(error, "internal error: unsupported treewidth solve mode");
    return false;
  }
  if (qsop->nvars >= 64U && mode == QSOP_SOLVE_MODE_COUNT_TABLE) {
    return solve_treewidth_precomputed_crt(qsop, max_bag_vars, order, order_width, out, stats,
                                           trace, error);
  }
  if (mode == QSOP_SOLVE_MODE_FOURIER) {
    return solve_treewidth_fourier_once(qsop, max_bag_vars, order, order_width, out, stats, trace,
                                        error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    set_error(error, "out of memory while allocating treewidth result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    return false;
  }
  const bool ok = solve_treewidth_once(qsop, max_bag_vars, order, order_width, 0, result->counts,
                                       stats, trace, error);
  if (!ok) {
    qsop_result_free(result);
    return false;
  }
  *out = result;
  return true;
}

bool qsop_solve_treewidth_order_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order_policy,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  return qsop_solve_treewidth_order_mode_trace_stats(
      qsop, max_bag_vars, order_policy, QSOP_SOLVE_MODE_COUNT_TABLE, out, stats, trace, error);
}

bool qsop_solve_treewidth_order_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order_policy,
    qsop_solve_mode_t mode, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (mode != QSOP_SOLVE_MODE_COUNT_TABLE && mode != QSOP_SOLVE_MODE_FOURIER) {
    set_error(error, "internal error: unsupported treewidth solve mode");
    return false;
  }
  if (qsop->nvars >= 64U && mode == QSOP_SOLVE_MODE_COUNT_TABLE) {
    return solve_treewidth_crt(qsop, max_bag_vars, order_policy, out, stats, trace, error);
  }
  if (mode == QSOP_SOLVE_MODE_FOURIER) {
    return solve_treewidth_fourier_order_policy_once(qsop, max_bag_vars, order_policy, out, stats,
                                                     trace, error);
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    set_error(error, "out of memory while allocating treewidth result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    return false;
  }
  const bool ok = solve_treewidth_order_policy_once(qsop, max_bag_vars, order_policy, 0,
                                                    result->counts, stats, trace, error);
  if (!ok) {
    qsop_result_free(result);
    return false;
  }
  *out = result;
  return true;
}

bool qsop_solve_treewidth_precomputed_order_count_mod_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order,
    uint32_t order_width, uint64_t count_modulus, uint64_t *counts,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (qsop == NULL || order == NULL || counts == NULL) {
    set_error(error, "internal error: null treewidth precomputed-order modular solve argument");
    return false;
  }
  return solve_treewidth_once(qsop, max_bag_vars, order, order_width, count_modulus, counts,
                              stats, trace, error);
}
