#include "../../src/solve/branch_shadow.h"

#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residual.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Generic invariants, checked after every build/reduce in this file.
 * --------------------------------------------------------------------------- */

static int check_simple_graph(const char *name, const branch_shadow_t *s) {
  int rc = 0;
  for (uint32_t v = 0; v < s->nvars; v++) {
    if (!s->alive[v]) {
      if (s->adj[v].len != 0U || s->adj[v].data != NULL) {
        fprintf(stderr, "%s: dead vertex %u still has adjacency\n", name, v);
        rc = 1;
      }
      continue;
    }
    const branch_shadow_adjlist_t *adj = &s->adj[v];
    for (uint32_t i = 0; i < adj->len; i++) {
      const uint32_t n = adj->data[i];
      if (n == v) {
        fprintf(stderr, "%s: self-loop at %u\n", name, v);
        rc = 1;
        continue;
      }
      if (n >= s->nvars || !s->alive[n]) {
        fprintf(stderr, "%s: dangling edge %u->%u\n", name, v, n);
        rc = 1;
        continue;
      }
      for (uint32_t j = i + 1U; j < adj->len; j++) {
        if (adj->data[j] == n) {
          fprintf(stderr, "%s: duplicate neighbour %u at %u\n", name, n, v);
          rc = 1;
        }
      }
      bool found = false;
      for (uint32_t k = 0; k < s->adj[n].len; k++) {
        if (s->adj[n].data[k] == v) {
          found = true;
          break;
        }
      }
      if (!found) {
        fprintf(stderr, "%s: asymmetric edge %u->%u\n", name, v, n);
        rc = 1;
      }
    }
  }
  return rc;
}

/* Exhaustive degree-<=2 elimination must leave every alive vertex at degree >= 3: whenever a
 * vertex's degree drops to (or starts at, or a fill edge raises it back to) <= 2 it is pushed
 * onto the worklist, and the worklist runs to exhaustion, so no alive vertex can end below 3. */
static int check_fully_reduced(const char *name, const branch_shadow_t *s) {
  int rc = 0;
  for (uint32_t v = 0; v < s->nvars; v++) {
    if (s->alive[v] && s->adj[v].len < 3U) {
      fprintf(stderr, "%s: alive vertex %u has degree %u after series reduce\n", name, v,
              s->adj[v].len);
      rc = 1;
    }
  }
  return rc;
}

static int check_reduce(const char *name, uint32_t n, const uint32_t *eu, const uint32_t *ev,
                        uint32_t ne, int64_t expect_alive_vars /* -1 = don't check */) {
  int rc = 0;
  branch_shadow_t s = {0};
  qsop_error_t err = {0};
  if (!branch_shadow_build_from_edges(n, eu, ev, ne, &s, &err)) {
    fprintf(stderr, "%s: build failed: %s\n", name, err.message);
    return 1;
  }
  rc |= check_simple_graph(name, &s);
  if (!branch_shadow_series_reduce(&s, &err)) {
    fprintf(stderr, "%s: reduce failed: %s\n", name, err.message);
    branch_shadow_free(&s);
    return 1;
  }
  rc |= check_simple_graph(name, &s);
  rc |= check_fully_reduced(name, &s);
  if (expect_alive_vars >= 0 && (int64_t)s.alive_vars != expect_alive_vars) {
    fprintf(stderr, "%s: alive_vars %u != expected %" PRId64 "\n", name, s.alive_vars,
            expect_alive_vars);
    rc = 1;
  }

  /* Determinism: build + reduce again from scratch, compare. */
  branch_shadow_t s2 = {0};
  qsop_error_t err2 = {0};
  if (!branch_shadow_build_from_edges(n, eu, ev, ne, &s2, &err2) ||
      !branch_shadow_series_reduce(&s2, &err2)) {
    fprintf(stderr, "%s: second build/reduce failed\n", name);
    rc = 1;
  } else {
    if (s2.alive_vars != s.alive_vars || s2.alive_edges != s.alive_edges) {
      fprintf(stderr, "%s: nondeterministic alive_vars/edges\n", name);
      rc = 1;
    }
    for (uint32_t v = 0; v < n; v++) {
      if (s.alive[v] != s2.alive[v]) {
        fprintf(stderr, "%s: nondeterministic alive[%u]\n", name, v);
        rc = 1;
      } else if (s.alive[v] && s.adj[v].len != s2.adj[v].len) {
        fprintf(stderr, "%s: nondeterministic degree[%u]\n", name, v);
        rc = 1;
      }
    }
  }
  branch_shadow_free(&s2);
  branch_shadow_free(&s);
  return rc;
}

/* ---------------------------------------------------------------------------
 * Reduction cases.
 * --------------------------------------------------------------------------- */

static int test_empty_graph(void) { return check_reduce("empty", 0, NULL, NULL, 0, 0); }

