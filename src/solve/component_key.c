#include "component_key.h"

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

static void swap_u32(uint32_t *a, uint32_t *b) {
  const uint32_t tmp = *a;
  *a = *b;
  *b = tmp;
}

static bool component_edge_less(uint32_t au, uint32_t av, uint32_t bu, uint32_t bv) {
  if (au != bu) {
    return au < bu;
  }
  return av < bv;
}

static void sort_component_edges(uint32_t nedges, uint32_t *edge_u, uint32_t *edge_v) {
  for (uint32_t i = 1; i < nedges; i++) {
    uint32_t u = edge_u[i];
    uint32_t v = edge_v[i];
    uint32_t j = i;
    while (j > 0 && component_edge_less(u, v, edge_u[j - 1U], edge_v[j - 1U])) {
      edge_u[j] = edge_u[j - 1U];
      edge_v[j] = edge_v[j - 1U];
      j--;
    }
    edge_u[j] = u;
    edge_v[j] = v;
  }
}

static int compare_component_arrays(uint32_t nvars, uint32_t nedges, const uint64_t *lhs_unary,
                                    const uint32_t *lhs_edge_u, const uint32_t *lhs_edge_v,
                                    const uint64_t *rhs_unary,
                                    const uint32_t *rhs_edge_u, const uint32_t *rhs_edge_v) {
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
  }
  return 0;
}

typedef struct small_component_canonicalizer {
  const qsop_instance_t *sub;
  uint32_t *perm;
  bool *used;
  uint64_t *candidate_unary;
  uint32_t *candidate_edge_u;
  uint32_t *candidate_edge_v;
  uint64_t *best_unary;
  uint32_t *best_edge_u;
  uint32_t *best_edge_v;
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
  }
  sort_component_edges(sub->nedges, ctx->candidate_edge_u, ctx->candidate_edge_v);

  if (ctx->have_best &&
      compare_component_arrays(sub->nvars, sub->nedges, ctx->candidate_unary, ctx->candidate_edge_u,
                               ctx->candidate_edge_v, ctx->best_unary,
                               ctx->best_edge_u, ctx->best_edge_v) >= 0) {
    return;
  }

  memcpy(ctx->best_unary, ctx->candidate_unary, (size_t)sub->nvars * sizeof(*ctx->best_unary));
  memcpy(ctx->best_edge_u, ctx->candidate_edge_u, (size_t)sub->nedges * sizeof(*ctx->best_edge_u));
  memcpy(ctx->best_edge_v, ctx->candidate_edge_v, (size_t)sub->nedges * sizeof(*ctx->best_edge_v));
  ctx->have_best = true;
}

static void enumerate_component_permutations(small_component_canonicalizer_t *ctx,
                                             uint32_t depth) {
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

bool qsop_canonicalize_small_component(qsop_instance_t *sub, uint32_t max_nvars,
                                       qsop_error_t *error) {
  if (sub == NULL || sub->nvars <= 1U || sub->nvars > max_nvars) {
    return true;
  }

  const size_t nvars_alloc = sub->nvars == 0 ? 1U : sub->nvars;
  const size_t nedges_alloc = sub->nedges == 0 ? 1U : sub->nedges;
  small_component_canonicalizer_t ctx = {
      .sub = sub,
      .perm = malloc(nvars_alloc * sizeof(uint32_t)),
      .used = calloc(nvars_alloc, sizeof(bool)),
      .candidate_unary = malloc(nvars_alloc * sizeof(uint64_t)),
      .candidate_edge_u = malloc(nedges_alloc * sizeof(uint32_t)),
      .candidate_edge_v = malloc(nedges_alloc * sizeof(uint32_t)),
      .best_unary = malloc(nvars_alloc * sizeof(uint64_t)),
      .best_edge_u = malloc(nedges_alloc * sizeof(uint32_t)),
      .best_edge_v = malloc(nedges_alloc * sizeof(uint32_t)),
  };
  if (ctx.perm == NULL || ctx.used == NULL || ctx.candidate_unary == NULL ||
      ctx.candidate_edge_u == NULL || ctx.candidate_edge_v == NULL ||
      ctx.best_unary == NULL || ctx.best_edge_u == NULL || ctx.best_edge_v == NULL) {
    free(ctx.perm);
    free(ctx.used);
    free(ctx.candidate_unary);
    free(ctx.candidate_edge_u);
    free(ctx.candidate_edge_v);
    free(ctx.best_unary);
    free(ctx.best_edge_u);
    free(ctx.best_edge_v);
    set_error(error, "out of memory while canonicalizing small component");
    return false;
  }

  enumerate_component_permutations(&ctx, 0);
  memcpy(sub->unary, ctx.best_unary, (size_t)sub->nvars * sizeof(*sub->unary));
  memcpy(sub->edge_u, ctx.best_edge_u, (size_t)sub->nedges * sizeof(*sub->edge_u));
  memcpy(sub->edge_v, ctx.best_edge_v, (size_t)sub->nedges * sizeof(*sub->edge_v));

  free(ctx.perm);
  free(ctx.used);
  free(ctx.candidate_unary);
  free(ctx.candidate_edge_u);
  free(ctx.candidate_edge_v);
  free(ctx.best_unary);
  free(ctx.best_edge_u);
  free(ctx.best_edge_v);
  return true;
}
