#ifndef DLX4SOP_RESIDUE_H
#define DLX4SOP_RESIDUE_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>

bool qsop_counts_alloc(uint32_t r, uint64_t **out, qsop_error_t *error);

void qsop_counts_clear(uint32_t r, uint64_t *counts);

void qsop_counts_shift_add(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift);

bool qsop_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                          const uint64_t *right, qsop_error_t *error);

#endif