static int test_isolated_vertex(void) {
  return check_reduce("isolated5", 5, NULL, NULL, 0, 0);
}

static int test_path(void) {
  int rc = 0;
  for (uint32_t n = 2; n <= 40; n += 7) {
    uint32_t *eu = malloc(n * sizeof(*eu));
    uint32_t *ev = malloc(n * sizeof(*ev));
    uint32_t ne = 0;
    for (uint32_t i = 0; i + 1U < n; i++) {
      eu[ne] = i;
      ev[ne] = i + 1U;
      ne++;
    }
    rc |= check_reduce("path", n, eu, ev, ne, 0);
    free(eu);
    free(ev);
  }
  return rc;
}

static int test_cycle(void) {
  int rc = 0;
  for (uint32_t n = 3; n <= 40; n += 7) {
    uint32_t *eu = malloc((n + 1U) * sizeof(*eu));
    uint32_t *ev = malloc((n + 1U) * sizeof(*ev));
    uint32_t ne = 0;
    for (uint32_t i = 0; i + 1U < n; i++) {
      eu[ne] = i;
      ev[ne] = i + 1U;
      ne++;
    }
    eu[ne] = n - 1U;
    ev[ne] = 0;
    ne++;
    /* A pure cycle is a 2-regular graph with no degree-3+ anchor anywhere, so it always
     * collapses entirely -- but assert only the general invariants (as recommended for
     * elimination-order-sensitive cases), plus this specific corollary. */
    rc |= check_reduce("cycle", n, eu, ev, ne, 0);
    free(eu);
    free(ev);
  }
  return rc;
}

static int test_star(void) {
  int rc = 0;
  for (uint32_t leaves = 1; leaves <= 20; leaves += 6) {
    const uint32_t n = leaves + 1U; /* vertex 0 is the centre */
    uint32_t *eu = malloc(leaves * sizeof(*eu));
    uint32_t *ev = malloc(leaves * sizeof(*ev));
    for (uint32_t i = 0; i < leaves; i++) {
      eu[i] = 0;
      ev[i] = i + 1U;
    }
    /* A star's centre degree only ever decreases as leaves peel off, so it too eventually
     * drops to <= 2 and is eliminated: the whole star collapses. */
    rc |= check_reduce("star", n, eu, ev, leaves, 0);
    free(eu);
    free(ev);
  }
  return rc;
}

static int test_complete_bipartite_no_op(void) {
  /* K_{4,4}: every vertex has degree 4, so nothing is eligible for elimination -- the shadow
   * should come out of series_reduce completely unchanged. */
  const uint32_t a = 4, b = 4, n = a + b;
  uint32_t eu[16], ev[16];
  uint32_t ne = 0;
  for (uint32_t i = 0; i < a; i++) {
    for (uint32_t j = 0; j < b; j++) {
      eu[ne] = i;
      ev[ne] = a + j;
      ne++;
    }
  }
  return check_reduce("kbipartite44", n, eu, ev, ne, (int64_t)n);
}

static int test_grid_partial(void) {
  /* 3x3 grid: corners (degree 2) are eliminated; the 4 edge-cells + centre survive as a
   * "wheel"-shaped degree-3/4 core. Assert partial (not full, not zero) reduction, plus the
   * general invariants -- exact survivor count is elimination-order-sensitive in general, even
   * though this particular size happens to be traceable by hand. */
  const uint32_t g = 3, n = g * g;
  uint32_t eu[2 * 9], ev[2 * 9];
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
  branch_shadow_t s = {0};
  qsop_error_t err = {0};
  int rc = 0;
  if (!branch_shadow_build_from_edges(n, eu, ev, ne, &s, &err) ||
      !branch_shadow_series_reduce(&s, &err)) {
    fprintf(stderr, "grid3x3: build/reduce failed: %s\n", err.message);
    branch_shadow_free(&s);
    return 1;
  }
  rc |= check_simple_graph("grid3x3", &s);
  rc |= check_fully_reduced("grid3x3", &s);
  if (s.alive_vars == 0U || s.alive_vars >= n) {
    fprintf(stderr, "grid3x3: expected partial reduction, got alive_vars=%u\n", s.alive_vars);
    rc = 1;
  }
  branch_shadow_free(&s);
  return rc;
}

/* QNN pair motif: x, y -- check -- value, with x/y additionally embedded in a K4 {x,y,p,q} so
 * they have enough degree of their own to survive as logical variables (a single isolated
 * gadget, with x/y otherwise degree 1, is topologically a tree and -- correctly -- collapses to
 * nothing; the motif's premise is that x_i/y_i are hubs wired into many other things). Expect:
 * check and value eliminated, K4 {x,y,p,q} survives unchanged (x--y was already an edge, so the
 * fill-edge attempt from eliminating `check` is a no-op -- this doubles as the "duplicate fill
 * edge" case). */
