/* Test-only oracle: the pre-2026-07-12 qsop_simplify_hadamard, kept verbatim (functions renamed
 * with a ref_ prefix, and the public entry point renamed to qsop_simplify_hadamard_reference) so
 * test_qsop_simplify_differential.c can fuzz it against the live adjacency-incremental
 * implementation in src/core/qsop_simplify.c. Correct but O(eliminations * (nvars+nedges)):
 * rebuilds every array from scratch per single-variable elimination. Never call this from
 * anything but that one test. */
#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

bool qsop_simplify_hadamard_reference(qsop_instance_t *inst);

/* The parser keeps its own file-static copy of this helper; mirror it here so the
 * simplifier can fold coefficients mod r without reaching into parser internals. */
static uint64_t ref_add_mod_u64(uint64_t a, uint64_t b, uint64_t r) {
  __extension__ typedef unsigned __int128 uint128_t;
  return (uint64_t)(((uint128_t)a + (uint128_t)b) % (uint128_t)r);
}

static uint64_t ref_sub_mod_u64(uint64_t a, uint64_t b, uint64_t r) {
  return ref_add_mod_u64(a % r, r - (b % r), r);
}

typedef struct ref_simplify_edge {
  uint32_t u;
  uint32_t v;
} ref_simplify_edge_t;

static int ref_compare_simplify_edges(const void *left, const void *right) {
  const ref_simplify_edge_t *a = left;
  const ref_simplify_edge_t *b = right;
  if (a->u != b->u) {
    return a->u < b->u ? -1 : 1;
  }
  if (a->v != b->v) {
    return a->v < b->v ? -1 : 1;
  }
  return 0;
}

/* Sort edges by (min,max), fold self-loops into unary (x^2 == x), and drop parity-cancelling
 * duplicates in place. ref_simplify_round assumes this shape on entry so that the degree counts it
 * derives are the true active degrees -- the degree-0 rules below are unsound otherwise. */
