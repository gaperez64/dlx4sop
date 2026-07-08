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

/* Perform one elimination round.
 *
 * Because omega^(r/2) = -1, summing a free variable v with unary[v]==0 out of the amplitude
 * is an exact identity that always scales the amplitude by 2, compensated by norm_h -= 2:
 *   degree-2, edges (a,v),(v,b): sum_v omega^{(r/2)(a*v+v*b)} = 2*[a==b]  -> merge b into a.
 *   degree-1, edge (a,v):        sum_v omega^{(r/2)(a*v)}     = 2*[a==0]  -> pin a := 0.
 * Both remove two variables. The guard norm_h>=2 keeps probability |amp|^2*2^-norm_h exact
 * (and prevents underflow). On success *changed is set and the instance is rebuilt in
 * canonical form; on allocation failure returns false with the instance left untouched. */
static bool simplify_round(qsop_instance_t *inst, uint64_t sign_coeff, bool *changed) {
  *changed = false;
  const uint32_t n = inst->nvars;
  if (inst->norm_h < 2U || n == 0U) {
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

  uint32_t v = UINT32_MAX;
  for (uint32_t x = 0; x < n; x++) {
    if (inst->unary[x] == 0U && (degree[x] == 1U || degree[x] == 2U)) {
      v = x;
      break;
    }
  }
  if (v == UINT32_MAX) {
    free(degree);
    return true; /* nothing eligible: *changed stays false, fixpoint reached */
  }
  const uint32_t vdeg = degree[v];
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

  bool merge_mode; /* true: degree-2 merge (drop->keep); false: degree-1 pin (drop := 0). */
  uint32_t keep = UINT32_MAX;
  uint32_t drop;
  if (vdeg == 2U) {
    keep = nbr[0] < nbr[1] ? nbr[0] : nbr[1];
    drop = nbr[0] < nbr[1] ? nbr[1] : nbr[0];
    merge_mode = true;
    if (keep == drop) {
      return true; /* defensive: canonical form should never yield parallel edges */
    }
  } else {
    drop = nbr[0];
    merge_mode = false;
  }

  /* removed[x] marks variables that get no new id: always v, plus the second removed
   * variable (the merged neighbor in degree-2, or the pinned neighbor in degree-1). */
  bool *removed = calloc(n, sizeof(*removed));
  uint32_t *newid = malloc((size_t)n * sizeof(*newid));
  if (removed == NULL || newid == NULL) {
    free(removed);
    free(newid);
    return false;
  }
  removed[v] = true;
  removed[drop] = true;

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
  simplify_edge_t *edges = malloc((inst->nedges == 0U ? 1U : inst->nedges) * sizeof(*edges));
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
  if (merge_mode) {
    /* fold the merged neighbor's linear phase onto the survivor (substituting x_drop:=x_keep) */
    new_unary[newid[keep]] = add_mod_u64(new_unary[newid[keep]], inst->unary[drop], inst->r);
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
      /* a pre-existing chord between the merged endpoints becomes a self-loop; x^2==x */
      new_unary[nu] = add_mod_u64(new_unary[nu], sign_coeff, inst->r);
      continue;
    }
    edges[medges].u = nu < nw ? nu : nw;
    edges[medges].v = nu < nw ? nw : nu;
    medges++;
  }

  if (medges > 1U) {
    qsort(edges, medges, sizeof(*edges), compare_simplify_edges);
  }
  uint32_t kept_edges = 0;
  for (uint32_t i = 0; i < medges;) {
    const simplify_edge_t e = edges[i];
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
  inst->norm_h -= 2U;
  *changed = true;
  return true;
}

bool qsop_simplify_hadamard(qsop_instance_t *inst) {
  if (inst == NULL) {
    return true;
  }
  if (inst->r < 2U || (inst->r % 2U) != 0U) {
    return true; /* the sign format requires an even modulus; nothing safe to fold otherwise */
  }
  const uint64_t sign_coeff = inst->r / 2U;
  for (;;) {
    bool changed = false;
    if (!simplify_round(inst, sign_coeff, &changed)) {
      return false;
    }
    if (!changed) {
      break;
    }
  }
  return true;
}