static int test_qnn_pair_motif(void) {
  enum { x = 0, y = 1, p = 2, q = 3, check = 4, value = 5, n = 6 };
  const uint32_t eu[] = {x, x, x, y, y, p, x, y, check};
  const uint32_t ev[] = {y, p, q, p, q, q, check, check, value};
  const uint32_t ne = sizeof(eu) / sizeof(eu[0]);

  branch_shadow_t s = {0};
  qsop_error_t err = {0};
  int rc = 0;
  if (!branch_shadow_build_from_edges(n, eu, ev, ne, &s, &err)) {
    fprintf(stderr, "qnn_pair: build failed: %s\n", err.message);
    return 1;
  }
  rc |= check_simple_graph("qnn_pair", &s);
  if (!branch_shadow_series_reduce(&s, &err)) {
    fprintf(stderr, "qnn_pair: reduce failed: %s\n", err.message);
    branch_shadow_free(&s);
    return 1;
  }
  rc |= check_simple_graph("qnn_pair", &s);
  rc |= check_fully_reduced("qnn_pair", &s);

  if (s.alive_vars != 4U || s.alive_edges != 6U) {
    fprintf(stderr, "qnn_pair: expected K4 survivor (4 vars/6 edges), got %u/%" PRIu64 "\n",
            s.alive_vars, s.alive_edges);
    rc = 1;
  }
  if (s.alive[check] || s.alive[value]) {
    fprintf(stderr, "qnn_pair: check/value survived elimination\n");
    rc = 1;
  }
  if (!s.alive[x] || !s.alive[y] || !s.alive[p] || !s.alive[q]) {
    fprintf(stderr, "qnn_pair: x/y/p/q did not all survive\n");
    rc = 1;
  }
  bool xy_edge = false;
  for (uint32_t i = 0; i < s.adj[x].len; i++) {
    if (s.adj[x].data[i] == y) {
      xy_edge = true;
    }
  }
  if (!xy_edge) {
    fprintf(stderr, "qnn_pair: x--y edge missing after reduction\n");
    rc = 1;
  }
  branch_shadow_free(&s);
  return rc;
}

/* ---------------------------------------------------------------------------
 * Shortlist-level cases, over qsop_residual_t.
 * --------------------------------------------------------------------------- */

static qsop_residual_t *make_residual(uint32_t nvars, const uint32_t *eu, const uint32_t *ev,
                                      uint32_t ne) {
  uint64_t *unary = calloc(nvars == 0U ? 1U : nvars, sizeof(*unary));
  qsop_instance_t inst = {
      .r = 2,
      .nvars = nvars,
      .norm_h = 0,
      .constant = 0,
      .unary = unary,
      .nedges = ne,
      .edge_u = (uint32_t *)eu,
      .edge_v = (uint32_t *)ev,
  };
  qsop_residual_t *residual = NULL;
  qsop_error_t err = {0};
  const bool ok = qsop_residual_create(&inst, &residual, &err);
  free(unary);
  if (!ok) {
    fprintf(stderr, "make_residual: create failed: %s\n", err.message);
    return NULL;
  }
  return residual;
}

static int test_shortlist_empty_core(void) {
  /* A pure path has no degree-3+ core, so the reduced shadow is empty and the shortlist must
   * come back empty (not an error). */
  enum { n = 6 };
  const uint32_t eu[] = {0, 1, 2, 3, 4};
  const uint32_t ev[] = {1, 2, 3, 4, 5};
  qsop_residual_t *residual = make_residual(n, eu, ev, 5);
  if (residual == NULL) {
    return 1;
  }
  uint32_t *vars = NULL;
  uint32_t len = 0;
  qsop_error_t err = {0};
  int rc = 0;
  if (!branch_shadow_shortlist(residual, 8U, &vars, &len, NULL, &err)) {
    fprintf(stderr, "shortlist_empty_core: shortlist failed: %s\n", err.message);
    rc = 1;
  } else if (len != 0U || vars != NULL) {
    fprintf(stderr, "shortlist_empty_core: expected empty shortlist, got len=%u\n", len);
    rc = 1;
  }
  free(vars);
  qsop_residual_free(residual);
  return rc;
}

