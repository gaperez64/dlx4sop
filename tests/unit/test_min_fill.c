#include "dlx4sop/min_fill.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Straightforward dense O(n^3) reference implementation of the same greedy elimination, used to
 * prove the sparse core reproduces width, fill and order byte-for-byte. */

static bool ref_better(qsop_treewidth_order_t p, uint64_t fill, uint32_t degree,
                       uint64_t best_fill, uint32_t best_degree) {
  switch (p) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
    return fill < best_fill || (fill == best_fill && degree < best_degree);
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return degree < best_degree || (degree == best_degree && fill < best_fill);
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return fill < best_fill || (fill == best_fill && degree > best_degree);
  }
  return false;
}

static void ref_eliminate(uint32_t n, const uint32_t *eu, const uint32_t *ev, uint32_t ne,
                          qsop_treewidth_order_t policy, uint32_t *order, uint32_t *width_out,
                          uint64_t *fill_out) {
  bool *a = calloc((size_t)n * n + 1U, sizeof(*a));
  bool *active = malloc((size_t)n * sizeof(*active) + 1U);
  for (uint32_t v = 0; v < n; v++) {
    active[v] = true;
  }
  for (uint32_t e = 0; e < ne; e++) {
    const uint32_t u = eu[e], v = ev[e];
    if (u == v || u >= n || v >= n) {
      continue;
    }
    a[(size_t)u * n + v] = true;
    a[(size_t)v * n + u] = true;
  }
  uint32_t width = 0;
  uint64_t fill_total = 0;
  for (uint32_t step = 0; step < n; step++) {
    bool found = false;
    uint32_t best = 0, best_degree = 0;
    uint64_t best_fill = 0;
    for (uint32_t v = 0; v < n; v++) {
      if (!active[v]) {
        continue;
      }
      uint32_t degree = 0;
      for (uint32_t u = 0; u < n; u++) {
        if (active[u] && u != v && a[(size_t)v * n + u]) {
          degree++;
        }
      }
      uint64_t fill = 0;
      for (uint32_t u = 0; u < n; u++) {
        if (!active[u] || u == v || !a[(size_t)v * n + u]) {
          continue;
        }
        for (uint32_t w = u + 1U; w < n; w++) {
          if (active[w] && w != v && a[(size_t)v * n + w] && !a[(size_t)u * n + w]) {
            fill++;
          }
        }
      }
      if (!found || ref_better(policy, fill, degree, best_fill, best_degree)) {
        found = true;
        best = v;
        best_fill = fill;
        best_degree = degree;
      }
    }
    if (!found) {
      break;
    }
    if (best_degree > width) {
      width = best_degree;
    }
    fill_total += best_fill;
    order[step] = best;
    for (uint32_t u = 0; u < n; u++) {
      if (!active[u] || u == best || !a[(size_t)best * n + u]) {
        continue;
      }
      for (uint32_t w = u + 1U; w < n; w++) {
        if (active[w] && w != best && a[(size_t)best * n + w]) {
          a[(size_t)u * n + w] = true;
          a[(size_t)w * n + u] = true;
        }
      }
    }
    active[best] = false;
  }
  *width_out = width;
  *fill_out = fill_total;
  free(a);
  free(active);
}

static uint32_t ref_gf2_rank(uint64_t *rows, uint32_t nrows, uint32_t words) {
  uint32_t rank = 0;
  for (uint32_t col = 0; col < words * 64U && rank < nrows; col++) {
    uint32_t piv = rank;
    while (piv < nrows && !((rows[(size_t)piv * words + col / 64U] >> (col % 64U)) & 1U)) {
      piv++;
    }
    if (piv == nrows) {
      continue;
    }
    if (piv != rank) {
      for (uint32_t w = 0; w < words; w++) {
        const uint64_t t = rows[(size_t)rank * words + w];
        rows[(size_t)rank * words + w] = rows[(size_t)piv * words + w];
        rows[(size_t)piv * words + w] = t;
      }
    }
    for (uint32_t r = 0; r < nrows; r++) {
      if (r != rank && ((rows[(size_t)r * words + col / 64U] >> (col % 64U)) & 1U)) {
        for (uint32_t w = 0; w < words; w++) {
          rows[(size_t)r * words + w] ^= rows[(size_t)rank * words + w];
        }
      }
    }
    rank++;
  }
  return rank;
}

