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

/* Eliminate free variables with unary==0 and incidence degree 1..2 (Hadamard uncompute
 * collapse). Amplitude-exact: preserves |amp|^2 * 2^-norm_h. Rewrites the instance in place
 * to a canonical qsop-sign form (u<v edges, parity-deduped, no self-loops, dense vars).
 * Returns false only on allocation failure, leaving the instance unchanged and valid. */
bool qsop_simplify_hadamard(qsop_instance_t *inst);

#endif
