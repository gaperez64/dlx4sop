#include "dlx4sop/bitset.h"
#include "dlx4sop/min_fill.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "dlx4sop/simd.h"
#include "trace.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Long-double pi, matching the precision (and literal) already used for amplitude
 * reconstruction in src/cli/sop_solve.c's write_probability_stats. */
static const long double tw_two_pi =
    6.283185307179586476925286766559005768394338798750211641949889L;

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

/* Single Fourier-mode factor: one complex accumulator per bag assignment, with no
 * dependence on r in table size (unlike tw_factor_t, which holds r-many residue/mode
 * slots per assignment). This is what makes the single-amplitude evaluation r-independent
 * (Corollary 1), instead of the O(r) per-node cost of count-table/all-modes-Fourier mode. */
typedef struct tw_factor_complex {
  uint32_t arity;
  uint32_t *vars;
  size_t assignments;
  long double *re;
  long double *im;
} tw_factor_complex_t;

typedef struct tw_factor_complex64 {
  uint32_t arity;
  uint32_t *vars;
  size_t assignments;
  double *re;
  double *im;
} tw_factor_complex64_t;

typedef struct tw_factor_complex_list {
  tw_factor_complex_t *items;
  size_t len;
  size_t cap;
} tw_factor_complex_list_t;

typedef struct tw_factor_complex64_list {
  tw_factor_complex64_t *items;
  size_t len;
  size_t cap;
} tw_factor_complex64_list_t;

/* Context for the single-mode solve: r and target_mode are needed to evaluate
 * omega_r^{target_mode * k} at leaves; complex_ops counts total per-assignment complex
 * multiply-accumulate operations performed, the basis for the certified numerical error
 * bound (see solve_treewidth_single_mode_once). */
typedef struct tw_complex_context {
  uint64_t r;
  uint32_t target_mode;
  uint32_t max_bag_vars;
  uint64_t complex_ops;
  /* See tw_complex64_context's scale_exp2: long double buys ~2^16384 of headroom instead of
   * ~2^1024, which an amplitude still runs out of past ~16k variables. */
  int scale_exp2;
  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
} tw_complex_context_t;

/* scale_exp2 is the running binary exponent factored out of the f64 tables. The unnormalized
 * amplitude of an n-variable instance grows like 2^n, so a plain double table overflows to inf --
 * and then inf*0 to nan -- somewhere past n ~ 1024. Each message is renormalized to a max entry in
 * [1,2) and the discarded power of two is accumulated here, making the DP immune to the instance
 * size. Scaling by a power of two is exact in binary floating point, so the mantissas the DP
 * computes are bit-for-bit what the unscaled DP would have computed, when it did not overflow. */
typedef struct tw_complex64_context {
  uint64_t r;
  uint32_t target_mode;
  uint32_t max_bag_vars;
  uint64_t complex_ops;
  int scale_exp2;
  const qsop_simd_vtable_t *simd;
  qsop_solve_stats_t *stats;
  qsop_solve_trace_t *trace;
} tw_complex64_context_t;

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

/* Count-table / all-modes-Fourier paths below allocate O(r) or O(r^2) structures per bag, so
 * their entry points refuse a modulus that would not fit uint32_t rather than silently
 * truncating it; the single-mode path (tw_complex_context_t below) has no such limit. */