/* Dense O(n^4) reference for the prefix cut rank. */
static uint32_t ref_cut_rank(uint32_t n, const uint32_t *eu, const uint32_t *ev, uint32_t ne) {
  if (n <= 1U) {
    return 0;
  }
  const uint32_t words = (n + 63U) / 64U;
  uint64_t *adjr = calloc((size_t)n * words, sizeof(*adjr));
  uint64_t *rows = calloc((size_t)n * words, sizeof(*rows));
  for (uint32_t e = 0; e < ne; e++) {
    const uint32_t u = eu[e], v = ev[e];
    if (u == v || u >= n || v >= n) {
      continue;
    }
    adjr[(size_t)u * words + v / 64U] |= UINT64_C(1) << (v % 64U);
    adjr[(size_t)v * words + u / 64U] |= UINT64_C(1) << (u % 64U);
  }
  uint32_t maxr = 0;
  for (uint32_t c = 1; c < n; c++) {
    for (uint32_t v = 0; v < c; v++) {
      for (uint32_t w = 0; w < words; w++) {
        rows[(size_t)v * words + w] = adjr[(size_t)v * words + w];
      }
      for (uint32_t col = 0; col < c; col++) {
        rows[(size_t)v * words + col / 64U] &= ~(UINT64_C(1) << (col % 64U));
      }
    }
    const uint32_t r = ref_gf2_rank(rows, c, words);
    if (r > maxr) {
      maxr = r;
    }
  }
  free(adjr);
  free(rows);
  return maxr;
}

static int check(const char *name, uint32_t n, const uint32_t *eu, const uint32_t *ev,
                 uint32_t ne) {
  int rc = 0;
  {
    uint32_t got_cr = 0;
    qsop_error_t err = {0};
    const uint32_t ref_cr = ref_cut_rank(n, eu, ev, ne);
    if (!qsop_prefix_cut_rank(n, eu, ev, ne, &got_cr, &err)) {
      fprintf(stderr, "%s: cut-rank core failed: %s\n", name, err.message);
      rc = 1;
    } else if (got_cr != ref_cr) {
      fprintf(stderr, "%s: cut-rank %u (ref) != %u (got)\n", name, ref_cr, got_cr);
      rc = 1;
    }
  }
  for (int pi = 0; pi < 3; pi++) {
    const qsop_treewidth_order_t policy = (qsop_treewidth_order_t)pi;
    uint32_t *ref_order = malloc((size_t)(n == 0 ? 1U : n) * sizeof(uint32_t));
    uint32_t *got_order = malloc((size_t)(n == 0 ? 1U : n) * sizeof(uint32_t));
    uint32_t ref_w = 0, got_w = 0;
    uint64_t ref_f = 0, got_f = 0;
    bool capped = false;
    qsop_error_t error = {0};
    ref_eliminate(n, eu, ev, ne, policy, ref_order, &ref_w, &ref_f);
    if (!qsop_min_fill_eliminate(n, eu, ev, ne, policy, UINT32_MAX, got_order, &got_w, &got_f,
                                 &capped, &error)) {
      fprintf(stderr, "%s[p%d]: core failed: %s\n", name, pi, error.message);
      rc = 1;
    } else if (ref_w != got_w || ref_f != got_f || capped) {
      fprintf(stderr, "%s[p%d]: width %u/%u fill %llu/%llu capped=%d\n", name, pi, ref_w, got_w,
              (unsigned long long)ref_f, (unsigned long long)got_f, capped);
      rc = 1;
    } else {
      for (uint32_t i = 0; i < n; i++) {
        if (ref_order[i] != got_order[i]) {
          fprintf(stderr, "%s[p%d]: order[%u] %u/%u\n", name, pi, i, ref_order[i], got_order[i]);
          rc = 1;
          break;
        }
      }
    }
    free(ref_order);
    free(got_order);
  }
  return rc;
}

/* Simple deterministic LCG for reproducible random graphs. */
static uint64_t lcg_state = 88172645463325252ULL;
static uint32_t lcg(void) {
  lcg_state ^= lcg_state << 13;
  lcg_state ^= lcg_state >> 7;
  lcg_state ^= lcg_state << 17;
  return (uint32_t)(lcg_state >> 32);
}

static int check_random(const char *name, uint32_t n, uint32_t nedges) {
  uint32_t *eu = malloc((size_t)(nedges == 0 ? 1U : nedges) * sizeof(uint32_t));
  uint32_t *ev = malloc((size_t)(nedges == 0 ? 1U : nedges) * sizeof(uint32_t));
  for (uint32_t e = 0; e < nedges; e++) {
    eu[e] = lcg() % n;
    ev[e] = lcg() % n;
  }
  const int rc = check(name, n, eu, ev, nedges);
  free(eu);
  free(ev);
  return rc;
}

