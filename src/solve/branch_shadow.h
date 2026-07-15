#ifndef DLX4SOP_BRANCH_SHADOW_H
#define DLX4SOP_BRANCH_SHADOW_H

/* Shadow-graph branching heuristic for the branch backend's single-Fourier cutset-conditioning
 * path (see branch.c's branch_choose_cutset_candidate).
 *
 * A shadow graph is a plain, unlabelled simple graph on the *same* variable IDs as an active
 * qsop_residual_t, built by dropping coefficients/phases and keeping only the active adjacency.
 * Exhaustively eliminating degree-<=2 vertices (a pendant/isolated vertex is just dropped; a
 * degree-2 vertex is dropped and its two neighbours get a virtual "fill" edge) collapses long
 * gadget chains -- e.g. an imported QNN circuit's `x, y -> check -> value` motif, where `value`
 * has degree 1 and `check` then has degree 2 -- down to a much smaller logical graph on the
 * surviving hub variables, without ever increasing width (series reduction cannot create fill-in
 * the way general min-fill elimination can).
 *
 * This module produces a *shortlist* of original variable IDs worth branching on next -- nothing
 * more. It never decides which value to branch to, never materializes a decomposition, and is
 * never itself handed to the treewidth or rankwidth backend: the shadow graph's own apparent
 * width is not a correctness or memory-safety certificate for the real (labelled) residual, which
 * can have a much larger cut rank than its shadow. branch.c is responsible for evaluating every
 * shortlisted candidate against the *real* residual with the existing lookahead machinery and
 * picking the final winner from that -- this module only narrows the search. */

#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residual.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct branch_shadow_adjlist {
  uint32_t *data;
  uint32_t len;
  uint32_t cap;
} branch_shadow_adjlist_t;

typedef struct branch_shadow {
  uint32_t nvars; /* fixed index space: same variable IDs as the source residual/edge list */
  bool *alive;
  branch_shadow_adjlist_t *adj; /* simple: no self-loops, no duplicate neighbours */
  uint32_t alive_vars;
  uint64_t alive_edges;
} branch_shadow_t;

void branch_shadow_free(branch_shadow_t *shadow);

/* Raw-graph constructor, decoupled from qsop_residual_t (mirrors qsop_min_fill_eliminate's
 * contract): every vertex in 0..nvars-1 starts alive; self-loops and duplicate edges are
 * silently dropped. Intended for unit tests exercising the reduction/scoring logic directly on
 * hand-built graphs. */
bool branch_shadow_build_from_edges(uint32_t nvars, const uint32_t *edge_u, const uint32_t *edge_v,
                                    uint32_t nedges, branch_shadow_t *out, qsop_error_t *error);

/* Builds a shadow graph from the *active* portion of a residual: alive[v] iff
 * qsop_residual_var_active(residual, v), and only active edges between active endpoints are
 * added. Uses only the public residual accessors (there is no cheaper "active edges only"
 * iterator available outside residual.c, so this is one dense O(nvars + nedges) scan). */
bool branch_shadow_build(const qsop_residual_t *residual, branch_shadow_t *out, qsop_error_t *error);

bool branch_shadow_clone(const branch_shadow_t *src, branch_shadow_t *out, qsop_error_t *error);

/* No-op if u == v or either endpoint is out of range or already adjacent (simple graph). */
bool branch_shadow_add_edge(branch_shadow_t *shadow, uint32_t u, uint32_t v, qsop_error_t *error);

/* No-op if v is out of range or already dead. Removes v from every live neighbour's adjacency. */
void branch_shadow_remove_vertex(branch_shadow_t *shadow, uint32_t v);

/* Exhaustively eliminates every alive vertex of degree <= 2: a degree-0/1 vertex is dropped; a
 * degree-2 vertex is dropped and a virtual fill edge is added between its two (necessarily
 * distinct, since adjacency is simple) neighbours, cascading through any neighbour whose degree
 * drops to <= 2 as a result. Deterministic (worklist seeded in ascending vertex-ID order); the
 * survivor set does not depend on processing order, only which fill edges get added along the
 * way, so this needs no fill-minimizing heap unlike src/core/min_fill.c's elimination. Graph
 * reduction only -- never touches a residual. */
bool branch_shadow_series_reduce(branch_shadow_t *shadow, qsop_error_t *error);

/* Connected-component count and largest-component size over the alive vertices, via an explicit
 * (non-recursive) stack. */
bool branch_shadow_component_stats(const branch_shadow_t *shadow, uint32_t *out_components,
                                   uint32_t *out_largest, qsop_error_t *error);

typedef struct branch_shadow_candidate {
  uint32_t var;
  uint32_t remaining_vars;
  uint64_t remaining_edges;
  uint32_t components;
  uint32_t largest_component;
  uint32_t eliminated_vars; /* parent alive_vars - child alive_vars, including var itself */
  uint32_t original_degree; /* var's degree in the parent (pre-removal) reduced shadow */
} branch_shadow_candidate_t;

/* Lexicographic: smaller largest_component -> smaller remaining_vars -> smaller remaining_edges
 * -> larger eliminated_vars -> larger original_degree -> smaller var (deterministic tiebreak). */
bool branch_shadow_candidate_better(const branch_shadow_candidate_t *a,
                                    const branch_shadow_candidate_t *b);

/* Remove-and-rereduce lookahead: clones `reduced`, removes `v`, reruns series reduction, and
 * scores the result. Operates entirely on the (cloned) shadow graph -- never touches a residual,
 * so needs no checkpoint/undo discipline. */
bool branch_shadow_score_candidate(const branch_shadow_t *reduced, uint32_t v,
                                   branch_shadow_candidate_t *out, qsop_error_t *error);

/* Defensive budgets: a residual/shadow this large is not what the heuristic was designed for
 * (delegate-probe failure means the residual is too *wide*, not too *small* -- a huge, low-width
 * residual can still reach cutset conditioning), and rebuilding a shadow from scratch at every
 * cutset node (no incremental caching in this first patch) means a per-node dense scan whose cost
 * must stay bounded regardless of mode. */
#define BRANCH_SHADOW_MAX_SOURCE_VARS 20000U
#define BRANCH_SHADOW_MAX_SOURCE_EDGES 200000U
#define BRANCH_SHADOW_MAX_CANDIDATE_EVALS 64U

/* Builds and reduces the shadow, scores up to BRANCH_SHADOW_MAX_CANDIDATE_EVALS surviving
 * vertices (pre-filtered by current shadow degree, descending, when the reduced core is larger
 * than that), and returns the best `limit` original variable IDs via a caller-owned *out_vars
 * (freed with free()).
 *
 * Budget-exceeded, an empty reduced core, and (defensively) every returned ID failing the
 * "still active in the residual" check all return true with *out_len == 0 -- not an error; the
 * caller is expected to fall back to its existing candidate selection. Only allocation failure
 * returns false. stats (nullable) accumulates branch_shadow_builds/skips/max_source_vars/
 * max_core_vars/build_ns. */
bool branch_shadow_shortlist(const qsop_residual_t *residual, uint32_t limit, uint32_t **out_vars,
                             uint32_t *out_len, qsop_solve_stats_t *stats, qsop_error_t *error);

#endif