static bool treewidth_refuse_large_modulus(const qsop_instance_t *qsop, qsop_error_t *error) {
  if (qsop != NULL && qsop->r > UINT32_MAX) {
    set_error(error, "treewidth count-table/all-modes-Fourier solve refuses modulus > 2^32-1; use "
                     "--solve-mode single-fourier");
    return false;
  }
  return true;
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

static bool factor_alloc_scope(const uint32_t *vars, uint32_t arity, uint32_t r, tw_factor_t *out,
                               qsop_error_t *error) {
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

static bool scope_contains_var(const uint32_t *vars, uint32_t arity, uint32_t var) {
  for (uint32_t i = 0; i < arity; i++) {
    if (vars[i] == var) {
      return true;
    }
  }
  return false;
}

static uint32_t scope_var_pos(const uint32_t *vars, uint32_t arity, uint32_t var) {
  for (uint32_t i = 0; i < arity; i++) {
    if (vars[i] == var) {
      return i;
    }
  }
  return UINT32_MAX;
}

static bool factor_contains_var(const tw_factor_t *factor, uint32_t var) {
  return scope_contains_var(factor->vars, factor->arity, var);
}

static uint32_t factor_var_pos(const tw_factor_t *factor, uint32_t var) {
  return scope_var_pos(factor->vars, factor->arity, var);
}

static bool make_scope_union(const uint32_t *left_vars, uint32_t left_arity,
                             const uint32_t *right_vars, uint32_t right_arity, uint32_t **vars_out,
                             uint32_t *arity_out, qsop_error_t *error) {
  uint32_t arity = 0;
  uint32_t *vars = malloc(((size_t)left_arity + right_arity + 1U) * sizeof(*vars));
  if (vars == NULL) {
    set_error(error, "out of memory while building treewidth factor scope");
    return false;
  }

  uint32_t i = 0;
  uint32_t j = 0;
  while (i < left_arity || j < right_arity) {
    if (j == right_arity || (i < left_arity && left_vars[i] < right_vars[j])) {
      vars[arity++] = left_vars[i++];
    } else if (i == left_arity || right_vars[j] < left_vars[i]) {
      vars[arity++] = right_vars[j++];
    } else {
      vars[arity++] = left_vars[i];
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
  const uint64_t entries = factor->assignments > UINT64_MAX / (ctx->r == 0 ? 1U : (uint64_t)ctx->r)
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
  if (!make_scope_union(left->vars, left->arity, right->vars, right->arity, &vars, &arity, error)) {
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

  uint32_t *left_positions =
      malloc((left->arity == 0 ? 1U : left->arity) * sizeof(*left_positions));
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
  if (!make_scope_union(left->vars, left->arity, right->vars, right->arity, &vars, &arity, error)) {
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

  uint32_t *left_positions =
      malloc((left->arity == 0 ? 1U : left->arity) * sizeof(*left_positions));
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
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.fourier_multiply", arity, out->assignments, start);
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

static bool factor_sum_out_fourier(const tw_factor_t *input, uint32_t var, const tw_context_t *ctx,
                                   tw_factor_t *out, qsop_error_t *error) {
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
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.fourier_sum_out", arity, out->assignments, start);
  return true;
}

/* Long double twin of factor_complex64_renormalize. */
static void factor_complex_renormalize(tw_factor_complex_t *factor, tw_complex_context_t *ctx) {
  long double peak = 0.0L;
  for (size_t i = 0; i < factor->assignments; i++) {
    const long double re = fabsl(factor->re[i]);
    const long double im = fabsl(factor->im[i]);
    if (re > peak) {
      peak = re;
    }
    if (im > peak) {
      peak = im;
    }
  }
  if (peak == 0.0L || !isfinite(peak)) {
    return;
  }
  const int exponent = ilogbl(peak);
  if (exponent == 0) {
    return;
  }
  const long double scale = ldexpl(1.0L, -exponent);
  for (size_t i = 0; i < factor->assignments; i++) {
    factor->re[i] *= scale;
    factor->im[i] *= scale;
  }
  ctx->scale_exp2 += exponent;
}

static void factor_complex_free(tw_factor_complex_t *factor) {
  if (factor == NULL) {
    return;
  }
  free(factor->vars);
  free(factor->re);
  free(factor->im);
  *factor = (tw_factor_complex_t){0};
}

static bool factor_complex_alloc_scope(const uint32_t *vars, uint32_t arity,
                                       tw_factor_complex_t *out, qsop_error_t *error) {
  *out = (tw_factor_complex_t){0};

  size_t assignments = 0;
  if (!checked_assignment_count(arity, &assignments, error)) {
    return false;
  }
  if (assignments > SIZE_MAX / sizeof(long double)) {
    set_error(error, "treewidth single-mode factor table is too large");
    return false;
  }

  /* malloc, not calloc: every caller either fully overwrites every element itself (the
   * factor_multiply_complex hot loop chief among them) or explicitly zeroes what it needs
   * (factor_sum_out_complex's output, which accumulates with +=) -- zero-initializing here
   * would just be redundant work on top of that. */
  uint32_t *scope = malloc((arity == 0 ? 1U : (size_t)arity) * sizeof(*scope));
  long double *re = malloc(assignments * sizeof(*re));
  long double *im = malloc(assignments * sizeof(*im));
  if (scope == NULL || re == NULL || im == NULL) {
    free(scope);
    free(re);
    free(im);
    set_error(error, "out of memory while allocating treewidth single-mode factor");
    return false;
  }
  if (arity != 0) {
    memcpy(scope, vars, (size_t)arity * sizeof(*scope));
  }

  *out = (tw_factor_complex_t){
      .arity = arity,
      .vars = scope,
      .assignments = assignments,
      .re = re,
      .im = im,
  };
  return true;
}

static bool factor_complex_identity(tw_factor_complex_t *out, qsop_error_t *error) {
  if (!factor_complex_alloc_scope(NULL, 0, out, error)) {
    return false;
  }
  out->re[0] = 1.0L;
  out->im[0] = 0.0L;
  return true;
}

/* omega_r^{target_mode * k}, computed directly from a scalar angle: r never sizes an
 * array in this path, only a divisor here, so table cost is fully independent of r. */
static void tw_root_of_unity(uint64_t r, uint32_t target_mode, uint64_t k, long double *re,
                             long double *im) {
  const long double angle = tw_two_pi * (long double)target_mode * (long double)k / (long double)r;
  *re = cosl(angle);
  *im = sinl(angle);
}

static void note_complex_factor_table(const tw_complex_context_t *ctx,
                                      const tw_factor_complex_t *factor) {
  if (ctx->stats == NULL) {
    return;
  }
  const uint64_t entries = (uint64_t)factor->assignments;
  ctx->stats->table_entries = entries;
  if (entries > ctx->stats->max_table_entries) {
    ctx->stats->max_table_entries = entries;
  }
}

static bool factor_multiply_complex(const tw_factor_complex_t *left,
                                    const tw_factor_complex_t *right, tw_complex_context_t *ctx,
                                    tw_factor_complex_t *out, qsop_error_t *error) {
  uint32_t *vars = NULL;
  uint32_t arity = 0;
  if (!make_scope_union(left->vars, left->arity, right->vars, right->arity, &vars, &arity, error)) {
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

  uint32_t *left_positions =
      malloc((left->arity == 0 ? 1U : left->arity) * sizeof(*left_positions));
  uint32_t *right_positions =
      malloc((right->arity == 0 ? 1U : right->arity) * sizeof(*right_positions));
  if (left_positions == NULL || right_positions == NULL) {
    free(vars);
    free(left_positions);
    free(right_positions);
    set_error(error, "out of memory while multiplying treewidth single-mode factors");
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
  bool ok = factor_complex_alloc_scope(vars, arity, out, error);
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
    factor_complex_free(out);
    free(left_positions);
    free(right_positions);
    free(left_map);
    free(right_map);
    return false;
  }

  for (size_t assignment = 0; assignment < out->assignments; assignment++) {
    const size_t left_assignment = left_map[assignment];
    const size_t right_assignment = right_map[assignment];
    const long double lre = left->re[left_assignment];
    const long double lim = left->im[left_assignment];
    const long double rre = right->re[right_assignment];
    const long double rim = right->im[right_assignment];
    out->re[assignment] = lre * rre - lim * rim;
    out->im[assignment] = lre * rim + lim * rre;
    ctx->complex_ops++;
  }

  free(left_positions);
  free(right_positions);
  free(left_map);
  free(right_map);
  if (ctx->stats != NULL) {
    add_saturating_u64(&ctx->stats->join_pairs, (uint64_t)out->assignments);
  }
  note_complex_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.single_mode_multiply", arity, out->assignments,
                          start);
  return true;
}

static bool factor_sum_out_complex(const tw_factor_complex_t *input, uint32_t var,
                                   tw_complex_context_t *ctx, tw_factor_complex_t *out,
                                   qsop_error_t *error) {
  const uint32_t pos = scope_var_pos(input->vars, input->arity, var);
  if (pos == UINT32_MAX) {
    set_error(error, "internal error: treewidth factor does not contain eliminated variable");
    return false;
  }

  uint32_t *vars = malloc((input->arity == 0 ? 1U : (size_t)input->arity) * sizeof(*vars));
  if (vars == NULL) {
    set_error(error, "out of memory while projecting treewidth single-mode factor");
    return false;
  }
  uint32_t arity = 0;
  for (uint32_t i = 0; i < input->arity; i++) {
    if (i != pos) {
      vars[arity++] = input->vars[i];
    }
  }

  const uint64_t start = qsop_trace_begin(ctx->trace);
  bool ok = factor_complex_alloc_scope(vars, arity, out, error);
  free(vars);
  if (!ok) {
    return false;
  }
  const size_t stride = (size_t)1U << pos;
  const size_t block = stride << 1U;
  for (size_t base = 0; base < input->assignments; base += block) {
    const size_t out_base = base >> 1U;
    for (size_t off = 0; off < stride; off++) {
      const size_t lower = base + off;
      const size_t upper = lower + stride;
      const size_t projected = out_base + off;
      out->re[projected] = input->re[lower] + input->re[upper];
      out->im[projected] = input->im[lower] + input->im[upper];
      ctx->complex_ops += 2U;
    }
  }

  note_complex_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.single_mode_sum_out", arity, out->assignments,
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

static bool factor_complex_list_reserve(tw_factor_complex_list_t *list, size_t needed,
                                        qsop_error_t *error) {
  if (needed <= list->cap) {
    return true;
  }

  size_t new_cap = list->cap == 0 ? 16U : list->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "treewidth single-mode factor list is too large");
      return false;
    }
    new_cap *= 2U;
  }
  tw_factor_complex_t *items = realloc(list->items, new_cap * sizeof(*items));
  if (items == NULL) {
    set_error(error, "out of memory while growing treewidth single-mode factor list");
    return false;
  }
  for (size_t i = list->cap; i < new_cap; i++) {
    items[i] = (tw_factor_complex_t){0};
  }
  list->items = items;
  list->cap = new_cap;
  return true;
}

static bool factor_complex_list_push_take(tw_factor_complex_list_t *list,
                                          tw_factor_complex_t *factor, qsop_error_t *error) {
  if (!factor_complex_list_reserve(list, list->len + 1U, error)) {
    return false;
  }
  list->items[list->len++] = *factor;
  *factor = (tw_factor_complex_t){0};
  return true;
}

static void factor_complex_list_remove_at(tw_factor_complex_list_t *list, size_t index) {
  factor_complex_free(&list->items[index]);
  list->len--;
  if (index != list->len) {
    list->items[index] = list->items[list->len];
    list->items[list->len] = (tw_factor_complex_t){0};
  }
}

static tw_factor_complex_t factor_complex_list_take_at(tw_factor_complex_list_t *list,
                                                       size_t index) {
  tw_factor_complex_t taken = list->items[index];
  list->len--;
  if (index != list->len) {
    list->items[index] = list->items[list->len];
  }
  list->items[list->len] = (tw_factor_complex_t){0};
  return taken;
}

static void factor_complex_list_free(tw_factor_complex_list_t *list) {
  if (list == NULL) {
    return;
  }
  for (size_t i = 0; i < list->len; i++) {
    factor_complex_free(&list->items[i]);
  }
  free(list->items);
  *list = (tw_factor_complex_list_t){0};
}

static bool make_treewidth_order(const qsop_instance_t *qsop, qsop_treewidth_order_t order_policy,
                                 uint32_t *order, uint32_t *width_out, qsop_error_t *error) {
  /* Delegates to the shared sparse min-fill core (byte-identical selection to the old dense
   * bitset loop, per the qsop_treewidth_order_t tie-break). */
  return qsop_min_fill_eliminate(qsop->nvars, qsop->edge_u, qsop->edge_v, qsop->nedges,
                                 order_policy, UINT32_MAX, order, width_out, NULL, NULL, NULL,
                                 error);
}

/* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
 * count-table path, which allocates O(r) structures below. */
static bool append_unary_factor(const qsop_instance_t *qsop, uint32_t var, tw_factor_list_t *list,
                                qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  tw_factor_t factor = {0};
  if (!factor_alloc_scope(&var, 1, r32, &factor, error)) {
    return false;
  }
  factor_counts(&factor, r32, 0)[0] = 1;
  factor_counts(&factor, r32, 1)[qsop->unary[var] % r32] = 1;
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

/* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
 * all-modes-Fourier path, which allocates O(r^2) structures below. */
static bool append_unary_factor_fourier(const qsop_instance_t *qsop, uint32_t var,
                                        const uint64_t *powers, tw_factor_list_t *list,
                                        qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  tw_factor_t factor = {0};
  if (!factor_alloc_scope(&var, 1, r32, &factor, error)) {
    return false;
  }
  for (uint32_t mode = 0; mode < r32; mode++) {
    factor_counts(&factor, r32, 0)[mode] = 1;
    factor_counts(&factor, r32, 1)[mode] = powers[(size_t)mode * r32 + (qsop->unary[var] % r32)];
  }
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

/* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
 * count-table path, which allocates O(r) structures below. */
static bool append_edge_factor(const qsop_instance_t *qsop, uint32_t edge, tw_factor_list_t *list,
                               qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint32_t vars[2] = {qsop->edge_u[edge], qsop->edge_v[edge]};
  if (vars[0] > vars[1]) {
    const uint32_t tmp = vars[0];
    vars[0] = vars[1];
    vars[1] = tmp;
  }

  tw_factor_t factor = {0};
  if (!factor_alloc_scope(vars, 2, r32, &factor, error)) {
    return false;
  }
  for (size_t assignment = 0; assignment < factor.assignments; assignment++) {
    const bool left = (assignment & 1U) != 0;
    const bool right = (assignment & 2U) != 0;
    const uint32_t residue = left && right ? r32 / 2U : 0;
    factor_counts(&factor, r32, assignment)[residue] = 1;
  }
  if (!factor_list_push_take(list, &factor, error)) {
    factor_free(&factor);
    return false;
  }
  return true;
}

/* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
 * all-modes-Fourier path, which allocates O(r^2) structures below. */
static bool append_edge_factor_fourier(const qsop_instance_t *qsop, uint32_t edge,
                                       const uint64_t *powers, tw_factor_list_t *list,
                                       qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint32_t vars[2] = {qsop->edge_u[edge], qsop->edge_v[edge]};
  if (vars[0] > vars[1]) {
    const uint32_t tmp = vars[0];
    vars[0] = vars[1];
    vars[1] = tmp;
  }

  tw_factor_t factor = {0};
  if (!factor_alloc_scope(vars, 2, r32, &factor, error)) {
    return false;
  }
  for (size_t assignment = 0; assignment < factor.assignments; assignment++) {
    const bool left = (assignment & 1U) != 0;
    const bool right = (assignment & 2U) != 0;
    const uint32_t residue = left && right ? r32 / 2U : 0;
    for (uint32_t mode = 0; mode < r32; mode++) {
      factor_counts(&factor, r32, assignment)[mode] = powers[(size_t)mode * r32 + residue];
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

static bool append_unary_factor_complex(const qsop_instance_t *qsop, uint32_t var,
                                        uint32_t target_mode, tw_factor_complex_list_t *list,
                                        qsop_error_t *error) {
  tw_factor_complex_t factor = {0};
  if (!factor_complex_alloc_scope(&var, 1, &factor, error)) {
    return false;
  }
  factor.re[0] = 1.0L;
  factor.im[0] = 0.0L;
  tw_root_of_unity(qsop->r, target_mode, qsop->unary[var] % qsop->r, &factor.re[1], &factor.im[1]);
  if (!factor_complex_list_push_take(list, &factor, error)) {
    factor_complex_free(&factor);
    return false;
  }
  return true;
}

static bool append_edge_factor_complex(const qsop_instance_t *qsop, uint32_t edge,
                                       uint32_t target_mode, tw_factor_complex_list_t *list,
                                       qsop_error_t *error) {
  uint32_t vars[2] = {qsop->edge_u[edge], qsop->edge_v[edge]};
  if (vars[0] > vars[1]) {
    const uint32_t tmp = vars[0];
    vars[0] = vars[1];
    vars[1] = tmp;
  }

  tw_factor_complex_t factor = {0};
  if (!factor_complex_alloc_scope(vars, 2, &factor, error)) {
    return false;
  }
  /* omega_r^{target_mode * r/2} = (-1)^target_mode exactly (both are integers): compute the
   * sign directly instead of through cosl/sinl, so a mathematically exact +-1 does not pick
   * up avoidable floating-point rounding noise (every bit of it counts toward Delta_num). */
  const long double sign_re = (target_mode % 2U == 0U) ? 1.0L : -1.0L;
  for (size_t assignment = 0; assignment < factor.assignments; assignment++) {
    const bool left = (assignment & 1U) != 0;
    const bool right = (assignment & 2U) != 0;
    factor.re[assignment] = (left && right) ? sign_re : 1.0L;
    factor.im[assignment] = 0.0L;
  }
  if (!factor_complex_list_push_take(list, &factor, error)) {
    factor_complex_free(&factor);
    return false;
  }
  return true;
}

static bool build_initial_factors_complex(const qsop_instance_t *qsop, uint32_t target_mode,
                                          tw_factor_complex_list_t *list, qsop_error_t *error) {
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!append_unary_factor_complex(qsop, v, target_mode, list, error)) {
      return false;
    }
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (!append_edge_factor_complex(qsop, e, target_mode, list, error)) {
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

static bool eliminate_variable_complex(tw_factor_complex_list_t *list, uint32_t var,
                                       tw_complex_context_t *ctx, qsop_error_t *error) {
  tw_factor_complex_t combined = {0};
  bool collected = false;
  for (size_t i = 0; i < list->len;) {
    if (!scope_contains_var(list->items[i].vars, list->items[i].arity, var)) {
      i++;
      continue;
    }

    if (!collected) {
      combined = factor_complex_list_take_at(list, i);
      collected = true;
      continue;
    }

    tw_factor_complex_t next = {0};
    if (!factor_multiply_complex(&combined, &list->items[i], ctx, &next, error)) {
      factor_complex_free(&combined);
      return false;
    }
    factor_complex_free(&combined);
    combined = next;
    /* A variable can sit in arbitrarily many factors, so bound each partial product too. */
    factor_complex_renormalize(&combined, ctx);
    factor_complex_list_remove_at(list, i);
    collected = true;
  }

  if (!collected) {
    set_error(error, "internal error: treewidth elimination found no factor for variable");
    return false;
  }

  tw_factor_complex_t reduced = {0};
  if (!factor_sum_out_complex(&combined, var, ctx, &reduced, error)) {
    factor_complex_free(&combined);
    return false;
  }
  factor_complex_free(&combined);
  factor_complex_renormalize(&reduced, ctx);
  if (!factor_complex_list_push_take(list, &reduced, error)) {
    factor_complex_free(&reduced);
    return false;
  }
  return true;
}

static bool multiply_remaining_factors_complex(tw_factor_complex_list_t *list,
                                               tw_complex_context_t *ctx, tw_factor_complex_t *out,
                                               qsop_error_t *error) {
  if (list->len == 0) {
    return factor_complex_identity(out, error);
  }

  *out = factor_complex_list_take_at(list, 0);
  while (list->len != 0) {
    tw_factor_complex_t next = {0};
    if (!factor_multiply_complex(out, &list->items[0], ctx, &next, error)) {
      factor_complex_free(out);
      return false;
    }
    factor_complex_free(out);
    *out = next;
    /* One leftover factor per connected component; the product is as unbounded as the messages. */
    factor_complex_renormalize(out, ctx);
    factor_complex_list_remove_at(list, 0);
  }
  return true;
}

static void factor_complex64_free(tw_factor_complex64_t *factor) {
  if (factor == NULL) {
    return;
  }
  free(factor->vars);
  free(factor->re);
  free(factor->im);
  *factor = (tw_factor_complex64_t){0};
}

/* Divide `factor` by the largest power of two that leaves its biggest component in [1,2), and hand
 * that exponent to the context. A zero factor is left alone: the amplitude is then exactly zero and
 * no scaling can improve on that. Only the exponent moves, so no bit of the mantissas is lost. */
static void factor_complex64_renormalize(tw_factor_complex64_t *factor,
                                         tw_complex64_context_t *ctx) {
  double peak = 0.0;
  for (size_t i = 0; i < factor->assignments; i++) {
    const double re = fabs(factor->re[i]);
    const double im = fabs(factor->im[i]);
    if (re > peak) {
      peak = re;
    }
    if (im > peak) {
      peak = im;
    }
  }
  if (peak == 0.0 || !isfinite(peak)) {
    return;
  }

  const int exponent = ilogb(peak);
  if (exponent == 0) {
    return;
  }
  const double scale = ldexp(1.0, -exponent);
  for (size_t i = 0; i < factor->assignments; i++) {
    factor->re[i] *= scale;
    factor->im[i] *= scale;
  }
  ctx->scale_exp2 += exponent;
}

static bool factor_complex64_alloc_scope(const uint32_t *vars, uint32_t arity,
                                         tw_factor_complex64_t *out, qsop_error_t *error) {
  *out = (tw_factor_complex64_t){0};

  size_t assignments = 0;
  if (!checked_assignment_count(arity, &assignments, error)) {
    return false;
  }
  if (assignments > SIZE_MAX / sizeof(double)) {
    set_error(error, "treewidth double single-mode factor table is too large");
    return false;
  }

  uint32_t *scope = malloc((arity == 0 ? 1U : (size_t)arity) * sizeof(*scope));
  double *re = malloc(assignments * sizeof(*re));
  double *im = malloc(assignments * sizeof(*im));
  if (scope == NULL || re == NULL || im == NULL) {
    free(scope);
    free(re);
    free(im);
    set_error(error, "out of memory while allocating treewidth double single-mode factor");
    return false;
  }
  if (arity != 0) {
    memcpy(scope, vars, (size_t)arity * sizeof(*scope));
  }

  *out = (tw_factor_complex64_t){
      .arity = arity,
      .vars = scope,
      .assignments = assignments,
      .re = re,
      .im = im,
  };
  return true;
}

static bool factor_complex64_identity(tw_factor_complex64_t *out, qsop_error_t *error) {
  if (!factor_complex64_alloc_scope(NULL, 0, out, error)) {
    return false;
  }
  out->re[0] = 1.0;
  out->im[0] = 0.0;
  return true;
}

static void tw_root_of_unity_f64(uint64_t r, uint32_t target_mode, uint64_t k, double *re,
                                 double *im) {
  const double angle = (double)tw_two_pi * (double)target_mode * (double)k / (double)r;
  *re = cos(angle);
  *im = sin(angle);
}

static void note_complex64_factor_table(const tw_complex64_context_t *ctx,
                                        const tw_factor_complex64_t *factor) {
  if (ctx->stats == NULL) {
    return;
  }
  const uint64_t entries = (uint64_t)factor->assignments;
  ctx->stats->table_entries = entries;
  if (entries > ctx->stats->max_table_entries) {
    ctx->stats->max_table_entries = entries;
  }
}

static bool projection_map_is_identity(const size_t *map, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (map[i] != i) {
      return false;
    }
  }
  return true;
}

static bool factor_multiply_complex64(const tw_factor_complex64_t *left,
                                      const tw_factor_complex64_t *right,
                                      tw_complex64_context_t *ctx, tw_factor_complex64_t *out,
                                      qsop_error_t *error) {
  uint32_t *vars = NULL;
  uint32_t arity = 0;
  if (!make_scope_union(left->vars, left->arity, right->vars, right->arity, &vars, &arity, error)) {
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

  uint32_t *left_positions =
      malloc((left->arity == 0 ? 1U : left->arity) * sizeof(*left_positions));
  uint32_t *right_positions =
      malloc((right->arity == 0 ? 1U : right->arity) * sizeof(*right_positions));
  if (left_positions == NULL || right_positions == NULL) {
    free(vars);
    free(left_positions);
    free(right_positions);
    set_error(error, "out of memory while multiplying treewidth double single-mode factors");
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
  bool ok = factor_complex64_alloc_scope(vars, arity, out, error);
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
    factor_complex64_free(out);
    free(left_positions);
    free(right_positions);
    free(left_map);
    free(right_map);
    return false;
  }

  const bool identity_maps = projection_map_is_identity(left_map, out->assignments) &&
                             projection_map_is_identity(right_map, out->assignments);
  if (identity_maps && ctx->simd != NULL && ctx->simd->complex_mul_assign_f64 != NULL &&
      out->assignments >= 8U) {
    ctx->simd->complex_mul_assign_f64(out->re, out->im, left->re, left->im, right->re, right->im,
                                      out->assignments);
    add_saturating_u64(&ctx->complex_ops, (uint64_t)out->assignments);
    if (ctx->stats != NULL && strcmp(qsop_simd_kernel_name(ctx->simd), "scalar") != 0) {
      add_saturating_u64(&ctx->stats->simd_vectorized_ops, (uint64_t)out->assignments);
    }
  } else {
    for (size_t assignment = 0; assignment < out->assignments; assignment++) {
      const size_t left_assignment = left_map[assignment];
      const size_t right_assignment = right_map[assignment];
      const double lre = left->re[left_assignment];
      const double lim = left->im[left_assignment];
      const double rre = right->re[right_assignment];
      const double rim = right->im[right_assignment];
      out->re[assignment] = lre * rre - lim * rim;
      out->im[assignment] = lre * rim + lim * rre;
      ctx->complex_ops++;
    }
    if (ctx->stats != NULL) {
      add_saturating_u64(&ctx->stats->simd_scalar_fallback_ops, (uint64_t)out->assignments);
    }
  }

  free(left_positions);
  free(right_positions);
  free(left_map);
  free(right_map);
  if (ctx->stats != NULL) {
    add_saturating_u64(&ctx->stats->join_pairs, (uint64_t)out->assignments);
  }
  note_complex64_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.single_mode_multiply_f64", arity, out->assignments,
                          start);
  return true;
}

static bool factor_sum_out_complex64(const tw_factor_complex64_t *input, uint32_t var,
                                     tw_complex64_context_t *ctx, tw_factor_complex64_t *out,
                                     qsop_error_t *error) {
  const uint32_t pos = scope_var_pos(input->vars, input->arity, var);
  if (pos == UINT32_MAX) {
    set_error(error, "internal error: treewidth factor does not contain eliminated variable");
    return false;
  }

  uint32_t *vars = malloc((input->arity == 0 ? 1U : (size_t)input->arity) * sizeof(*vars));
  if (vars == NULL) {
    set_error(error, "out of memory while projecting treewidth double single-mode factor");
    return false;
  }
  uint32_t arity = 0;
  for (uint32_t i = 0; i < input->arity; i++) {
    if (i != pos) {
      vars[arity++] = input->vars[i];
    }
  }

  const uint64_t start = qsop_trace_begin(ctx->trace);
  bool ok = factor_complex64_alloc_scope(vars, arity, out, error);
  free(vars);
  if (!ok) {
    return false;
  }

  const size_t stride = (size_t)1U << pos;
  const size_t block = stride << 1U;
  for (size_t base = 0; base < input->assignments; base += block) {
    const size_t out_base = base >> 1U;
    if (ctx->simd != NULL && ctx->simd->complex_sum_out_pairs_f64 != NULL && stride >= 8U) {
      ctx->simd->complex_sum_out_pairs_f64(out->re + out_base, out->im + out_base, input->re + base,
                                           input->im + base, stride);
      if (ctx->stats != NULL && strcmp(qsop_simd_kernel_name(ctx->simd), "scalar") != 0) {
        add_saturating_u64(&ctx->stats->simd_vectorized_ops, (uint64_t)stride);
      }
    } else {
      for (size_t off = 0; off < stride; off++) {
        const size_t lower = base + off;
        const size_t upper = lower + stride;
        const size_t projected = out_base + off;
        out->re[projected] = input->re[lower] + input->re[upper];
        out->im[projected] = input->im[lower] + input->im[upper];
      }
      if (ctx->stats != NULL) {
        add_saturating_u64(&ctx->stats->simd_scalar_fallback_ops, (uint64_t)stride);
      }
    }
    add_saturating_u64(&ctx->complex_ops, (uint64_t)(2U * stride));
  }

  note_complex64_factor_table(ctx, out);
  qsop_trace_emit_elapsed(ctx->trace, "treewidth.single_mode_sum_out_f64", arity, out->assignments,
                          start);
  return true;
}

static bool factor_complex64_list_reserve(tw_factor_complex64_list_t *list, size_t needed,
                                          qsop_error_t *error) {
  if (needed <= list->cap) {
    return true;
  }

  size_t new_cap = list->cap == 0 ? 16U : list->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "treewidth double single-mode factor list is too large");
      return false;
    }
    new_cap *= 2U;
  }
  tw_factor_complex64_t *items = realloc(list->items, new_cap * sizeof(*items));
  if (items == NULL) {
    set_error(error, "out of memory while growing treewidth double single-mode factor list");
    return false;
  }
  for (size_t i = list->cap; i < new_cap; i++) {
    items[i] = (tw_factor_complex64_t){0};
  }
  list->items = items;
  list->cap = new_cap;
  return true;
}

static bool factor_complex64_list_push_take(tw_factor_complex64_list_t *list,
                                            tw_factor_complex64_t *factor, qsop_error_t *error) {
  if (!factor_complex64_list_reserve(list, list->len + 1U, error)) {
    return false;
  }
  list->items[list->len++] = *factor;
  *factor = (tw_factor_complex64_t){0};
  return true;
}

static void factor_complex64_list_remove_at(tw_factor_complex64_list_t *list, size_t index) {
  factor_complex64_free(&list->items[index]);
  list->len--;
  if (index != list->len) {
    list->items[index] = list->items[list->len];
    list->items[list->len] = (tw_factor_complex64_t){0};
  }
}

static tw_factor_complex64_t factor_complex64_list_take_at(tw_factor_complex64_list_t *list,
                                                           size_t index) {
  tw_factor_complex64_t taken = list->items[index];
  list->len--;
  if (index != list->len) {
    list->items[index] = list->items[list->len];
  }
  list->items[list->len] = (tw_factor_complex64_t){0};
  return taken;
}

static void factor_complex64_list_free(tw_factor_complex64_list_t *list) {
  if (list == NULL) {
    return;
  }
  for (size_t i = 0; i < list->len; i++) {
    factor_complex64_free(&list->items[i]);
  }
  free(list->items);
  *list = (tw_factor_complex64_list_t){0};
}

static bool append_unary_factor_complex64(const qsop_instance_t *qsop, uint32_t var,
                                          uint32_t target_mode, tw_factor_complex64_list_t *list,
                                          qsop_error_t *error) {
  tw_factor_complex64_t factor = {0};
  if (!factor_complex64_alloc_scope(&var, 1, &factor, error)) {
    return false;
  }
  factor.re[0] = 1.0;
  factor.im[0] = 0.0;
  tw_root_of_unity_f64(qsop->r, target_mode, qsop->unary[var] % qsop->r, &factor.re[1],
                       &factor.im[1]);
  if (!factor_complex64_list_push_take(list, &factor, error)) {
    factor_complex64_free(&factor);
    return false;
  }
  return true;
}

static bool append_edge_factor_complex64(const qsop_instance_t *qsop, uint32_t edge,
                                         uint32_t target_mode, tw_factor_complex64_list_t *list,
                                         qsop_error_t *error) {
  uint32_t vars[2] = {qsop->edge_u[edge], qsop->edge_v[edge]};
  if (vars[0] > vars[1]) {
    const uint32_t tmp = vars[0];
    vars[0] = vars[1];
    vars[1] = tmp;
  }

  tw_factor_complex64_t factor = {0};
  if (!factor_complex64_alloc_scope(vars, 2, &factor, error)) {
    return false;
  }
  const double sign_re = (target_mode % 2U == 0U) ? 1.0 : -1.0;
  for (size_t assignment = 0; assignment < factor.assignments; assignment++) {
    const bool left = (assignment & 1U) != 0;
    const bool right = (assignment & 2U) != 0;
    factor.re[assignment] = (left && right) ? sign_re : 1.0;
    factor.im[assignment] = 0.0;
  }
  if (!factor_complex64_list_push_take(list, &factor, error)) {
    factor_complex64_free(&factor);
    return false;
  }
  return true;
}

static bool build_initial_factors_complex64(const qsop_instance_t *qsop, uint32_t target_mode,
                                            tw_factor_complex64_list_t *list, qsop_error_t *error) {
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!append_unary_factor_complex64(qsop, v, target_mode, list, error)) {
      return false;
    }
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (!append_edge_factor_complex64(qsop, e, target_mode, list, error)) {
      return false;
    }
  }
  return true;
}

static bool eliminate_variable_complex64(tw_factor_complex64_list_t *list, uint32_t var,
                                         tw_complex64_context_t *ctx, qsop_error_t *error) {
  tw_factor_complex64_t combined = {0};
  bool collected = false;
  for (size_t i = 0; i < list->len;) {
    if (!scope_contains_var(list->items[i].vars, list->items[i].arity, var)) {
      i++;
      continue;
    }

    if (!collected) {
      combined = factor_complex64_list_take_at(list, i);
      collected = true;
      continue;
    }

    tw_factor_complex64_t next = {0};
    if (!factor_multiply_complex64(&combined, &list->items[i], ctx, &next, error)) {
      factor_complex64_free(&combined);
      return false;
    }
    factor_complex64_free(&combined);
    combined = next;
    /* Renormalize every pairwise product, not just the finished message: a variable can be in
     * arbitrarily many factors (a 2000-leaf star leaves 2000 messages on its centre), and each
     * factor being under 2 only bounds their product by 2^k. */
    factor_complex64_renormalize(&combined, ctx);
    factor_complex64_list_remove_at(list, i);
    collected = true;
  }

  if (!collected) {
    set_error(error, "internal error: treewidth elimination found no factor for variable");
    return false;
  }

  tw_factor_complex64_t reduced = {0};
  if (!factor_sum_out_complex64(&combined, var, ctx, &reduced, error)) {
    factor_complex64_free(&combined);
    return false;
  }
  factor_complex64_free(&combined);
  /* Every message leaves this function with a peak in [1,2), so the product formed above can only
   * reach 2^(bag size) before the next renormalization -- comfortably inside double's range
   * whatever the instance size. */
  factor_complex64_renormalize(&reduced, ctx);
  if (!factor_complex64_list_push_take(list, &reduced, error)) {
    factor_complex64_free(&reduced);
    return false;
  }
  return true;
}

static bool multiply_remaining_factors_complex64(tw_factor_complex64_list_t *list,
                                                 tw_complex64_context_t *ctx,
                                                 tw_factor_complex64_t *out, qsop_error_t *error) {
  if (list->len == 0) {
    return factor_complex64_identity(out, error);
  }

  *out = factor_complex64_list_take_at(list, 0);
  while (list->len != 0) {
    tw_factor_complex64_t next = {0};
    if (!factor_multiply_complex64(out, &list->items[0], ctx, &next, error)) {
      factor_complex64_free(out);
      return false;
    }
    factor_complex64_free(out);
    *out = next;
    /* One leftover factor per connected component, so this product is as unbounded as the
     * elimination messages were. */
    factor_complex64_renormalize(out, ctx);
    factor_complex64_list_remove_at(list, 0);
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

  /* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
   * count-table path, which allocates O(r) structures below. */
  const uint32_t r32 = (uint32_t)qsop->r;
  tw_context_t ctx = {
      .r = r32,
      .max_bag_vars = max_bag_vars,
      .count_modulus = count_modulus,
      .stats = stats,
      .trace = trace,
  };

  if (stats != NULL) {
    stats->decomposition_width = order_width;
    stats->treewidth_single_complex_kernel = 1U;
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
  if (!shift_result_counts(r32, counts, final.counts, (uint32_t)qsop->constant, &ctx, error)) {
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

static bool solve_treewidth_fourier_mod_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                             const uint32_t *order, uint32_t order_width,
                                             uint64_t prime, const uint64_t *powers,
                                             const uint64_t *inv_powers, uint64_t *counts,
                                             qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                             qsop_error_t *error) {
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

  /* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
   * all-modes-Fourier path, which allocates O(r^2) structures below. */
  const uint32_t r32 = (uint32_t)qsop->r;
  tw_context_t ctx = {
      .r = r32,
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
  const bool ok = qsop_fourier_inverse_counts(r32, final.counts, (uint32_t)qsop->constant, powers,
                                              inv_powers, prime, counts, error);

  factor_list_free(&factors);
  factor_free(&final);
  return ok;
}

static bool solve_treewidth_fourier_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                         const uint32_t *order, uint32_t order_width,
                                         qsop_result_t **out, qsop_solve_stats_t *stats,
                                         qsop_solve_trace_t *trace, qsop_error_t *error) {
  /* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
   * all-modes-Fourier path, which allocates O(r^2) structures below. */
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_fourier_find_ntt_primes_for_nvars(r32, qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "treewidth Fourier CRT count table is too large");
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  uint64_t *all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
  uint64_t *residues = calloc(nprimes == 0 ? 1U : nprimes, sizeof(*residues));
  if (result == NULL || all_counts == NULL || residues == NULL) {
    free(primes);
    free(all_counts);
    free(residues);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating treewidth Fourier CRT solve state");
    return false;
  }
  result->r = r32;
  result->norm_h = qsop->norm_h;
  if (nprimes == 1U) {
    if (!qsop_counts_alloc(r32, &result->counts, error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  } else {
    result->count_strings = calloc(r32, sizeof(*result->count_strings));
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
    const bool ok = treewidth_fourier_prime_state(r32, primes[p], &powers, &inv_powers, error) &&
                    solve_treewidth_fourier_mod_once(
                        qsop, max_bag_vars, order, order_width, primes[p], powers, inv_powers,
                        &all_counts[p * (size_t)r32], stats_for_prime, trace_for_prime, error);
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
    memcpy(result->counts, all_counts, (size_t)r32 * sizeof(*result->counts));
  } else {
    for (uint32_t residue = 0; residue < r32; residue++) {
      for (size_t p = 0; p < nprimes; p++) {
        residues[p] = all_counts[p * (size_t)r32 + residue];
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
  }

  free(primes);
  free(all_counts);
  free(residues);
  *out = result;
  return true;
}

static bool solve_treewidth_order_policy_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                              qsop_treewidth_order_t order_policy,
                                              uint64_t count_modulus, uint64_t *counts,
                                              qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                              qsop_error_t *error) {
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

static bool
solve_treewidth_fourier_order_policy_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                          qsop_treewidth_order_t order_policy, qsop_result_t **out,
                                          qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
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

/* Conservative worst-case bound on floating-point error accumulated by the single-mode
 * complex DP, following standard backward-error analysis for long double (80-bit extended,
 * unit roundoff ~5.42e-20) arithmetic: each complex multiply-accumulate or complex add
 * counted in complex_ops contributes at most a small constant multiple of the unit roundoff
 * to the (unnormalized) result's error. This bound is intentionally not tight -- it is
 * validated empirically against exact histogram reconstruction in the differential tests. */
static long double single_mode_error_bound(uint64_t complex_ops) {
  static const long double ops_per_step =
      8.0L; /* complex multiply: 4 mul + 2 add/sub, plus margin */
  return (long double)complex_ops * ops_per_step * LDBL_EPSILON;
}

static long double single_mode_error_bound_f64(uint64_t complex_ops) {
  static const long double ops_per_step = 8.0L;
  return (long double)complex_ops * ops_per_step * DBL_EPSILON;
}

static bool solve_treewidth_single_mode_once(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                             const uint32_t *order, uint32_t order_width,
                                             uint32_t target_mode, int *out_scale_exp2,
                                             long double *out_re, long double *out_im,
                                             long double *out_numeric_error_bound,
                                             qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                             qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || order == NULL || out_re == NULL || out_im == NULL) {
    set_error(error, "internal error: null treewidth single-mode solve argument");
    return false;
  }
  if (max_bag_vars == 0 && qsop->nvars != 0) {
    set_error(error,
              "treewidth backend refuses non-constant instances with --max-vars 0; pass a larger "
              "--max-vars");
    return false;
  }

  tw_complex_context_t ctx = {
      .r = qsop->r,
      .target_mode = target_mode,
      .max_bag_vars = max_bag_vars,
      .stats = stats,
      .trace = trace,
  };

  if (stats != NULL) {
    stats->decomposition_width = order_width;
  }

  tw_factor_complex_list_t factors = {0};
  const uint64_t factors_start = qsop_trace_begin(trace);
  if (!build_initial_factors_complex(qsop, target_mode, &factors, error)) {
    factor_complex_list_free(&factors);
    return false;
  }
  qsop_trace_emit_elapsed(trace, "treewidth.single_mode_initial_factors", 0, factors.len,
                          factors_start);

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    if (!eliminate_variable_complex(&factors, order[pos], &ctx, error)) {
      factor_complex_list_free(&factors);
      return false;
    }
  }

  tw_factor_complex_t final = {0};
  if (!multiply_remaining_factors_complex(&factors, &ctx, &final, error)) {
    factor_complex_list_free(&factors);
    return false;
  }
  if (final.arity != 0) {
    factor_complex_list_free(&factors);
    factor_complex_free(&final);
    set_error(error, "internal error: treewidth single-mode solve left an uneliminated factor");
    return false;
  }

  /* Fold in the constant term c: the DP so far has accumulated
     sum_x omega_r^{target_mode * (sum_v b_v x_v + edges(x))}; the true SOP value multiplies
     this by omega_r^{target_mode * c}. */
  long double c_re = 0.0L;
  long double c_im = 0.0L;
  tw_root_of_unity(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
  const long double result_re = final.re[0] * c_re - final.im[0] * c_im;
  const long double result_im = final.re[0] * c_im + final.im[0] * c_re;

  factor_complex_list_free(&factors);
  factor_complex_free(&final);

  *out_re = result_re;
  *out_im = result_im;
  *out_scale_exp2 = ctx.scale_exp2;
  if (out_numeric_error_bound != NULL) {
    *out_numeric_error_bound = single_mode_error_bound(ctx.complex_ops);
  }
  return true;
}

/* Emits a mantissa and a separate binary exponent rather than one number: the DP runs on f64 tables
 * (so the SIMD kernels apply) and never lets a table's magnitude drift, and the caller folds the
 * exponent into what it needs -- usually the normalized amplitude, which is bounded by 1. That
 * clears double's ~1.8e308 ceiling, which the raw amplitude of any instance past ~1024 variables
 * sails straight through, and long double's ~2^16384 ceiling past ~16k variables. */
static bool solve_treewidth_single_mode_once_f64(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order, uint32_t order_width,
    uint32_t target_mode, const qsop_simd_vtable_t *simd, int *out_scale_exp2, long double *out_re,
    long double *out_im, long double *out_numeric_error_bound, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || order == NULL || out_re == NULL || out_im == NULL) {
    set_error(error, "internal error: null treewidth double single-mode solve argument");
    return false;
  }
  if (max_bag_vars == 0 && qsop->nvars != 0) {
    set_error(error,
              "treewidth backend refuses non-constant instances with --max-vars 0; pass a larger "
              "--max-vars");
    return false;
  }

  tw_complex64_context_t ctx = {
      .r = qsop->r,
      .target_mode = target_mode,
      .max_bag_vars = max_bag_vars,
      .simd = simd,
      .stats = stats,
      .trace = trace,
  };

  if (stats != NULL) {
    stats->decomposition_width = order_width;
    stats->treewidth_single_complex_kernel = 2U;
  }

  tw_factor_complex64_list_t factors = {0};
  const uint64_t factors_start = qsop_trace_begin(trace);
  if (!build_initial_factors_complex64(qsop, target_mode, &factors, error)) {
    factor_complex64_list_free(&factors);
    return false;
  }
  qsop_trace_emit_elapsed(trace, "treewidth.single_mode_initial_factors_f64", 0, factors.len,
                          factors_start);

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    if (!eliminate_variable_complex64(&factors, order[pos], &ctx, error)) {
      factor_complex64_list_free(&factors);
      return false;
    }
  }

  tw_factor_complex64_t final = {0};
  if (!multiply_remaining_factors_complex64(&factors, &ctx, &final, error)) {
    factor_complex64_list_free(&factors);
    return false;
  }
  if (final.arity != 0) {
    factor_complex64_list_free(&factors);
    factor_complex64_free(&final);
    set_error(error,
              "internal error: treewidth double single-mode solve left an uneliminated factor");
    return false;
  }

  double c_re = 0.0;
  double c_im = 0.0;
  tw_root_of_unity_f64(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
  const double mantissa_re = final.re[0] * c_re - final.im[0] * c_im;
  const double mantissa_im = final.re[0] * c_im + final.im[0] * c_re;
  const int scale_exp2 = ctx.scale_exp2;

  factor_complex64_list_free(&factors);
  factor_complex64_free(&final);

  *out_re = (long double)mantissa_re;
  *out_im = (long double)mantissa_im;
  *out_scale_exp2 = scale_exp2;
  if (out_numeric_error_bound != NULL) {
    *out_numeric_error_bound = single_mode_error_bound_f64(ctx.complex_ops);
  }
  return true;
}

static bool solve_treewidth_single_mode_order_policy_once(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order_policy,
    uint32_t target_mode, int *out_scale_exp2, long double *out_re, long double *out_im,
    long double *out_numeric_error_bound, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  uint32_t *order = NULL;
  uint32_t width = 0;
  const uint64_t order_start = qsop_trace_begin(trace);
  if (!qsop_treewidth_order_alloc(qsop, order_policy, &order, &width, error)) {
    return false;
  }
  qsop_trace_emit_elapsed(trace, treewidth_order_trace_phase(order_policy), width, qsop->nvars,
                          order_start);
  const bool ok = solve_treewidth_single_mode_once(qsop, max_bag_vars, order, width, target_mode,
                                                   out_scale_exp2, out_re, out_im,
                                                   out_numeric_error_bound, stats, trace, error);
  free(order);
  return ok;
}

static bool solve_treewidth_single_mode_order_policy_once_f64(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, qsop_treewidth_order_t order_policy,
    uint32_t target_mode, const qsop_simd_vtable_t *simd, int *out_scale_exp2, long double *out_re,
    long double *out_im, long double *out_numeric_error_bound, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  uint32_t *order = NULL;
  uint32_t width = 0;
  const uint64_t order_start = qsop_trace_begin(trace);
  if (!qsop_treewidth_order_alloc(qsop, order_policy, &order, &width, error)) {
    return false;
  }
  qsop_trace_emit_elapsed(trace, treewidth_order_trace_phase(order_policy), width, qsop->nvars,
                          order_start);
  const bool ok = solve_treewidth_single_mode_once_f64(
      qsop, max_bag_vars, order, width, target_mode, simd, out_scale_exp2, out_re, out_im,
      out_numeric_error_bound, stats, trace, error);
  free(order);
  return ok;
}

/* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
 * count-table path, which allocates O(r) structures below. */
static bool solve_treewidth_crt(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                qsop_treewidth_order_t order_policy, qsop_result_t **out,
                                qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "treewidth CRT count table is too large");
    return false;
  }

  uint64_t *all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
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
  result->r = r32;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(r32, sizeof(*result->count_strings));
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
                                           &all_counts[p * (size_t)r32], stats_for_prime,
                                           trace_for_prime, error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t residue = 0; residue < r32; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)r32 + residue];
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

/* Gated by every public entry point above to qsop->r <= UINT32_MAX before reaching this
 * count-table path, which allocates O(r) structures below. */
static bool solve_treewidth_precomputed_crt(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                            const uint32_t *order, uint32_t order_width,
                                            qsop_result_t **out, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error) {
  const uint32_t r32 = (uint32_t)qsop->r;
  uint64_t *primes = NULL;
  size_t nprimes = 0;
  if (!qsop_crt_find_primes_for_nvars(qsop->nvars, &primes, &nprimes, error)) {
    return false;
  }
  if (nprimes > SIZE_MAX / (r32 == 0 ? 1U : (size_t)r32) / sizeof(uint64_t)) {
    free(primes);
    set_error(error, "treewidth CRT count table is too large");
    return false;
  }

  uint64_t *all_counts = calloc(nprimes * (size_t)r32, sizeof(*all_counts));
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
  result->r = r32;
  result->norm_h = qsop->norm_h;
  result->count_strings = calloc(r32, sizeof(*result->count_strings));
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
                              &all_counts[p * (size_t)r32], stats_for_prime, trace_for_prime,
                              error)) {
      free(primes);
      free(all_counts);
      free(residues);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t residue = 0; residue < r32; residue++) {
    for (size_t p = 0; p < nprimes; p++) {
      residues[p] = all_counts[p * (size_t)r32 + residue];
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
  return qsop_solve_treewidth_mode_trace_stats(qsop, max_bag_vars, QSOP_SOLVE_MODE_COUNT_TABLE, out,
                                               stats, trace, error);
}

bool qsop_solve_treewidth_mode_trace_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                           qsop_solve_mode_t mode, qsop_result_t **out,
                                           qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                           qsop_error_t *error) {
  return qsop_solve_treewidth_order_mode_trace_stats(
      qsop, max_bag_vars, QSOP_TREEWIDTH_ORDER_MIN_FILL, mode, out, stats, trace, error);
}

bool qsop_treewidth_order_alloc(const qsop_instance_t *qsop, qsop_treewidth_order_t order_policy,
                                uint32_t **order_out, uint32_t *width_out, qsop_error_t *error) {
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
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order, uint32_t order_width,
    qsop_result_t **out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  return qsop_solve_treewidth_precomputed_order_mode_trace_stats(
      qsop, max_bag_vars, order, order_width, QSOP_SOLVE_MODE_COUNT_TABLE, out, stats, trace,
      error);
}

bool qsop_solve_treewidth_precomputed_order_mode_trace_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order, uint32_t order_width,
    qsop_solve_mode_t mode, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
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
  if (!treewidth_refuse_large_modulus(qsop, error)) {
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
  result->r = (uint32_t)qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc((uint32_t)qsop->r, &result->counts, error)) {
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

bool qsop_solve_treewidth_order_trace_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                            qsop_treewidth_order_t order_policy,
                                            qsop_result_t **out, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_treewidth_order_mode_trace_stats(
      qsop, max_bag_vars, order_policy, QSOP_SOLVE_MODE_COUNT_TABLE, out, stats, trace, error);
}

bool qsop_solve_treewidth_order_mode_trace_stats(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                                 qsop_treewidth_order_t order_policy,
                                                 qsop_solve_mode_t mode, qsop_result_t **out,
                                                 qsop_solve_stats_t *stats,
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
  if (!treewidth_refuse_large_modulus(qsop, error)) {
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
  result->r = (uint32_t)qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc((uint32_t)qsop->r, &result->counts, error)) {
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

bool qsop_solve_treewidth_single_mode(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                      qsop_treewidth_order_t order_policy, uint32_t target_mode,
                                      qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                      qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->r == 0) {
    set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }

  long double re = 0.0L;
  long double im = 0.0L;
  long double numeric_error_bound = 0.0L;
  int scale_exp2 = 0;
  if (!solve_treewidth_single_mode_order_policy_once(qsop, max_bag_vars, order_policy, target_mode,
                                                     &scale_exp2, &re, &im, &numeric_error_bound,
                                                     stats, trace, error)) {
    return false;
  }
  out->re = re;
  out->im = im;
  out->scale_exp2 = scale_exp2;
  out->numeric_error_bound = numeric_error_bound;
  return true;
}

bool qsop_solve_treewidth_single_mode_f64(const qsop_instance_t *qsop, uint32_t max_bag_vars,
                                          qsop_treewidth_order_t order_policy, uint32_t target_mode,
                                          const qsop_simd_vtable_t *simd, qsop_amplitude_t *out,
                                          qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                          qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->r == 0) {
    set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }

  long double re = 0.0L;
  long double im = 0.0L;
  long double numeric_error_bound = 0.0L;
  int scale_exp2 = 0;
  if (!solve_treewidth_single_mode_order_policy_once_f64(
          qsop, max_bag_vars, order_policy, target_mode, simd, &scale_exp2, &re, &im,
          &numeric_error_bound, stats, trace, error)) {
    return false;
  }
  out->re = re;
  out->im = im;
  out->scale_exp2 = scale_exp2;
  out->numeric_error_bound = numeric_error_bound;
  return true;
}

bool qsop_solve_treewidth_precomputed_order_single_mode(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order, uint32_t order_width,
    uint32_t target_mode, qsop_amplitude_t *out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL || order == NULL) {
    set_error(error, "internal error: null treewidth precomputed-order single-mode solve argument");
    return false;
  }
  if (qsop->r == 0) {
    set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }

  long double re = 0.0L;
  long double im = 0.0L;
  long double numeric_error_bound = 0.0L;
  int scale_exp2 = 0;
  if (!solve_treewidth_single_mode_once(qsop, max_bag_vars, order, order_width, target_mode,
                                        &scale_exp2, &re, &im, &numeric_error_bound, stats, trace,
                                        error)) {
    return false;
  }
  out->re = re;
  out->im = im;
  out->scale_exp2 = scale_exp2;
  out->numeric_error_bound = numeric_error_bound;
  return true;
}

bool qsop_solve_treewidth_precomputed_order_single_mode_f64(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order, uint32_t order_width,
    uint32_t target_mode, const qsop_simd_vtable_t *simd, qsop_amplitude_t *out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL || order == NULL) {
    set_error(error,
              "internal error: null treewidth precomputed-order double single-mode solve argument");
    return false;
  }
  if (qsop->r == 0) {
    set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }

  long double re = 0.0L;
  long double im = 0.0L;
  long double numeric_error_bound = 0.0L;
  int scale_exp2 = 0;
  if (!solve_treewidth_single_mode_once_f64(qsop, max_bag_vars, order, order_width, target_mode,
                                            simd, &scale_exp2, &re, &im, &numeric_error_bound,
                                            stats, trace, error)) {
    return false;
  }
  out->re = re;
  out->im = im;
  out->scale_exp2 = scale_exp2;
  out->numeric_error_bound = numeric_error_bound;
  return true;
}

bool qsop_solve_treewidth_precomputed_order_count_mod_stats(
    const qsop_instance_t *qsop, uint32_t max_bag_vars, const uint32_t *order, uint32_t order_width,
    uint64_t count_modulus, uint64_t *counts, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  if (qsop == NULL || order == NULL || counts == NULL) {
    set_error(error, "internal error: null treewidth precomputed-order modular solve argument");
    return false;
  }
  if (!treewidth_refuse_large_modulus(qsop, error)) {
    return false;
  }
  return solve_treewidth_once(qsop, max_bag_vars, order, order_width, count_modulus, counts, stats,
                              trace, error);
}
