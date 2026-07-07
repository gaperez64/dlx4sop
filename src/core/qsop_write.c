#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

bool qsop_result_write_residue_vector(FILE *file, const qsop_result_t *result,
                                      qsop_error_t *error) {
  if (file == NULL || result == NULL) {
    if (error != NULL) {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message),
               "internal error: null residue-vector write argument");
    }
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

  return !write_failed(file, error);
}
