#include "dlx4sop/min_fill.h"

#include <stdlib.h>
#include <string.h>

static void min_fill_set_error(qsop_error_t *error, const char *message) {
  if (error == NULL) {
    return;
  }
  error->path = NULL;
  error->line = 0;
  error->column = 0;
  size_t i = 0;
  for (; message[i] != '\0' && i + 1U < sizeof(error->message); i++) {
    error->message[i] = message[i];
  }
  error->message[i] = '\0';
}

typedef struct adjlist {
  uint32_t *data;
  uint32_t len;
  uint32_t cap;
} adjlist_t;

static bool adjlist_push(adjlist_t *a, uint32_t x) {
  if (a->len == a->cap) {
    const uint32_t ncap = a->cap == 0U ? 4U : (a->cap > UINT32_MAX / 2U ? UINT32_MAX : a->cap * 2U);
    uint32_t *nd = realloc(a->data, (size_t)ncap * sizeof(*nd));
    if (nd == NULL) {
      return false;
    }
    a->data = nd;
    a->cap = ncap;
  }
  a->data[a->len++] = x;
  return true;
}

static void adjlist_remove(adjlist_t *a, uint32_t x) {
  for (uint32_t i = 0; i < a->len; i++) {
    if (a->data[i] == x) {
      a->data[i] = a->data[a->len - 1U];
      a->len--;
      return;
    }
  }
}

/* Strictly-better test matching the dense treewidth_candidate_is_better (index tie-break is
 * handled by the caller keeping the first, i.e. lowest-index, candidate on a full tie). */
static bool min_fill_better(qsop_treewidth_order_t tie_break, uint64_t fill, uint32_t degree,
                            uint64_t best_fill, uint32_t best_degree) {
  switch (tie_break) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
    return fill < best_fill || (fill == best_fill && degree < best_degree);
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return degree < best_degree || (degree == best_degree && fill < best_fill);
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return fill < best_fill || (fill == best_fill && degree > best_degree);
  }
  return false;
}

/* fill(v) = number of non-adjacent pairs among v's (active) neighbours. Uses `mark` stamped
 * with N(v) for O(1) membership; cost is O(sum of neighbour degrees). */
static uint64_t min_fill_vertex_fill(uint32_t v, const adjlist_t *adj, uint64_t *mark,
                                     uint64_t stamp) {
  const adjlist_t *av = &adj[v];
  const uint32_t d = av->len;
  for (uint32_t i = 0; i < d; i++) {
    mark[av->data[i]] = stamp;
  }
  uint64_t inter = 0; /* counts each edge among N(v) twice */
  for (uint32_t i = 0; i < d; i++) {
    const adjlist_t *au = &adj[av->data[i]];
    for (uint32_t j = 0; j < au->len; j++) {
      if (mark[au->data[j]] == stamp) {
        inter++;
      }
    }
  }
  const uint64_t pairs = (uint64_t)d * (d - 1U) / 2U;
  return pairs - inter / 2U;
}

/* --- Incremental GF(2) prefix-cut-rank --------------------------------------------------- */

typedef struct gf2_basis {
  uint64_t *rows; /* len * words, one reduced basis vector per row */
  uint32_t *pivot;
  uint32_t len;
  uint32_t cap;
  uint32_t words;
} gf2_basis_t;

static uint32_t gf2_lowest_set(const uint64_t *row, uint32_t words) {
  for (uint32_t w = 0; w < words; w++) {
    if (row[w] != 0) {
      return w * 64U + (uint32_t)__builtin_ctzll(row[w]);
    }
  }
  return UINT32_MAX;
}

/* Reduce `row` against the basis, then (if nonzero) insert it in reduced row-echelon form so
 * that every pivot column holds a single 1 across the basis. `row` is scratch (consumed). */
static bool gf2_basis_add(gf2_basis_t *b, uint64_t *row, qsop_error_t *error) {
  const uint32_t words = b->words;
  for (uint32_t i = 0; i < b->len; i++) {
    const uint32_t p = b->pivot[i];
    if ((row[p / 64U] >> (p % 64U)) & 1U) {
      const uint64_t *bi = &b->rows[(size_t)i * words];
      for (uint32_t w = 0; w < words; w++) {
        row[w] ^= bi[w];
      }
    }
  }
  const uint32_t newpivot = gf2_lowest_set(row, words);
  if (newpivot == UINT32_MAX) {
    return true; /* row is spanned by the basis; rank unchanged */
  }
  for (uint32_t i = 0; i < b->len; i++) {
    uint64_t *bi = &b->rows[(size_t)i * words];
    if ((bi[newpivot / 64U] >> (newpivot % 64U)) & 1U) {
      for (uint32_t w = 0; w < words; w++) {
        bi[w] ^= row[w];
      }
    }
  }
  if (b->len == b->cap) {
    const uint32_t ncap =
        b->cap == 0U ? 16U : (b->cap > UINT32_MAX / 2U ? UINT32_MAX : b->cap * 2U);
    uint64_t *nr = realloc(b->rows, (size_t)ncap * words * sizeof(*nr));
    uint32_t *np = realloc(b->pivot, (size_t)ncap * sizeof(*np));
    if (nr != NULL) {
      b->rows = nr;
    }
    if (np != NULL) {
      b->pivot = np;
    }
    if (nr == NULL || np == NULL) {
      min_fill_set_error(error, "out of memory while computing prefix cut rank");
      return false;
    }
    b->cap = ncap;
  }
  memcpy(&b->rows[(size_t)b->len * words], row, words * sizeof(*row));
  b->pivot[b->len] = newpivot;
  b->len++;
  return true;
}

