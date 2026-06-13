#ifndef DLX4SOP_QSOP_SOLVE_H
#define DLX4SOP_QSOP_SOLVE_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct qsop_result {
  uint32_t r;
  uint64_t norm_h;
  uint64_t *counts;
} qsop_result_t;

void qsop_result_free(qsop_result_t *result);

bool qsop_solve_bruteforce(const qsop_instance_t *qsop, uint32_t max_vars,
                           qsop_result_t **out, qsop_error_t *error);

bool qsop_result_write_residue_vector(FILE *file, const qsop_result_t *result,
                                      qsop_error_t *error);

#endif
