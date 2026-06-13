#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residual.h"
#include "dlx4sop/residue.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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

static bool first_active_var(const qsop_residual_t *residual, uint32_t *out) {
  const uint32_t nvars = qsop_residual_nvars(residual);
  for (uint32_t v = 0; v < nvars; v++) {
    if (qsop_residual_var_active(residual, v)) {
      *out = v;
      return true;
    }
  }
  return false;
}

static bool branch_sum_rec(qsop_residual_t *residual, uint64_t *counts, qsop_error_t *error) {
  if (qsop_residual_active_vars(residual) == 0) {
    counts[qsop_residual_constant(residual)]++;
    return true;
  }

  uint32_t v = 0;
  if (!first_active_var(residual, &v)) {
    set_error(error, "residual active-var count disagrees with active flags");
    return false;
  }

  for (uint8_t value = 0; value <= 1U; value++) {
    const size_t checkpoint = qsop_residual_checkpoint(residual);
    if (!qsop_residual_branch(residual, v, value, error)) {
      return false;
    }
    if (!branch_sum_rec(residual, counts, error)) {
      return false;
    }
    if (!qsop_residual_undo(residual, checkpoint, error)) {
      return false;
    }
  }

  return true;
}

bool qsop_solve_residual_branch(const qsop_instance_t *qsop, uint32_t max_vars,
                                qsop_result_t **out, qsop_error_t *error) {
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
              "residual branch solver refuses %" PRIu32
              " variables; pass a larger --max-vars or use a future backend",
              qsop->nvars);
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  qsop_residual_t *residual = NULL;
  if (result == NULL) {
    set_error(error, "out of memory while allocating result");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;
  if (!qsop_counts_alloc(qsop->r, &result->counts, error) ||
      !qsop_residual_create(qsop, &residual, error)) {
    qsop_result_free(result);
    qsop_residual_free(residual);
    return false;
  }

  if (!branch_sum_rec(residual, result->counts, error)) {
    qsop_result_free(result);
    qsop_residual_free(residual);
    return false;
  }

  qsop_residual_free(residual);
  *out = result;
  return true;
}
