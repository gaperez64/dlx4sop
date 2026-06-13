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

typedef struct qsop_solve_stats {
  uint64_t search_nodes;
  uint64_t leaf_assignments;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint32_t components;
} qsop_solve_stats_t;

typedef struct qsop_solve_trace_event {
  const char *phase;
  uint32_t depth;
  uint64_t items;
  uint64_t elapsed_ns;
} qsop_solve_trace_event_t;

typedef void (*qsop_solve_trace_fn)(void *user, const qsop_solve_trace_event_t *event);

typedef struct qsop_solve_trace {
  qsop_solve_trace_fn emit;
  void *user;
} qsop_solve_trace_t;

void qsop_result_free(qsop_result_t *result);

bool qsop_solve_bruteforce(const qsop_instance_t *qsop, uint32_t max_vars, qsop_result_t **out,
                           qsop_error_t *error);

bool qsop_solve_bruteforce_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                 qsop_result_t **out, qsop_solve_stats_t *stats,
                                 qsop_error_t *error);

bool qsop_solve_bruteforce_trace_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                       qsop_result_t **out, qsop_solve_stats_t *stats,
                                       qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_components_bruteforce(const qsop_instance_t *qsop, uint32_t max_component_vars,
                                      qsop_result_t **out, qsop_error_t *error);

bool qsop_solve_components_bruteforce_stats(const qsop_instance_t *qsop,
                                            uint32_t max_component_vars, qsop_result_t **out,
                                            qsop_solve_stats_t *stats, qsop_error_t *error);

bool qsop_solve_components_bruteforce_trace_stats(const qsop_instance_t *qsop,
                                                  uint32_t max_component_vars, qsop_result_t **out,
                                                  qsop_solve_stats_t *stats,
                                                  qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_solve_residual_branch(const qsop_instance_t *qsop, uint32_t max_vars, qsop_result_t **out,
                                qsop_error_t *error);

bool qsop_solve_residual_branch_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                      qsop_result_t **out, qsop_solve_stats_t *stats,
                                      qsop_error_t *error);

bool qsop_solve_residual_branch_trace_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                            qsop_result_t **out, qsop_solve_stats_t *stats,
                                            qsop_solve_trace_t *trace, qsop_error_t *error);

bool qsop_result_write_residue_vector(FILE *file, const qsop_result_t *result, qsop_error_t *error);

#endif