static int test_shortlist_active_and_limit(void) {
  /* Two disjoint K4s (8 vars, degree 3 each -- no reduction at all), asking for a shortlist of
   * 3. Every returned ID must be an original, currently-active variable, and length must not
   * exceed the requested limit. */
  enum { n = 8, limit = 3 };
  uint32_t eu[12], ev[12];
  uint32_t ne = 0;
  for (uint32_t base = 0; base < 8U; base += 4U) {
    for (uint32_t i = 0; i < 4U; i++) {
      for (uint32_t j = i + 1U; j < 4U; j++) {
        eu[ne] = base + i;
        ev[ne] = base + j;
        ne++;
      }
    }
  }
  qsop_residual_t *residual = make_residual(n, eu, ev, ne);
  if (residual == NULL) {
    return 1;
  }
  uint32_t *vars = NULL;
  uint32_t len = 0;
  qsop_error_t err = {0};
  int rc = 0;
  if (!branch_shadow_shortlist(residual, limit, &vars, &len, NULL, &err)) {
    fprintf(stderr, "shortlist_active_and_limit: shortlist failed: %s\n", err.message);
    rc = 1;
  } else {
    if (len == 0U || len > limit) {
      fprintf(stderr, "shortlist_active_and_limit: len=%u outside (0,%u]\n", len, limit);
      rc = 1;
    }
    for (uint32_t i = 0; i < len; i++) {
      if (vars[i] >= n || !qsop_residual_var_active(residual, vars[i])) {
        fprintf(stderr, "shortlist_active_and_limit: candidate %u not active/in range\n", vars[i]);
        rc = 1;
      }
      for (uint32_t j = i + 1U; j < len; j++) {
        if (vars[i] == vars[j]) {
          fprintf(stderr, "shortlist_active_and_limit: duplicate candidate %u\n", vars[i]);
          rc = 1;
        }
      }
    }
  }
  free(vars);
  qsop_residual_free(residual);
  return rc;
}

static int test_shortlist_prefers_collapsing_candidate(void) {
  /* Two K5s {0..4} and {5..9} joined by a single extra edge (0,5). Removing the joint endpoint
   * (0 or 5) disconnects the two K5s (one side drops to a stable K4, the other stays a full,
   * disconnected K5): largest_component shrinks to 5. Removing an ordinary member (e.g. 1)
   * leaves the joint edge intact, so everything stays one connected component of size 9. Rank
   * #1 in branch_shadow_candidate_better is smaller largest_component, so 0 (tied with its
   * mirror-symmetric partner 5, broken by lower ID) must outrank an ordinary member. */
  enum { n = 10 };
  uint32_t eu[10 + 10 + 1], ev[10 + 10 + 1];
  uint32_t ne = 0;
  for (uint32_t base = 0; base < 10U; base += 5U) {
    for (uint32_t i = 0; i < 5U; i++) {
      for (uint32_t j = i + 1U; j < 5U; j++) {
        eu[ne] = base + i;
        ev[ne] = base + j;
        ne++;
      }
    }
  }
  eu[ne] = 0;
  ev[ne] = 5;
  ne++;

  qsop_residual_t *residual = make_residual(n, eu, ev, ne);
  if (residual == NULL) {
    return 1;
  }
  uint32_t *vars = NULL;
  uint32_t len = 0;
  qsop_error_t err = {0};
  int rc = 0;
  if (!branch_shadow_shortlist(residual, 1U, &vars, &len, NULL, &err)) {
    fprintf(stderr, "shortlist_prefers_collapsing: shortlist failed: %s\n", err.message);
    rc = 1;
  } else if (len != 1U || vars[0] != 0U) {
    fprintf(stderr, "shortlist_prefers_collapsing: expected {0}, got len=%u var=%s\n", len,
            len == 1U ? "?" : "(none)");
    rc = 1;
  }
  free(vars);
  qsop_residual_free(residual);
  return rc;
}

static int test_shortlist_off_by_zero_limit(void) {
  qsop_residual_t *residual = make_residual(0, NULL, NULL, 0);
  if (residual == NULL) {
    return 1;
  }
  uint32_t *vars = (uint32_t *)0x1; /* sentinel, must be reset to NULL */
  uint32_t len = 1234;
  qsop_error_t err = {0};
  int rc = 0;
  if (!branch_shadow_shortlist(residual, 0U, &vars, &len, NULL, &err)) {
    fprintf(stderr, "shortlist_off_by_zero_limit: shortlist failed: %s\n", err.message);
    rc = 1;
  } else if (vars != NULL || len != 0U) {
    fprintf(stderr, "shortlist_off_by_zero_limit: expected no-op for limit=0\n");
    rc = 1;
  }
  qsop_residual_free(residual);
  return rc;
}

int main(void) {
  int rc = 0;

  rc |= test_empty_graph();
  rc |= test_isolated_vertex();
  rc |= test_path();
  rc |= test_cycle();
  rc |= test_star();
  rc |= test_complete_bipartite_no_op();
  rc |= test_grid_partial();
  rc |= test_qnn_pair_motif();

  rc |= test_shortlist_empty_core();
  rc |= test_shortlist_active_and_limit();
  rc |= test_shortlist_prefers_collapsing_candidate();
  rc |= test_shortlist_off_by_zero_limit();

  if (rc == 0) {
    fprintf(stderr, "all branch_shadow tests passed\n");
  }
  return rc;
}