int main(void) {
  int rc = 0;

  /* Empty / trivial. */
  rc |= check("empty0", 0, NULL, NULL, 0);
  rc |= check("isolated5", 5, NULL, NULL, 0);
  {
    const uint32_t eu[] = {0}, ev[] = {1};
    rc |= check("single_edge", 2, eu, ev, 1);
  }

  /* Path 0-1-2-...-(n-1). */
  for (uint32_t n = 2; n <= 80; n += 13) {
    uint32_t *eu = malloc(n * sizeof(uint32_t));
    uint32_t *ev = malloc(n * sizeof(uint32_t));
    uint32_t ne = 0;
    for (uint32_t i = 0; i + 1U < n; i++) {
      eu[ne] = i;
      ev[ne] = i + 1U;
      ne++;
    }
    rc |= check("path", n, eu, ev, ne);
    /* Cycle: add closing edge. */
    eu[ne] = 0;
    ev[ne] = n - 1U;
    ne++;
    rc |= check("cycle", n, eu, ev, ne);
    free(eu);
    free(ev);
  }

  /* Star: center 0 connected to all others (n up to >63). */
  for (uint32_t n = 4; n <= 70; n += 33) {
    uint32_t *eu = malloc(n * sizeof(uint32_t));
    uint32_t *ev = malloc(n * sizeof(uint32_t));
    uint32_t ne = 0;
    for (uint32_t i = 1; i < n; i++) {
      eu[ne] = 0;
      ev[ne] = i;
      ne++;
    }
    rc |= check("star", n, eu, ev, ne);
    free(eu);
    free(ev);
  }

  /* Complete bipartite K_{a,b} (low rankwidth, higher treewidth). */
  {
    const uint32_t a = 5, b = 7, n = a + b;
    uint32_t eu[a * b], ev[a * b];
    uint32_t ne = 0;
    for (uint32_t i = 0; i < a; i++) {
      for (uint32_t j = 0; j < b; j++) {
        eu[ne] = i;
        ev[ne] = a + j;
        ne++;
      }
    }
    rc |= check("kbipartite", n, eu, ev, ne);
  }

  /* 2D grid g x g. */
  for (uint32_t g = 3; g <= 9; g += 3) {
    const uint32_t n = g * g;
    uint32_t *eu = malloc((size_t)2 * n * sizeof(uint32_t));
    uint32_t *ev = malloc((size_t)2 * n * sizeof(uint32_t));
    uint32_t ne = 0;
    for (uint32_t y = 0; y < g; y++) {
      for (uint32_t x = 0; x < g; x++) {
        const uint32_t v = y * g + x;
        if (x + 1U < g) {
          eu[ne] = v;
          ev[ne] = v + 1U;
          ne++;
        }
        if (y + 1U < g) {
          eu[ne] = v;
          ev[ne] = v + g;
          ne++;
        }
      }
    }
    rc |= check("grid", n, eu, ev, ne);
    free(eu);
    free(ev);
  }

  /* Random sparse and dense, including >63 vars. */
  rc |= check_random("rand_sparse_40", 40, 80);
  rc |= check_random("rand_sparse_100", 100, 200);
  rc |= check_random("rand_dense_30", 30, 200);
  rc |= check_random("rand_big_128", 128, 256);
  rc |= check_random("rand_multi_20", 20, 300); /* forces duplicate-edge dedup */

  /* Early-abort: a path has width 1, so a threshold of 1 must not cap, but 0 must. */
  {
    uint32_t eu[9], ev[9];
    for (uint32_t i = 0; i < 9; i++) {
      eu[i] = i;
      ev[i] = i + 1U;
    }
    uint32_t w = 0;
    bool capped = false;
    qsop_error_t error = {0};
    qsop_min_fill_eliminate(10, eu, ev, 9, QSOP_TREEWIDTH_ORDER_MIN_FILL, 0, NULL, &w, NULL,
                            &capped, &error);
    if (!capped || w <= 0) {
      fprintf(stderr, "early-abort threshold 0: expected cap with width>0, got capped=%d w=%u\n",
              capped, w);
      rc = 1;
    }
    capped = false;
    qsop_min_fill_eliminate(10, eu, ev, 9, QSOP_TREEWIDTH_ORDER_MIN_FILL, 1, NULL, &w, NULL,
                            &capped, &error);
    if (capped || w != 1) {
      fprintf(stderr, "early-abort threshold 1: expected no cap width 1, got capped=%d w=%u\n",
              capped, w);
      rc = 1;
    }
  }

  if (rc == 0) {
    printf("min-fill parity: all cases match\n");
  }
  return rc == 0 ? 0 : 1;
}
