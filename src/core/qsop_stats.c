#include "dlx4sop/qsop_stats.h"

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

static bool write_failed(FILE *file, qsop_error_t *error) {
  if (!ferror(file)) {
    return false;
  }
  set_error(error, "write failed: %s", strerror(errno));
  return true;
}

bool qsop_compute_stats(const qsop_instance_t *qsop, qsop_stats_t *stats, qsop_error_t *error) {
  if (qsop == NULL || stats == NULL) {
    set_error(error, "internal error: null stats argument");
    return false;
  }

  *stats = (qsop_stats_t){
      .r = qsop->r,
      .nvars = qsop->nvars,
      .nedges = qsop->nedges,
      .norm_h = qsop->norm_h,
      .mode = qsop->mode,
  };

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (qsop->unary[v] != 0) {
      stats->nonzero_unary++;
    }
  }

  uint32_t *degree = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*degree));
  uint64_t *rowptr = calloc((size_t)qsop->nvars + 1U, sizeof(*rowptr));
  uint32_t *cursor = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*cursor));
  uint32_t *colind = calloc(qsop->nedges == 0 ? 1U : (size_t)2U * qsop->nedges, sizeof(*colind));
  bool *visited = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*visited));
  uint32_t *queue = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*queue));
  if (degree == NULL || rowptr == NULL || cursor == NULL || colind == NULL || visited == NULL ||
      queue == NULL) {
    free(degree);
    free(rowptr);
    free(cursor);
    free(colind);
    free(visited);
    free(queue);
    set_error(error, "out of memory while computing stats");
    return false;
  }

  for (uint32_t e = 0; e < qsop->nedges; e++) {
    degree[qsop->edge_u[e]]++;
    degree[qsop->edge_v[e]]++;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (degree[v] > stats->max_degree) {
      stats->max_degree = degree[v];
    }
    rowptr[v + 1U] = rowptr[v] + degree[v];
    cursor[v] = (uint32_t)rowptr[v];
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint32_t u = qsop->edge_u[e];
    const uint32_t v = qsop->edge_v[e];
    colind[cursor[u]++] = v;
    colind[cursor[v]++] = u;
  }

  for (uint32_t start = 0; start < qsop->nvars; start++) {
    if (visited[start]) {
      continue;
    }

    stats->components++;
    uint32_t head = 0;
    uint32_t tail = 0;
    visited[start] = true;
    queue[tail++] = start;
    while (head < tail) {
      const uint32_t v = queue[head++];
      for (uint64_t p = rowptr[v]; p < rowptr[v + 1U]; p++) {
        const uint32_t other = colind[p];
        if (!visited[other]) {
          visited[other] = true;
          queue[tail++] = other;
        }
      }
    }
  }

  free(degree);
  free(rowptr);
  free(cursor);
  free(colind);
  free(visited);
  free(queue);
  return true;
}

bool qsop_stats_write_text(FILE *file, const qsop_stats_t *stats, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    set_error(error, "internal error: null stats text write argument");
    return false;
  }

  fprintf(file, "modulus: %" PRIu32 "\n", stats->r);
  fprintf(file, "variables: %" PRIu32 "\n", stats->nvars);
  fprintf(file, "quadratic_terms: %" PRIu32 "\n", stats->nedges);
  fprintf(file, "nonzero_unary: %" PRIu32 "\n", stats->nonzero_unary);
  fprintf(file, "normalization_h: %" PRIu64 "\n", stats->norm_h);
  fprintf(file, "mode: %s\n", qsop_mode_name(stats->mode));
  fprintf(file, "components: %" PRIu32 "\n", stats->components);
  fprintf(file, "max_degree: %" PRIu32 "\n", stats->max_degree);
  return !write_failed(file, error);
}

bool qsop_stats_write_json(FILE *file, const qsop_stats_t *stats, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    set_error(error, "internal error: null stats JSON write argument");
    return false;
  }

  fprintf(file,
          "{\"modulus\":%" PRIu32 ",\"variables\":%" PRIu32
          ",\"quadratic_terms\":%" PRIu32 ",\"nonzero_unary\":%" PRIu32
          ",\"normalization_h\":%" PRIu64 ",\"mode\":\"%s\",\"components\":%" PRIu32
          ",\"max_degree\":%" PRIu32 "}\n",
          stats->r, stats->nvars, stats->nedges, stats->nonzero_unary, stats->norm_h,
          qsop_mode_name(stats->mode), stats->components, stats->max_degree);
  return !write_failed(file, error);
}