bool qsop_prefix_cut_rank(uint32_t nvars, const uint32_t *edge_u, const uint32_t *edge_v,
                          uint32_t nedges, uint32_t *out_rank, qsop_error_t *error) {
  *out_rank = 0;
  if (nvars <= 1U) {
    return true;
  }
  const uint32_t words = (nvars + 63U) / 64U;

  adjlist_t *adj = calloc(nvars, sizeof(*adj));
  uint64_t *row = calloc(words, sizeof(*row));
  gf2_basis_t basis = {.words = words};
  bool ok = adj != NULL && row != NULL;

  if (ok) {
    for (uint32_t e = 0; e < nedges; e++) {
      const uint32_t u = edge_u[e];
      const uint32_t v = edge_v[e];
      if (u == v || u >= nvars || v >= nvars) {
        continue;
      }
      if (!adjlist_push(&adj[u], v) || !adjlist_push(&adj[v], u)) {
        ok = false;
        break;
      }
    }
  }

  uint32_t maxrank = 0;
  for (uint32_t c = 1; c < nvars && ok; c++) {
    const uint32_t leaving = c - 1U;
    /* Delete column `leaving` (the minimum present column): at most one basis vector owns it as
     * its pivot, so clear that bit and re-insert the residual. */
    uint32_t owner = UINT32_MAX;
    for (uint32_t i = 0; i < basis.len; i++) {
      if (basis.pivot[i] == leaving) {
        owner = i;
        break;
      }
    }
    if (owner != UINT32_MAX) {
      memcpy(row, &basis.rows[(size_t)owner * words], words * sizeof(*row));
      row[leaving / 64U] &= ~(UINT64_C(1) << (leaving % 64U));
      basis.len--;
      if (owner != basis.len) {
        memcpy(&basis.rows[(size_t)owner * words], &basis.rows[(size_t)basis.len * words],
               words * sizeof(*row));
        basis.pivot[owner] = basis.pivot[basis.len];
      }
      ok = gf2_basis_add(&basis, row, error);
      if (!ok) {
        break;
      }
    }
    /* Add vertex `leaving` to the left: its adjacency restricted to the new right side {c..}. */
    memset(row, 0, words * sizeof(*row));
    const adjlist_t *av = &adj[leaving];
    for (uint32_t i = 0; i < av->len; i++) {
      const uint32_t w = av->data[i];
      if (w >= c) {
        row[w / 64U] |= UINT64_C(1) << (w % 64U);
      }
    }
    ok = gf2_basis_add(&basis, row, error);
    if (basis.len > maxrank) {
      maxrank = basis.len;
    }
  }

  if (ok) {
    *out_rank = maxrank;
  }
  if (adj != NULL) {
    for (uint32_t v = 0; v < nvars; v++) {
      free(adj[v].data);
    }
  }
  free(adj);
  free(row);
  free(basis.rows);
  free(basis.pivot);
  return ok;
}

