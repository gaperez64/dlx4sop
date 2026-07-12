#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* The parser keeps its own file-static copy of this helper; mirror it here so the
 * simplifier can fold coefficients mod r without reaching into parser internals. */
static uint64_t add_mod_u64(uint64_t a, uint64_t b, uint64_t r) {
  __extension__ typedef unsigned __int128 uint128_t;
  return (uint64_t)(((uint128_t)a + (uint128_t)b) % (uint128_t)r);
}

static uint64_t sub_mod_u64(uint64_t a, uint64_t b, uint64_t r) {
  return add_mod_u64(a % r, r - (b % r), r);
}

typedef struct simplify_edge {
  uint32_t u;
  uint32_t v;
} simplify_edge_t;

static int compare_simplify_edges(const void *left, const void *right) {
  const simplify_edge_t *a = left;
  const simplify_edge_t *b = right;
  if (a->u != b->u) {
    return a->u < b->u ? -1 : 1;
  }
  if (a->v != b->v) {
    return a->v < b->v ? -1 : 1;
  }
  return 0;
}

/* Sort edges by (min,max), fold self-loops into unary (x^2 == x), and drop parity-cancelling
 * duplicates in place. The elimination pass below assumes this shape on entry so that the
 * adjacency lists it builds reflect the true active degrees -- the degree-0 rules are unsound
 * otherwise. */
static bool canonicalize_edges(qsop_instance_t *inst, uint64_t sign_coeff) {
  simplify_edge_t *edges = malloc((inst->nedges == 0U ? 1U : inst->nedges) * sizeof(*edges));
  if (edges == NULL) {
    return false;
  }

  uint32_t m = 0;
  for (uint32_t i = 0; i < inst->nedges; i++) {
    const uint32_t u = inst->edge_u[i];
    const uint32_t v = inst->edge_v[i];
    if (u == v) {
      inst->unary[u] = add_mod_u64(inst->unary[u], sign_coeff, inst->r);
      continue;
    }
    edges[m].u = u < v ? u : v;
    edges[m].v = u < v ? v : u;
    m++;
  }
  if (m > 1U) {
    qsort(edges, m, sizeof(*edges), compare_simplify_edges);
  }

  uint32_t kept = 0;
  for (uint32_t i = 0; i < m;) {
    const simplify_edge_t e = edges[i];
    uint32_t parity = 0;
    do {
      parity ^= 1U;
      i++;
    } while (i < m && edges[i].u == e.u && edges[i].v == e.v);
    if (parity != 0U) {
      edges[kept] = e;
      kept++;
    }
  }

  for (uint32_t i = 0; i < kept; i++) {
    inst->edge_u[i] = edges[i].u;
    inst->edge_v[i] = edges[i].v;
  }
  inst->nedges = kept;
  free(edges);
  return true;
}

/* The canonical zero-amplitude witness: one isolated variable carrying unary r/2, whose factor is
 * 1 + omega^(r/2) = 0. Every backend evaluates it to exactly zero without any search, so a proof
 * that the amplitude vanishes is expressed as an instance rather than as an out-of-band flag. */
static bool collapse_to_zero(qsop_instance_t *inst, uint64_t sign_coeff) {
  uint64_t *unary = malloc(sizeof(*unary));
  uint32_t *edge_u = malloc(sizeof(*edge_u));
  uint32_t *edge_v = malloc(sizeof(*edge_v));
  if (unary == NULL || edge_u == NULL || edge_v == NULL) {
    free(unary);
    free(edge_u);
    free(edge_v);
    return false;
  }

  unary[0] = sign_coeff;
  free(inst->unary);
  free(inst->edge_u);
  free(inst->edge_v);
  inst->unary = unary;
  inst->edge_u = edge_u;
  inst->edge_v = edge_v;
  inst->nvars = 1U;
  inst->nedges = 0U;
  inst->constant = 0U;
  return true;
}

/* --- Incremental adjacency, mirroring src/core/min_fill.c's adjlist_t -------------------- */

typedef struct hadamard_adjlist {
  uint32_t *data;
  uint32_t len;
  uint32_t cap;
} hadamard_adjlist_t;

