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

/* Eliminate free variables whose unary coefficient is a multiple of r/2 -- that is, 0 or r/2 --
 * and whose incidence degree is at most 2 (Hadamard uncompute collapse). Summing such a variable
 * out is exact because omega^(r/2) = -1: it scales the amplitude by 2 and forces an XOR constraint
 * on the variable's neighbours, which at degree 0/1/2 is absorbed back into a qsop-sign instance
 * as, respectively, nothing, a pin, or a (possibly negated) merge. A degree-0 variable with unary
 * r/2 has factor 1 + omega^(r/2) = 0, so the whole amplitude vanishes and the instance collapses to
 * the canonical zero witness (nvars=1, nedges=0, constant=0, unary[0]=r/2).
 *
 * Preserves both |amp|^2 * 2^-norm_h and the normalized amplitude amp * 2^(-norm_h/2); the raw
 * amplitude is rescaled, compensated by norm_h -= 2 per elimination. Rewrites the instance in
 * place to a canonical qsop-sign form (u<v edges, parity-deduped, no self-loops, dense vars).
 * Returns false only on allocation failure, leaving the instance unchanged and valid.
 *
 * Cost is O(eliminations * (nvars + nedges)): one full rebuild per eliminated variable. That is
 * fine for circuit-derived instances (~2s on a 15k-variable qwalk import) but is worth replacing
 * with an incremental worklist if a corpus ever makes it the bottleneck. */
typedef struct qsop_hadamard_simplify_stats {
  uint32_t degree0_eliminations;
  uint32_t degree1_eliminations;
  uint32_t degree2_eliminations;
  bool zero_witness;
} qsop_hadamard_simplify_stats_t;

/* Statistics-bearing twin used by search-time kernelization.  Passing NULL for stats is
 * supported.  qsop_simplify_hadamard remains the source-compatible convenience wrapper. */
bool qsop_simplify_hadamard_with_stats(qsop_instance_t *inst,
                                       qsop_hadamard_simplify_stats_t *stats);

bool qsop_simplify_hadamard(qsop_instance_t *inst);

#endif