bool qsop_min_fill_eliminate(uint32_t nvars, const uint32_t *edge_u, const uint32_t *edge_v,
                             uint32_t nedges, qsop_treewidth_order_t tie_break,
                             uint32_t width_abort_threshold, uint32_t *order_out,
                             uint32_t *width_out, uint64_t *fill_edges_out, uint64_t *dp_work_out,
                             bool *width_capped_out, qsop_error_t *error) {
  if (dp_work_out != NULL) {
    *dp_work_out = 0;
  }
  if (width_out != NULL) {
    *width_out = 0;
  }
  if (fill_edges_out != NULL) {
    *fill_edges_out = 0;
  }
  if (width_capped_out != NULL) {
    *width_capped_out = false;
  }
  if (nvars == 0U) {
    return true;
  }

  adjlist_t *adj = calloc(nvars, sizeof(*adj));
  uint64_t *fillv = malloc((size_t)nvars * sizeof(*fillv));
  uint32_t *degv = malloc((size_t)nvars * sizeof(*degv));
  bool *active = malloc((size_t)nvars * sizeof(*active));
  uint64_t *mark = calloc(nvars, sizeof(*mark));
  uint32_t *touched = malloc((size_t)nvars * sizeof(*touched));
  bool ok = adj != NULL && fillv != NULL && degv != NULL && active != NULL && mark != NULL &&
            touched != NULL;

  if (ok) {
    for (uint32_t e = 0; e < nedges; e++) {
      const uint32_t u = edge_u[e];
      const uint32_t v = edge_v[e];
      if (u == v || u >= nvars || v >= nvars) {
        continue;
      }
      /* Skip duplicate edges so degree matches the dense bitset (idempotent) builders. */
      bool present = false;
      for (uint32_t i = 0; i < adj[u].len; i++) {
        if (adj[u].data[i] == v) {
          present = true;
          break;
        }
      }
      if (present) {
        continue;
      }
      if (!adjlist_push(&adj[u], v) || !adjlist_push(&adj[v], u)) {
        ok = false;
        break;
      }
    }
  }

  uint64_t stamp = 0;
  if (ok) {
    for (uint32_t v = 0; v < nvars; v++) {
      active[v] = true;
      degv[v] = adj[v].len;
      fillv[v] = min_fill_vertex_fill(v, adj, mark, ++stamp);
    }

    uint32_t width = 0;
    for (uint32_t step = 0; step < nvars; step++) {
      bool found = false;
      uint32_t best = 0;
      uint64_t best_fill = 0;
      uint32_t best_degree = 0;
      for (uint32_t v = 0; v < nvars; v++) {
        if (!active[v]) {
          continue;
        }
        if (!found || min_fill_better(tie_break, fillv[v], degv[v], best_fill, best_degree)) {
          found = true;
          best = v;
          best_fill = fillv[v];
          best_degree = degv[v];
        }
      }
      if (!found) {
        break;
      }

      if (best_degree > width) {
        width = best_degree;
        if (width_abort_threshold != UINT32_MAX && width > width_abort_threshold) {
          if (width_capped_out != NULL) {
            *width_capped_out = true;
          }
          break;
        }
      }
      if (order_out != NULL) {
        order_out[step] = best;
      }
      if (fill_edges_out != NULL) {
        *fill_edges_out += best_fill;
      }
      if (dp_work_out != NULL && *dp_work_out != UINT64_MAX) {
        /* The bag eliminated at this step has best_degree + 1 variables. */
        const uint64_t entries =
            best_degree >= 63U ? UINT64_MAX : (UINT64_C(1) << (best_degree + 1U));
        if (entries == UINT64_MAX || *dp_work_out > UINT64_MAX - entries) {
          *dp_work_out = UINT64_MAX;
        } else {
          *dp_work_out += entries;
        }
      }

      /* Eliminate `best`: drop it from each neighbour, then make N(best) a clique. */
      adjlist_t *nb = &adj[best];
      for (uint32_t i = 0; i < nb->len; i++) {
        adjlist_remove(&adj[nb->data[i]], best);
      }
      for (uint32_t i = 0; i < nb->len && ok; i++) {
        const uint32_t u = nb->data[i];
        adjlist_t *au = &adj[u];
        const uint64_t clique_stamp = ++stamp;
        for (uint32_t k = 0; k < au->len; k++) {
          mark[au->data[k]] = clique_stamp;
        }
        for (uint32_t j = 0; j < nb->len; j++) {
          const uint32_t w = nb->data[j];
          if (w != u && mark[w] != clique_stamp) {
            mark[w] = clique_stamp; /* guards against duplicate pushes within this pass */
            if (!adjlist_push(au, w)) {
              ok = false;
              break;
            }
          }
        }
      }
      active[best] = false;
      degv[best] = 0;
      fillv[best] = 0;
      if (!ok) {
        break;
      }

      /* Recompute (fill,degree) for the touched set: N(best) plus their neighbours — a superset
       * of every vertex whose fill or degree can change this step. */
      const uint64_t touch_stamp = ++stamp;
      uint32_t ntouched = 0;
      for (uint32_t i = 0; i < nb->len; i++) {
        const uint32_t u = nb->data[i];
        if (active[u] && mark[u] != touch_stamp) {
          mark[u] = touch_stamp;
          touched[ntouched++] = u;
        }
        const adjlist_t *au = &adj[u];
        for (uint32_t k = 0; k < au->len; k++) {
          const uint32_t w = au->data[k];
          if (active[w] && mark[w] != touch_stamp) {
            mark[w] = touch_stamp;
            touched[ntouched++] = w;
          }
        }
      }
      for (uint32_t i = 0; i < ntouched; i++) {
        const uint32_t t = touched[i];
        degv[t] = adj[t].len;
        fillv[t] = min_fill_vertex_fill(t, adj, mark, ++stamp);
      }
    }

    if (width_out != NULL) {
      *width_out = width;
    }
  } else {
    min_fill_set_error(error, "out of memory while computing min-fill order");
  }

  if (adj != NULL) {
    for (uint32_t v = 0; v < nvars; v++) {
      free(adj[v].data);
    }
  }
  free(adj);
  free(fillv);
  free(degv);
  free(active);
  free(mark);
  free(touched);
  return ok;
}
