#ifndef DLX4SOP_QSOP_H
#define DLX4SOP_QSOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct qsop_instance {
  uint64_t r;
  uint32_t nvars;
  uint64_t norm_h;
  uint64_t constant;

  uint64_t *unary;
  uint32_t nedges;
  uint32_t *edge_u;
  uint32_t *edge_v;
} qsop_instance_t;

typedef struct qsop_error {
  const char *path;
  size_t line;
  size_t column;
  char message[256];
} qsop_error_t;

void qsop_free(qsop_instance_t *qsop);

bool qsop_parse_file(FILE *file, const char *path, qsop_instance_t **out, qsop_error_t *error);

bool qsop_write_file(FILE *file, const qsop_instance_t *qsop, qsop_error_t *error);

/* Eliminate free variables via the path-sum reduction rules, to a fixpoint. Two families fire:
 *
 * [HH] -- unary coefficient a multiple of r/2 (that is, 0 or r/2) and incidence degree at most 2.
 * Summing such a variable out is exact because omega^(r/2) = -1: it scales the amplitude by 2 and
 * forces an XOR constraint on the variable's neighbours, absorbed back into a qsop-sign instance at
 * degree 0/1/2 as, respectively, nothing, a pin, or a (possibly negated) merge. A degree-0 variable
 * with unary r/2 has factor 1 + omega^(r/2) = 0, so the whole amplitude vanishes and the instance
 * collapses to the canonical zero witness (nvars=1, nedges=0, constant=0, unary[0]=r/2). Each such
 * elimination scales the raw amplitude by 2, compensated by norm_h -= 2.
 *
 * [omega] -- unary coefficient r/4 or 3r/4 (a quarter turn) and incidence degree at most 2, when
 * 8 | r. Summing it out gives sum_{x} omega^{(r/4)x + (r/2)x*S} = sqrt(2) * omega^{r/8 - (r/4)Sbar}
 * (with S the neighbour sum and Sbar = S mod 2): a global r/8 phase, a -r/4 quarter turn on each
 * neighbour, and -- at degree 2 -- a toggled sign edge between the two neighbours (from the
 * -(r/4)*(a+b-2ab) expansion). The 3r/4 case is the mirror (+7r/8 phase, +r/4 on neighbours). Each
 * such elimination scales the raw amplitude by sqrt(2), compensated by norm_h -= 1.
 *
 * Both families preserve |amp|^2 * 2^-norm_h and the normalized amplitude amp * 2^(-norm_h/2).
 * Rewrites the instance in place to a canonical qsop-sign form (u<v edges, parity-deduped, no
 * self-loops, dense vars). Returns false only on allocation failure, leaving the instance valid.
 *
 * Eliminations are driven by an incremental lowest-index-first worklist that edits the adjacency
 * lists in place (only a single O(nvars+nedges) rebuild at the very end), so cost is
 * O(elimination-local work), not O(eliminations * (nvars+nedges)). */
typedef struct qsop_hadamard_simplify_stats {
  uint32_t degree0_eliminations;
  uint32_t degree1_eliminations;
  uint32_t degree2_eliminations;
  uint32_t omega_eliminations;
  bool zero_witness;
} qsop_hadamard_simplify_stats_t;

/* Statistics-bearing twin used by search-time kernelization.  Passing NULL for stats is
 * supported.  qsop_simplify_hadamard remains the source-compatible convenience wrapper. Both apply
 * [HH] and [omega]; they are sound only at whole-amplitude (mode-1) call sites -- qasm2sop import
 * and the solver root. */
bool qsop_simplify_hadamard_with_stats(qsop_instance_t *inst,
                                       qsop_hadamard_simplify_stats_t *stats);

bool qsop_simplify_hadamard(qsop_instance_t *inst);

/* [HH]-only reduction (no [omega]) for callers that run at an arbitrary odd target mode, where
 * [omega]'s i^k-dependent phase fold would be unsound -- currently the branch backend's
 * per-component materialized reduction. */
bool qsop_simplify_hadamard_hh_only_with_stats(qsop_instance_t *inst,
                                               qsop_hadamard_simplify_stats_t *stats);

/* Minimize the modulus to the coarsest power-of-two divisor of r (floored at 8) that still
 * represents every coefficient exactly (qasm2sop's emit-time angle reduction, lifted to a built
 * instance). Amplitude-preserving; rewrites r, constant and unary in place. */
bool qsop_reduce_modulus(qsop_instance_t *inst);

/* Whole-amplitude (mode-1) path-sum simplification umbrella: the [HH] + [omega] elimination
 * fixpoint followed by qsop_reduce_modulus. The single reusable entry point for the qasm2sop
 * importer and the solver root; both steps preserve the normalized amplitude exactly. Sound only at
 * mode-1 call sites (see qsop_simplify_hadamard_with_stats). */
bool qsop_simplify(qsop_instance_t *inst, qsop_hadamard_simplify_stats_t *stats);

#endif
