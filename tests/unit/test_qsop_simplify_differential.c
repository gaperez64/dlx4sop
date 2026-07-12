/* Differential fuzzer: qsop_simplify_hadamard (the live, adjacency-incremental implementation)
 * vs qsop_simplify_hadamard_reference (the historical full-rebuild-per-elimination one, kept
 * only in qsop_simplify_reference.c for this comparison). Random instances are simplified by
 * both and must come out byte-for-byte identical -- not just amplitude-equivalent -- since
 * golden qsop output depends on the exact surviving variable numbering and edge set. */
#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

bool qsop_simplify_hadamard_reference(qsop_instance_t *inst);

typedef struct {
  uint32_t u;
  uint32_t v;
} pair_t;

static int compare_pairs(const void *a, const void *b) {
  const pair_t *x = a;
  const pair_t *y = b;
  if (x->u != y->u) {
    return x->u < y->u ? -1 : 1;
  }
  if (x->v != y->v) {
    return x->v < y->v ? -1 : 1;
  }
  return 0;
}

static qsop_instance_t *clone_instance(const qsop_instance_t *src) {
  qsop_instance_t *q = calloc(1, sizeof(*q));
  q->r = src->r;
  q->nvars = src->nvars;
  q->norm_h = src->norm_h;
  q->constant = src->constant;
  q->nedges = src->nedges;
  q->unary = malloc((src->nvars == 0U ? 1U : src->nvars) * sizeof(*q->unary));
  q->edge_u = malloc((src->nedges == 0U ? 1U : src->nedges) * sizeof(*q->edge_u));
  q->edge_v = malloc((src->nedges == 0U ? 1U : src->nedges) * sizeof(*q->edge_v));
  for (uint32_t i = 0; i < src->nvars; i++) {
    q->unary[i] = src->unary[i];
  }
  for (uint32_t i = 0; i < src->nedges; i++) {
    q->edge_u[i] = src->edge_u[i];
    q->edge_v[i] = src->edge_v[i];
  }
  return q;
}

/* Returns 0 if identical (nvars, nedges, norm_h, constant, unary[], sorted edge set), else 1,
 * with a diagnostic printed to stderr for whichever field first differs. */
static int compare_results(const qsop_instance_t *reference, const qsop_instance_t *live) {
  if (reference->nvars != live->nvars) {
    fprintf(stderr, "nvars mismatch: reference=%u live=%u\n", reference->nvars, live->nvars);
    return 1;
  }
  if (reference->nedges != live->nedges) {
    fprintf(stderr, "nedges mismatch: reference=%u live=%u\n", reference->nedges, live->nedges);
    return 1;
  }
  if (reference->norm_h != live->norm_h) {
    fprintf(stderr, "norm_h mismatch: reference=%llu live=%llu\n",
            (unsigned long long)reference->norm_h, (unsigned long long)live->norm_h);
    return 1;
  }
  if (reference->constant != live->constant) {
    fprintf(stderr, "constant mismatch: reference=%llu live=%llu\n",
            (unsigned long long)reference->constant, (unsigned long long)live->constant);
    return 1;
  }
  for (uint32_t i = 0; i < reference->nvars; i++) {
    if (reference->unary[i] != live->unary[i]) {
      fprintf(stderr, "unary[%u] mismatch: reference=%llu live=%llu\n", i,
              (unsigned long long)reference->unary[i], (unsigned long long)live->unary[i]);
      return 1;
    }
  }

  pair_t *pa = malloc((reference->nedges == 0U ? 1U : reference->nedges) * sizeof(*pa));
  pair_t *pb = malloc((live->nedges == 0U ? 1U : live->nedges) * sizeof(*pb));
  for (uint32_t i = 0; i < reference->nedges; i++) {
    const uint32_t u = reference->edge_u[i];
    const uint32_t v = reference->edge_v[i];
    pa[i].u = u < v ? u : v;
    pa[i].v = u < v ? v : u;
  }
  for (uint32_t i = 0; i < live->nedges; i++) {
    const uint32_t u = live->edge_u[i];
    const uint32_t v = live->edge_v[i];
    pb[i].u = u < v ? u : v;
    pb[i].v = u < v ? v : u;
  }
  qsort(pa, reference->nedges, sizeof(*pa), compare_pairs);
  qsort(pb, live->nedges, sizeof(*pb), compare_pairs);
  int mismatch = 0;
  for (uint32_t i = 0; i < reference->nedges; i++) {
    if (pa[i].u != pb[i].u || pa[i].v != pb[i].v) {
      fprintf(stderr, "edge[%u] mismatch: reference=(%u,%u) live=(%u,%u)\n", i, pa[i].u, pa[i].v,
              pb[i].u, pb[i].v);
      mismatch = 1;
      break;
    }
  }
  free(pa);
  free(pb);
  return mismatch;
}

static uint64_t xorshift(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

int main(void) {
  uint64_t seed = 0x9e3779b97f4a7c15ULL;
  int failures = 0;
  const int trials = 50000;
  const uint64_t moduli[] = {8, 16, 32, 64, 128};

  for (int t = 0; t < trials; t++) {
    const uint32_t nvars = (uint32_t)(xorshift(&seed) % 24) + 1;
    const uint64_t r = moduli[xorshift(&seed) % (sizeof(moduli) / sizeof(moduli[0]))];
    const uint64_t sign_coeff = r / 2U;

    qsop_instance_t base = {0};
    base.r = r;
    base.nvars = nvars;
    base.norm_h = (xorshift(&seed) % 40U) * 2U;
    base.constant = xorshift(&seed) % r;
    base.unary = malloc(nvars * sizeof(*base.unary));
    for (uint32_t i = 0; i < nvars; i++) {
      /* Bias heavily toward {0, sign_coeff} so eliminations actually fire. */
      const uint64_t roll = xorshift(&seed) % 4U;
      base.unary[i] = roll == 0U ? 0U : roll == 1U ? sign_coeff : xorshift(&seed) % r;
    }

    /* Random candidate edge list, possibly with duplicates/self-loops (canonicalize_edges must
     * handle both); nedges up to a small multiple of nvars. */
    const uint32_t max_edges = nvars * 3U + 2U;
    const uint32_t nedges = (uint32_t)(xorshift(&seed) % (max_edges + 1U));
    uint32_t *eu = malloc((nedges == 0U ? 1U : nedges) * sizeof(*eu));
    uint32_t *ev = malloc((nedges == 0U ? 1U : nedges) * sizeof(*ev));
    for (uint32_t i = 0; i < nedges; i++) {
      eu[i] = (uint32_t)(xorshift(&seed) % nvars);
      ev[i] = (uint32_t)(xorshift(&seed) % nvars);
    }
    base.edge_u = eu;
    base.edge_v = ev;
    base.nedges = nedges;

    qsop_instance_t *reference = clone_instance(&base);
    qsop_instance_t *live = clone_instance(&base);
    free(base.unary);
    free(base.edge_u);
    free(base.edge_v);

    if (!qsop_simplify_hadamard_reference(reference) || !qsop_simplify_hadamard(live)) {
      fprintf(stderr, "trial %d: allocation failure\n", t);
      failures++;
    } else if (compare_results(reference, live) != 0) {
      fprintf(stderr, "  ^ trial %d FAILED\n", t);
      failures++;
    }
    qsop_free(reference);
    qsop_free(live);
  }

  if (failures > 0) {
    fprintf(stderr, "%d/%d trials FAILED\n", failures, trials);
    return 1;
  }
  printf("%d trials OK\n", trials);
  return 0;
}