static bool ref_canonicalize_edges(qsop_instance_t *inst, uint64_t sign_coeff) {
  ref_simplify_edge_t *edges = malloc((inst->nedges == 0U ? 1U : inst->nedges) * sizeof(*edges));
  if (edges == NULL) {
    return false;
  }

  uint32_t m = 0;
  for (uint32_t i = 0; i < inst->nedges; i++) {
    const uint32_t u = inst->edge_u[i];
    const uint32_t v = inst->edge_v[i];
    if (u == v) {
      inst->unary[u] = ref_add_mod_u64(inst->unary[u], sign_coeff, inst->r);
      continue;
    }
    edges[m].u = u < v ? u : v;
    edges[m].v = u < v ? v : u;
    m++;
  }
  if (m > 1U) {
    qsort(edges, m, sizeof(*edges), ref_compare_simplify_edges);
  }

  uint32_t kept = 0;
  for (uint32_t i = 0; i < m;) {
    const ref_simplify_edge_t e = edges[i];
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
static bool ref_is_zero_witness(const qsop_instance_t *inst, uint64_t sign_coeff) {
  return inst->nvars == 1U && inst->nedges == 0U && inst->constant == 0U &&
         inst->unary[0] == sign_coeff;
}

static bool ref_collapse_to_zero(qsop_instance_t *inst, uint64_t sign_coeff) {
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

/* Perform one elimination round.
 *
 * Because omega^(r/2) = -1, a variable v whose unary coefficient is a multiple of r/2 -- that is,
 * unary[v] in {0, r/2} -- factors out of the amplitude exactly. Writing s = 2*unary[v]/r in {0,1}
 * and S for the sum of v's neighbours,
 *
 *   sum_{x_v} omega^{x_v*(unary[v] + (r/2)*S)} = 1 + (-1)^(s + S) = 2 * [S == s (mod 2)],
 *
 * so summing v out scales the amplitude by 2 and imposes the XOR constraint S == s. The pass
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
 * The amplitude doubling is compensated by norm_h -= 2, which keeps both probability
 * |amp|^2 * 2^-norm_h and the normalized amplitude amp * 2^(-norm_h/2) exact. The guard
 * norm_h >= 2 prevents underflow. On success *changed is set and the instance is rebuilt in
 * canonical form; on allocation failure returns false with the instance left untouched. */
static bool ref_simplify_round(qsop_instance_t *inst, uint64_t sign_coeff, bool *changed) {
  *changed = false;
  const uint32_t n = inst->nvars;
  if (n == 0U) {
    return true;
  }

  uint32_t *degree = calloc(n, sizeof(*degree));
  if (degree == NULL) {
    return false;
  }
  for (uint32_t i = 0; i < inst->nedges; i++) {
    degree[inst->edge_u[i]]++;
    degree[inst->edge_v[i]]++;
  }

  /* An isolated variable with unary r/2 makes the amplitude identically zero. This fires before
   * the norm_h guard: the conclusion holds whatever the normalization is. */
  for (uint32_t x = 0; x < n; x++) {
    if (degree[x] == 0U && inst->unary[x] == sign_coeff) {
      free(degree);
      if (ref_is_zero_witness(inst, sign_coeff)) {
        return true; /* already the witness: fixpoint */
      }
      if (!ref_collapse_to_zero(inst, sign_coeff)) {
        return false;
      }
      *changed = true;
      return true;
    }
  }

  /* Pick the lowest-index electable candidate across both families, matching the live impl's
   * lowest-index-first heap: an [HH] variable (unary a multiple of r/2, degree <= 2) is electable
   * when norm_h >= 2; an [omega] variable (unary a quarter turn r/4 or 3r/4, degree <= 2, 8 | r) is
   * electable when norm_h >= 1. A lower-index [HH] candidate at norm_h < 2 is skipped, exactly as
   * the live impl pops and discards it. */
  const bool omega_ok = (inst->r % 8U) == 0U;
  const uint64_t quarter = inst->r / 4U;
  uint32_t v = UINT32_MAX;
  bool v_is_omega = false;
  for (uint32_t x = 0; x < n; x++) {
    if (degree[x] > 2U) {
      continue;
    }
    if (inst->norm_h >= 2U && inst->unary[x] % sign_coeff == 0U) {
      v = x;
      v_is_omega = false;
      break;
    }
    if (omega_ok && inst->norm_h >= 1U &&
        (inst->unary[x] == quarter || inst->unary[x] == inst->r - quarter)) {
      v = x;
      v_is_omega = true;
      break;
    }
  }
  if (v == UINT32_MAX) {
    free(degree);
    return true; /* nothing electable: *changed stays false, fixpoint reached */
  }
  const uint32_t vdeg = degree[v];
  /* s == 1 exactly when unary[v] == r/2; the [HH] eligibility test admits only 0 and r/2. */
  const bool negate = inst->unary[v] == sign_coeff;
  free(degree);

  /* Collect v's neighbors. Canonical (parity-deduped, self-loop-free) form guarantees a
   * degree-2 variable has two distinct neighbors. */
  uint32_t nbr[2] = {UINT32_MAX, UINT32_MAX};
  uint32_t nfound = 0;
  for (uint32_t i = 0; i < inst->nedges && nfound < vdeg; i++) {
    if (inst->edge_u[i] == v) {
      nbr[nfound++] = inst->edge_v[i];
    } else if (inst->edge_v[i] == v) {
      nbr[nfound++] = inst->edge_u[i];
    }
  }

  /* [omega]: sum_{x_v} omega^{(r/4)x_v + (r/2)x_v*S} = sqrt(2)*omega^{r/8 - (r/4)Sbar}. Fold the
   * r/8 phase, the -r/4 (or +r/4 for 3r/4) turn on each neighbour, and -- at degree 2 -- a toggled
   * sign edge between the two neighbours, then drop only v with norm_h -= 1. Rebuilt below via the
   * removed[]/newid[] machinery with drop left unset and an optional extra chord appended. */
  bool omega_add_chord = false;
  uint32_t omega_a = UINT32_MAX;
  uint32_t omega_b = UINT32_MAX;
  if (v_is_omega) {
    const bool conj = inst->unary[v] == inst->r - quarter;
    const uint64_t eighth = inst->r / 8U;
    inst->constant =
        ref_add_mod_u64(inst->constant, conj ? inst->r - eighth : eighth, inst->r);
    const uint64_t neigh_turn = conj ? quarter : inst->r - quarter;
    for (uint32_t i = 0; i < vdeg; i++) {
      inst->unary[nbr[i]] = ref_add_mod_u64(inst->unary[nbr[i]], neigh_turn, inst->r);
    }
    if (vdeg == 2U) {
      omega_add_chord = true;
      omega_a = nbr[0];
      omega_b = nbr[1];
    }
  }

  bool merge_mode = false; /* true: degree-2 substitution (drop := keep XOR s). */
  uint32_t keep = UINT32_MAX;
  uint32_t drop = UINT32_MAX;
  if (!v_is_omega) {
    if (vdeg == 2U) {
      keep = nbr[0] < nbr[1] ? nbr[0] : nbr[1];
      drop = nbr[0] < nbr[1] ? nbr[1] : nbr[0];
      merge_mode = true;
      if (keep == drop) {
        return true; /* defensive: canonical form should never yield parallel edges */
      }
    } else if (vdeg == 1U) {
      drop = nbr[0]; /* degree-1 pin: drop := s */
    }
  }

  /* Apply the substitution's linear-phase effects in place; the rebuild below only has to
   * relabel and re-canonicalize the quadratic part. */
  if (merge_mode) {
    if (negate) {
      /* x_drop := 1 - x_keep. unary[drop]*x_drop = unary[drop] - unary[drop]*x_keep. */
      inst->constant = ref_add_mod_u64(inst->constant, inst->unary[drop], inst->r);
      inst->unary[keep] = ref_sub_mod_u64(inst->unary[keep], inst->unary[drop], inst->r);
      /* (r/2)*x_drop*x_w = (r/2)*x_w + (r/2)*x_keep*x_w for every other neighbour w of drop;
       * the edge (keep,drop) itself becomes (r/2)*x_keep*(1-x_keep) == 0 and just vanishes. */
      for (uint32_t i = 0; i < inst->nedges; i++) {
        uint32_t w = UINT32_MAX;
        if (inst->edge_u[i] == drop) {
          w = inst->edge_v[i];
        } else if (inst->edge_v[i] == drop) {
          w = inst->edge_u[i];
        }
        if (w != UINT32_MAX && w != keep && w != v) {
          inst->unary[w] = ref_add_mod_u64(inst->unary[w], sign_coeff, inst->r);
        }
      }
    } else {
      inst->unary[keep] = ref_add_mod_u64(inst->unary[keep], inst->unary[drop], inst->r);
    }
  } else if (vdeg == 1U && negate) {
    /* x_drop := 1: its own linear phase becomes a constant, and every incident sign edge
     * (drop,w) degenerates to the unary term (r/2)*x_w. The edge (drop,v) is skipped -- v has
     * already been summed out. */
    inst->constant = ref_add_mod_u64(inst->constant, inst->unary[drop], inst->r);
    for (uint32_t i = 0; i < inst->nedges; i++) {
      uint32_t w = UINT32_MAX;
      if (inst->edge_u[i] == drop) {
        w = inst->edge_v[i];
      } else if (inst->edge_v[i] == drop) {
        w = inst->edge_u[i];
      }
      if (w != UINT32_MAX && w != v) {
        inst->unary[w] = ref_add_mod_u64(inst->unary[w], sign_coeff, inst->r);
      }
    }
  }

  /* removed[x] marks variables that get no new id: always v, plus the second removed
   * variable (the substituted neighbor in degree-2, or the pinned neighbor in degree-1).
   * A degree-0 variable removes only itself. */
  bool *removed = calloc(n, sizeof(*removed));
  uint32_t *newid = malloc((size_t)n * sizeof(*newid));
  if (removed == NULL || newid == NULL) {
    free(removed);
    free(newid);
    return false;
  }
  removed[v] = true;
  if (drop != UINT32_MAX) {
    removed[drop] = true;
  }

  uint32_t k = 0;
  for (uint32_t x = 0; x < n; x++) {
    if (removed[x]) {
      newid[x] = UINT32_MAX;
    } else {
      newid[x] = k;
      k++;
    }
  }

  uint64_t *new_unary = calloc(k == 0U ? 1U : k, sizeof(*new_unary));
  /* +1 slot: an [omega] degree-2 elimination appends one toggled chord below. */
  ref_simplify_edge_t *edges = malloc((inst->nedges + 1U) * sizeof(*edges));
  if (new_unary == NULL || edges == NULL) {
    free(removed);
    free(newid);
    free(new_unary);
    free(edges);
    return false;
  }

  for (uint32_t x = 0; x < n; x++) {
    if (!removed[x]) {
      new_unary[newid[x]] = inst->unary[x];
    }
  }

  uint32_t medges = 0;
  for (uint32_t i = 0; i < inst->nedges; i++) {
    uint32_t eu = inst->edge_u[i];
    uint32_t ev = inst->edge_v[i];
    if (merge_mode && eu == drop) {
      eu = keep;
    }
    if (merge_mode && ev == drop) {
      ev = keep;
    }
    if (removed[eu] || removed[ev]) {
      continue; /* edges incident to the eliminated variable(s) drop out */
    }
    const uint32_t nu = newid[eu];
    const uint32_t nw = newid[ev];
    if (nu == nw) {
      /* the chord between the two merged endpoints. x_keep^2 == x_keep folds it into unary;
       * under the negated merge it is x_keep*(1-x_keep) == 0 and contributes nothing. */
      if (!merge_mode || !negate) {
        new_unary[nu] = ref_add_mod_u64(new_unary[nu], sign_coeff, inst->r);
      }
      continue;
    }
    edges[medges].u = nu < nw ? nu : nw;
    edges[medges].v = nu < nw ? nw : nu;
    medges++;
  }

  /* An [omega] degree-2 elimination toggles the chord between v's two (surviving) neighbours; add
   * it here and let the parity dedup below cancel it against an existing copy or keep it fresh. */
  if (omega_add_chord) {
    const uint32_t nu = newid[omega_a];
    const uint32_t nw = newid[omega_b];
    edges[medges].u = nu < nw ? nu : nw;
    edges[medges].v = nu < nw ? nw : nu;
    medges++;
  }

  /* Only a merge or an [omega] chord toggle can create edges, and hence duplicates or a broken
   * (u,v) order. A pin or a degree-0 removal merely deletes edges, and newid is strictly monotone,
   * so the surviving list is still sorted and parity-deduped -- re-sorting every round would make
   * the fixpoint O(eliminations * m log m) for no reason. */
  uint32_t kept_edges = medges;
  if (merge_mode || omega_add_chord) {
    if (medges > 1U) {
      qsort(edges, medges, sizeof(*edges), ref_compare_simplify_edges);
    }
    kept_edges = 0;
    for (uint32_t i = 0; i < medges;) {
      const ref_simplify_edge_t e = edges[i];
      uint32_t parity = 0;
      do {
        parity ^= 1U;
        i++;
      } while (i < medges && edges[i].u == e.u && edges[i].v == e.v);
      if (parity != 0U) {
        edges[kept_edges] = e;
        kept_edges++;
      }
    }
  }

  uint32_t *new_edge_u = malloc((kept_edges == 0U ? 1U : kept_edges) * sizeof(*new_edge_u));
  uint32_t *new_edge_v = malloc((kept_edges == 0U ? 1U : kept_edges) * sizeof(*new_edge_v));
  if (new_edge_u == NULL || new_edge_v == NULL) {
    free(removed);
    free(newid);
    free(new_unary);
    free(edges);
    free(new_edge_u);
    free(new_edge_v);
    return false;
  }
  for (uint32_t i = 0; i < kept_edges; i++) {
    new_edge_u[i] = edges[i].u;
    new_edge_v[i] = edges[i].v;
  }

  free(removed);
  free(newid);
  free(edges);
  free(inst->unary);
  free(inst->edge_u);
  free(inst->edge_v);
  inst->unary = new_unary;
  inst->edge_u = new_edge_u;
  inst->edge_v = new_edge_v;
  inst->nvars = k;
  inst->nedges = kept_edges;
  inst->norm_h -= v_is_omega ? 1U : 2U; /* [omega] spends one doubling, [HH] two */
  *changed = true;
  return true;
}

bool qsop_simplify_hadamard_reference(qsop_instance_t *inst) {
  if (inst == NULL) {
    return true;
  }
  if (inst->r < 2U || (inst->r % 2U) != 0U) {
    return true; /* the sign format requires an even modulus; nothing safe to fold otherwise */
  }
  const uint64_t sign_coeff = inst->r / 2U;
  if (!ref_canonicalize_edges(inst, sign_coeff)) {
    return false;
  }
  for (;;) {
    bool changed = false;
    if (!ref_simplify_round(inst, sign_coeff, &changed)) {
      return false;
    }
    if (!changed) {
      break;
    }
  }
  return true;
}
