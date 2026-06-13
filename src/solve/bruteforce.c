#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "trace.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void set_error(qsop_error_t *error, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }

  error->path = NULL;
  error->line = 0;
  error->column = 0;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
}

void qsop_result_free(qsop_result_t *result) {
  if (result == NULL) {
    return;
  }
  if (result->count_strings != NULL) {
    for (uint32_t residue = 0; residue < result->r; residue++) {
      free(result->count_strings[residue]);
    }
  }
  free(result->count_strings);
  free(result->counts);
  free(result);
}

static bool bit_is_set(uint64_t assignment, uint32_t bit) {
  return ((assignment >> bit) & UINT64_C(1)) != 0;
}

bool qsop_solve_bruteforce(const qsop_instance_t *qsop, uint32_t max_vars, qsop_result_t **out,
                           qsop_error_t *error) {
  return qsop_solve_bruteforce_stats(qsop, max_vars, out, NULL, error);
}

bool qsop_solve_bruteforce_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                 qsop_result_t **out, qsop_solve_stats_t *stats,
                                 qsop_error_t *error) {
  return qsop_solve_bruteforce_trace_stats(qsop, max_vars, out, stats, NULL, error);
}

bool qsop_solve_bruteforce_trace_stats(const qsop_instance_t *qsop, uint32_t max_vars,
                                       qsop_result_t **out, qsop_solve_stats_t *stats,
                                       qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;

  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }
  if (qsop->nvars > max_vars) {
    set_error(error,
              "brute-force solver refuses %" PRIu32
              " variables; pass a larger --max-vars or use a future backend",
              qsop->nvars);
    return false;
  }
  if (qsop->nvars >= 63U) {
    set_error(error, "brute-force solver supports at most 62 variables with uint64 counters");
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    set_error(error, "out of memory while allocating result");
    return false;
  }

  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error)) {
    qsop_result_free(result);
    return false;
  }

  const uint64_t assignments = UINT64_C(1) << qsop->nvars;
  if (stats != NULL) {
    stats->leaf_assignments = assignments;
  }
  const uint64_t enumerate_start = qsop_trace_begin(trace);
  for (uint64_t assignment = 0; assignment < assignments; assignment++) {
    uint32_t phase = qsop->constant;
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if (bit_is_set(assignment, v)) {
        phase = (uint32_t)(((uint64_t)phase + qsop->unary[v]) % qsop->r);
      }
    }
    for (uint32_t e = 0; e < qsop->nedges; e++) {
      if (bit_is_set(assignment, qsop->edge_u[e]) && bit_is_set(assignment, qsop->edge_v[e])) {
        phase = (uint32_t)(((uint64_t)phase + qsop->edge_q[e]) % qsop->r);
      }
    }
    if (!qsop_count_add(&result->counts[phase], 1, error)) {
      qsop_result_free(result);
      return false;
    }
  }
  qsop_trace_emit_elapsed(trace, "brute_force.enumerate", 0, assignments, enumerate_start);

  *out = result;
  return true;
}

bool qsop_result_write_residue_vector(FILE *file, const qsop_result_t *result,
                                      qsop_error_t *error) {
  if (file == NULL || result == NULL) {
    set_error(error, "internal error: null residue-vector write argument");
    return false;
  }

  fprintf(file, "p qsop-result %" PRIu32 "\n", result->r);
  fprintf(file, "n %" PRIu64 "\n", result->norm_h);
  fputs("counts", file);
  for (uint32_t residue = 0; residue < result->r; residue++) {
    if (result->count_strings != NULL) {
      fprintf(file, " %s", result->count_strings[residue]);
    } else {
      fprintf(file, " %" PRIu64, result->counts[residue]);
    }
  }
  fputc('\n', file);

  if (ferror(file)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}