static bool hadamard_adjlist_push(hadamard_adjlist_t *a, uint32_t x) {
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

static bool hadamard_adjlist_contains(const hadamard_adjlist_t *a, uint32_t x) {
  for (uint32_t i = 0; i < a->len; i++) {
    if (a->data[i] == x) {
      return true;
    }
  }
  return false;
}

static void hadamard_adjlist_remove(hadamard_adjlist_t *a, uint32_t x) {
  for (uint32_t i = 0; i < a->len; i++) {
    if (a->data[i] == x) {
      a->data[i] = a->data[a->len - 1U];
      a->len--;
      return;
    }
  }
}

/* --- A tiny binary min-heap of variable indices, with lazy staleness checks -------------- */

typedef struct uint32_heap {
  uint32_t *data;
  uint32_t len;
  uint32_t cap;
} uint32_heap_t;

static bool heap_push(uint32_heap_t *h, uint32_t x) {
  if (h->len == h->cap) {
    const uint32_t ncap = h->cap == 0U ? 16U : h->cap * 2U;
    uint32_t *nd = realloc(h->data, (size_t)ncap * sizeof(*nd));
    if (nd == NULL) {
      return false;
    }
    h->data = nd;
    h->cap = ncap;
  }
  uint32_t i = h->len++;
  h->data[i] = x;
  while (i > 0U) {
    const uint32_t parent = (i - 1U) / 2U;
    if (h->data[parent] <= h->data[i]) {
      break;
    }
    const uint32_t tmp = h->data[parent];
    h->data[parent] = h->data[i];
    h->data[i] = tmp;
    i = parent;
  }
  return true;
}

static bool heap_pop(uint32_heap_t *h, uint32_t *out) {
  if (h->len == 0U) {
    return false;
  }
  *out = h->data[0];
  h->len--;
  h->data[0] = h->data[h->len];
  uint32_t i = 0;
  for (;;) {
    const uint32_t left = 2U * i + 1U;
    const uint32_t right = 2U * i + 2U;
    uint32_t smallest = i;
    if (left < h->len && h->data[left] < h->data[smallest]) {
      smallest = left;
    }
    if (right < h->len && h->data[right] < h->data[smallest]) {
      smallest = right;
    }
    if (smallest == i) {
      break;
    }
    const uint32_t tmp = h->data[i];
    h->data[i] = h->data[smallest];
    h->data[smallest] = tmp;
    i = smallest;
  }
  return true;
}

/* An isolated variable (degree 0) with unary == sign_coeff is the zero-amplitude witness: it
 * takes absolute priority over every ordinary elimination, exactly like the historical per-round
 * rescan's unconditional check before it ever looked for an ordinary candidate. Excluded from
 * eligible_ordinary/the heap so it never gets silently dropped as a plain degree-0 removal;
 * instead the caller checks for it explicitly, immediately, every time a variable's degree or
 * unary changes (see is_witness's call sites) -- norm_h is not touched by collapsing to the
 * witness, so exactly how many ordinary eliminations already ran before it is found is part of
 * the observable result and must match the reference round-by-round timing. */
static bool is_witness(const qsop_instance_t *inst, const hadamard_adjlist_t *adj, uint64_t sign_coeff,
                       uint32_t x) {
  return adj[x].len == 0U && inst->unary[x] == sign_coeff;
}

/* Eligible for ordinary elimination: degree <= 2 and unary in {0, sign_coeff} (s in {0,1} from
 * the derivation below), excluding the zero-witness combination above. */
static bool eligible_ordinary(const qsop_instance_t *inst, const hadamard_adjlist_t *adj,
                              const bool *removed, uint64_t sign_coeff, uint32_t x) {
  if (removed[x]) {
    return false;
  }
  const uint32_t deg = adj[x].len;
  if (deg > 2U) {
    return false;
  }
  if (inst->unary[x] % sign_coeff != 0U) {
    return false;
  }
  return !is_witness(inst, adj, sign_coeff, x);
}

/* Called whenever x's degree or unary just changed: notes the zero-witness condition (which
 * takes over immediately, so it is never queued as an ordinary elimination) or queues x for
 * ordinary elimination if it newly qualifies. */
static bool note_variable_changed(uint32_heap_t *heap, const qsop_instance_t *inst,
                                  const hadamard_adjlist_t *adj, const bool *removed,
                                  uint64_t sign_coeff, uint32_t x, bool *witness_found) {
  if (removed[x]) {
    return true;
  }
  if (is_witness(inst, adj, sign_coeff, x)) {
    *witness_found = true;
    return true;
  }
  if (!eligible_ordinary(inst, adj, removed, sign_coeff, x)) {
    return true;
  }
  return heap_push(heap, x);
}

/* Eliminate one degree<=2 variable `v` (see the derivation in qsop_simplify_hadamard's header
 * comment below): fold its linear-phase substitution effects into its neighbours' unary/constant
 * terms in place, then rewire its neighbourhood in the adjacency lists (no array rebuild). Pushes
 * every variable whose degree or unary changed onto `heap` for re-examination, and sets
 * *witness_found if any of them is now the zero-amplitude witness (checked eagerly rather than
 * deferred -- see is_witness). `scratch` is a caller-owned scratch buffer (reused across calls)
 * sized to at least the largest degree seen. */
static bool eliminate_variable(qsop_instance_t *inst, hadamard_adjlist_t *adj, bool *removed,
                               uint64_t sign_coeff, uint32_t v, uint32_heap_t *heap,
                               uint32_t **scratch, uint32_t *scratch_cap, bool *witness_found) {
  const uint32_t vdeg = adj[v].len;
  const bool negate = inst->unary[v] == sign_coeff;

  uint32_t keep = UINT32_MAX;
  uint32_t drop = UINT32_MAX;
  bool merge_mode = false;
  if (vdeg == 2U) {
    const uint32_t a = adj[v].data[0];
    const uint32_t b = adj[v].data[1];
    keep = a < b ? a : b;
    drop = a < b ? b : a;
    merge_mode = true;
  } else if (vdeg == 1U) {
    drop = adj[v].data[0];
  }

  if (merge_mode && keep == drop) {
    removed[v] = true; /* defensive: canonical form never yields parallel edges */
    return true;
  }

  if (drop != UINT32_MAX) {
    /* Snapshot drop's neighbours before mutating adj[drop]: the loop below both reads and
     * rewires it. */
    const uint32_t dcap = adj[drop].len;
    if (dcap > *scratch_cap) {
      uint32_t *ns = realloc(*scratch, (size_t)dcap * sizeof(**scratch));
      if (ns == NULL) {
        return false;
      }
      *scratch = ns;
      *scratch_cap = dcap;
    }
    uint32_t dn = 0;
    for (uint32_t i = 0; i < adj[drop].len; i++) {
      (*scratch)[dn++] = adj[drop].data[i];
    }

    if (merge_mode) {
      if (negate) {
        inst->constant = add_mod_u64(inst->constant, inst->unary[drop], inst->r);
        inst->unary[keep] = sub_mod_u64(inst->unary[keep], inst->unary[drop], inst->r);
      } else {
        inst->unary[keep] = add_mod_u64(inst->unary[keep], inst->unary[drop], inst->r);
      }
    } else {
      /* vdeg == 1: drop is pinned to a fixed boolean, not merged into another variable. */
      if (negate) {
        inst->constant = add_mod_u64(inst->constant, inst->unary[drop], inst->r);
      }
      /* !negate: x_drop := 0 makes every term touching drop vanish; nothing to fold. */
    }

    for (uint32_t i = 0; i < dn; i++) {
      const uint32_t w = (*scratch)[i];
      if (w == v) {
        continue;
      }
      hadamard_adjlist_remove(&adj[drop], w);
      hadamard_adjlist_remove(&adj[w], drop);
      if (merge_mode && w == keep) {
        /* The (keep,drop) chord folds to x_keep*(1-x_keep) == 0 under negate (nothing to add),
         * or to x_keep*x_keep == x_keep under a plain merge (adds sign_coeff to keep itself). */
        if (!negate) {
          inst->unary[keep] = add_mod_u64(inst->unary[keep], sign_coeff, inst->r);
        }
        if (!note_variable_changed(heap, inst, adj, removed, sign_coeff, keep, witness_found)) {
          return false;
        }
        continue;
      }
      if (negate) {
        inst->unary[w] = add_mod_u64(inst->unary[w], sign_coeff, inst->r);
      }
      if (merge_mode) {
        if (hadamard_adjlist_contains(&adj[keep], w)) {
          /* keep was already adjacent to w: the rewired edge doubles it, and 2*(r/2) == 0 mod
           * r cancels both copies -- matches canonicalize_edges' parity dedup. */
          hadamard_adjlist_remove(&adj[keep], w);
          hadamard_adjlist_remove(&adj[w], keep);
        } else {
          if (!hadamard_adjlist_push(&adj[keep], w) || !hadamard_adjlist_push(&adj[w], keep)) {
            return false;
          }
        }
      }
      if (!note_variable_changed(heap, inst, adj, removed, sign_coeff, w, witness_found)) {
        return false;
      }
    }

    hadamard_adjlist_remove(&adj[drop], v);
    if (merge_mode) {
      hadamard_adjlist_remove(&adj[keep], v);
    }
    removed[drop] = true;
  }

  removed[v] = true;
  inst->norm_h -= 2U;
  if (merge_mode) {
    if (!note_variable_changed(heap, inst, adj, removed, sign_coeff, keep, witness_found)) {
      return false;
    }
  }
  return true;
}

/* Because omega^(r/2) = -1, a variable v whose unary coefficient is a multiple of r/2 -- that is,
 * unary[v] in {0, r/2} -- factors out of the amplitude exactly. Writing s = 2*unary[v]/r in {0,1}
 * and S for the sum of v's neighbours,
 *
 *   sum_{x_v} omega^{x_v*(unary[v] + (r/2)*S)} = 1 + (-1)^(s + S) = 2 * [S == s (mod 2)],
 *
 * so summing v out scales the amplitude by 2 and imposes the XOR constraint S == s. This pass
 * applies the three cases where that constraint is cheap to absorb back into a qsop-sign instance:
 *
 *   degree 0: S = 0, so the factor is 2 when s == 0 (drop v) and 0 when s == 1 (the whole
 *             amplitude vanishes -- collapse to the witness above).
 *   degree 1, edge (a,v):        constraint a == s          -> pin a := s.
 *   degree 2, edges (a,v),(v,b): constraint a XOR b == s    -> substitute b := a XOR s.
 *
 * s == 0 recovers the historical unary[v] == 0 rules. s == 1 is the sign-carrying twin: pinning to
 * 1 folds unary[a] into the constant and flips each neighbour's unary by r/2, and the negated merge
 * b := 1 - a folds unary[b] into the constant, subtracts it from unary[a], and flips the unary of
 * every other neighbour of b (since (r/2)*(1-a)*w = (r/2)*w + (r/2)*a*w mod r).
 *
 * Eliminations are processed lowest-index-first via a min-heap of currently-eligible candidates
 * (re-validated at pop time against the live adjacency, since a candidate's degree/unary can
 * change or go stale between being queued and being popped) -- the same variable-at-a-time order
 * the historical per-round rescan used, but each step edits the adjacency lists in place instead
 * of rebuilding every array from scratch, so cost is O(elimination-local work) instead of
 * O(nvars+nedges) per elimination. Variables/edges are only renumbered and rebuilt once, at the
 * very end. The amplitude doubling per elimination is compensated by norm_h -= 2, which keeps both
 * probability |amp|^2 * 2^-norm_h and the normalized amplitude amp * 2^(-norm_h/2) exact; the
 * guard norm_h >= 2 prevents underflow. */
bool qsop_simplify_hadamard(qsop_instance_t *inst) {
  if (inst == NULL) {
    return true;
  }
  if (inst->r < 2U || (inst->r % 2U) != 0U) {
    return true; /* the sign format requires an even modulus; nothing safe to fold otherwise */
  }
  const uint64_t sign_coeff = inst->r / 2U;
  if (!canonicalize_edges(inst, sign_coeff)) {
    return false;
  }

  const uint32_t n = inst->nvars;
  if (n == 0U) {
    return true;
  }

  hadamard_adjlist_t *adj = calloc(n, sizeof(*adj));
  bool *removed = calloc(n, sizeof(*removed));
  bool ok = adj != NULL && removed != NULL;
  if (ok) {
    for (uint32_t i = 0; i < inst->nedges && ok; i++) {
      ok = hadamard_adjlist_push(&adj[inst->edge_u[i]], inst->edge_v[i]) &&
           hadamard_adjlist_push(&adj[inst->edge_v[i]], inst->edge_u[i]);
    }
  }

  /* Seeding this way (rather than a separate upfront witness scan) matches the historical
   * per-round check exactly: if any variable is already the witness before a single elimination
   * has run, witness_found comes out true here and the loop below never executes at all -- same
   * as the reference finding it on round one, with norm_h untouched. */
  uint32_heap_t heap = {0};
  bool witness_found = false;
  if (ok) {
    for (uint32_t x = 0; x < n && ok; x++) {
      ok = note_variable_changed(&heap, inst, adj, removed, sign_coeff, x, &witness_found);
    }
  }

  uint32_t *scratch = NULL;
  uint32_t scratch_cap = 0;
  while (ok && !witness_found && inst->norm_h >= 2U) {
    uint32_t v;
    if (!heap_pop(&heap, &v)) {
      break;
    }
    if (!eligible_ordinary(inst, adj, removed, sign_coeff, v)) {
      continue; /* stale: already removed, or degree/unary moved since it was queued */
    }
    ok = eliminate_variable(inst, adj, removed, sign_coeff, v, &heap, &scratch, &scratch_cap,
                            &witness_found);
  }
  free(scratch);
  free(heap.data);

  uint32_t live_count = 0;
  if (ok) {
    for (uint32_t x = 0; x < n; x++) {
      if (!removed[x]) {
        live_count++;
      }
    }
  }

  if (ok && witness_found && !(live_count == 1U && inst->constant == 0U)) {
    ok = collapse_to_zero(inst, sign_coeff);
  } else if (ok) {
    /* Final compaction: renumber surviving variables in original index order (matching the
     * historical per-round `newid` assignment byte-for-byte) and rebuild the edge list, sorted,
     * from the live adjacency -- this is the only O(nvars+nedges) pass in the whole function. */
    uint32_t *newid = malloc((size_t)n * sizeof(*newid));
    ok = newid != NULL;
    uint32_t k = 0;
    if (ok) {
      for (uint32_t x = 0; x < n; x++) {
        newid[x] = removed[x] ? UINT32_MAX : k++;
      }
    }
    uint64_t *new_unary = ok ? calloc(k == 0U ? 1U : k, sizeof(*new_unary)) : NULL;
    ok = ok && new_unary != NULL;
    if (ok) {
      for (uint32_t x = 0; x < n; x++) {
        if (!removed[x]) {
          new_unary[newid[x]] = inst->unary[x];
        }
      }
    }
    simplify_edge_t *edges = NULL;
    uint32_t medges = 0;
    if (ok) {
      uint32_t cap = 0;
      for (uint32_t x = 0; x < n; x++) {
        if (!removed[x]) {
          cap += adj[x].len;
        }
      }
      edges = malloc((cap == 0U ? 1U : cap / 2U + 1U) * sizeof(*edges));
      ok = edges != NULL;
      if (ok) {
        for (uint32_t x = 0; x < n && ok; x++) {
          if (removed[x]) {
            continue;
          }
          for (uint32_t i = 0; i < adj[x].len; i++) {
            const uint32_t y = adj[x].data[i];
            if (y <= x) {
              continue; /* emit each undirected edge once, from its lower-indexed endpoint */
            }
            edges[medges].u = newid[x];
            edges[medges].v = newid[y];
            medges++;
          }
        }
      }
    }
    if (ok && medges > 1U) {
      qsort(edges, medges, sizeof(*edges), compare_simplify_edges);
    }
    uint32_t *new_edge_u = ok ? malloc((medges == 0U ? 1U : medges) * sizeof(*new_edge_u)) : NULL;
    uint32_t *new_edge_v = ok ? malloc((medges == 0U ? 1U : medges) * sizeof(*new_edge_v)) : NULL;
    ok = ok && new_edge_u != NULL && new_edge_v != NULL;
    if (ok) {
      for (uint32_t i = 0; i < medges; i++) {
        new_edge_u[i] = edges[i].u;
        new_edge_v[i] = edges[i].v;
      }
      free(inst->unary);
      free(inst->edge_u);
      free(inst->edge_v);
      inst->unary = new_unary;
      inst->edge_u = new_edge_u;
      inst->edge_v = new_edge_v;
      inst->nvars = k;
      inst->nedges = medges;
    } else {
      free(new_unary);
      free(new_edge_u);
      free(new_edge_v);
    }
    free(newid);
    free(edges);
  }

  for (uint32_t x = 0; x < n; x++) {
    free(adj[x].data);
  }
  free(adj);
  free(removed);
  return ok;
}
