#include "dlx4sop/qsop.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool write_failed(FILE *file, qsop_error_t *error) {
  if (!ferror(file)) {
    return false;
  }
  if (error != NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "write failed: %s", strerror(errno));
  }
  return true;
}

bool qsop_write_file(FILE *file, const qsop_instance_t *qsop, qsop_error_t *error) {
  if (file == NULL || qsop == NULL) {
    if (error != NULL) {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message), "internal error: null write argument");
    }
    return false;
  }

  fprintf(file, "p qsop-sign %" PRIu64 " %" PRIu32 " %" PRIu32 "\n", qsop->r, qsop->nvars,
          qsop->nedges);
  fprintf(file, "n %" PRIu64 "\n", qsop->norm_h);
  fprintf(file, "cst %" PRIu64 "\n", qsop->constant);

  bool wrote_unary = false;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (qsop->unary[v] != 0) {
      if (!wrote_unary) {
        fputc('\n', file);
        wrote_unary = true;
      }
      fprintf(file, "u %" PRIu32 " %" PRIu64 "\n", v, qsop->unary[v]);
    }
  }

  if (qsop->nedges > 0) {
    fputc('\n', file);
  }

  for (uint32_t e = 0; e < qsop->nedges; e++) {
    fprintf(file, "e %" PRIu32 " %" PRIu32 "\n", qsop->edge_u[e], qsop->edge_v[e]);
  }

  return !write_failed(file, error);
}
