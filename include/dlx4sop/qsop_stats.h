#ifndef DLX4SOP_QSOP_STATS_H
#define DLX4SOP_QSOP_STATS_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct qsop_stats {
  uint32_t r;
  uint32_t nvars;
  uint32_t nedges;
  uint32_t nonzero_unary;
  uint64_t norm_h;
  qsop_mode_t mode;
  uint32_t components;
  uint32_t max_degree;
} qsop_stats_t;

bool qsop_compute_stats(const qsop_instance_t *qsop, qsop_stats_t *stats, qsop_error_t *error);

bool qsop_stats_write_text(FILE *file, const qsop_stats_t *stats, qsop_error_t *error);

bool qsop_stats_write_json(FILE *file, const qsop_stats_t *stats, qsop_error_t *error);

#endif
