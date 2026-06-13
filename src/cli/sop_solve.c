#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum solve_backend {
  SOLVE_BACKEND_COMPONENTS,
  SOLVE_BACKEND_BRUTE_FORCE,
  SOLVE_BACKEND_BRANCH,
  SOLVE_BACKEND_RANKWIDTH,
} solve_backend_t;

typedef enum solve_output_format {
  SOLVE_FORMAT_RESIDUE_VECTOR,
  SOLVE_FORMAT_STATS,
} solve_output_format_t;

typedef enum solve_trace_format {
  SOLVE_TRACE_NONE,
  SOLVE_TRACE_CSV,
} solve_trace_format_t;

typedef struct csv_trace_writer {
  FILE *file;
  bool wrote_header;
} csv_trace_writer_t;

static void print_usage(FILE *file) {
  fputs("usage: sop-solve [--format residue-vector|stats] "
        "[--backend components|brute-force|branch|rankwidth] "
        "[--branch-heuristic split|treewidth|linear-rankwidth] "
        "[--rankwidth-decomposition PATH] [--max-vars N] [--trace csv] [PATH|-]\n",
        file);
}

static void print_error(const qsop_error_t *error, const char *fallback_path) {
  const char *path = error->path != NULL ? error->path : fallback_path;
  if (path == NULL) {
    path = "<input>";
  }
  if (error->line > 0) {
    fprintf(stderr, "error: %s:%zu:%zu: %s\n", path, error->line, error->column, error->message);
  } else {
    fprintf(stderr, "error: %s: %s\n", path, error->message);
  }
}

static const char *backend_name(solve_backend_t backend) {
  switch (backend) {
  case SOLVE_BACKEND_COMPONENTS:
    return "components";
  case SOLVE_BACKEND_BRUTE_FORCE:
    return "brute-force";
  case SOLVE_BACKEND_BRANCH:
    return "branch";
  case SOLVE_BACKEND_RANKWIDTH:
    return "rankwidth";
  }
  return "unknown";
}

static const char *branch_heuristic_name(qsop_branch_heuristic_t heuristic) {
  switch (heuristic) {
  case QSOP_BRANCH_HEURISTIC_SPLIT:
    return "split";
  case QSOP_BRANCH_HEURISTIC_TREEWIDTH:
    return "treewidth";
  case QSOP_BRANCH_HEURISTIC_LINEAR_RANKWIDTH:
    return "linear-rankwidth";
  }
  return "unknown";
}

static void write_csv_trace_event(void *user, const qsop_solve_trace_event_t *event) {
  csv_trace_writer_t *writer = user;
  if (writer == NULL || writer->file == NULL || event == NULL) {
    return;
  }
  if (!writer->wrote_header) {
    fputs("phase,depth,items,elapsed_ns\n", writer->file);
    writer->wrote_header = true;
  }
  fprintf(writer->file, "%s,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n", event->phase, event->depth,
          event->items, event->elapsed_ns);
}

static bool write_solver_stats(FILE *file, solve_backend_t backend, const qsop_solve_stats_t *stats,
                               qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "internal error: null solver-stats write argument");
    return false;
  }

  fprintf(file, "backend: %s\n", backend_name(backend));
  if (backend == SOLVE_BACKEND_COMPONENTS) {
    fprintf(file, "components: %" PRIu32 "\n", stats->components);
    fprintf(file, "cache_hits: %" PRIu64 "\n", stats->cache_hits);
    fprintf(file, "cache_misses: %" PRIu64 "\n", stats->cache_misses);
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
  } else if (backend == SOLVE_BACKEND_BRUTE_FORCE) {
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
  } else if (backend == SOLVE_BACKEND_BRANCH) {
    fprintf(file, "search_nodes: %" PRIu64 "\n", stats->search_nodes);
    fprintf(file, "cache_hits: %" PRIu64 "\n", stats->cache_hits);
    fprintf(file, "cache_misses: %" PRIu64 "\n", stats->cache_misses);
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
  } else {
    fprintf(file, "decomposition_width: %" PRIu32 "\n", stats->decomposition_width);
    fprintf(file, "table_entries: %" PRIu64 "\n", stats->table_entries);
    fprintf(file, "join_pairs: %" PRIu64 "\n", stats->join_pairs);
  }

  if (ferror(file)) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

static bool parse_max_vars(const char *text, uint32_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
    return false;
  }

  *out = (uint32_t)value;
  return true;
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *rankwidth_decomposition_path = NULL;
  uint32_t max_vars = 24;
  solve_backend_t backend = SOLVE_BACKEND_COMPONENTS;
  qsop_branch_heuristic_t branch_heuristic = QSOP_BRANCH_HEURISTIC_SPLIT;
  bool branch_heuristic_set = false;
  solve_output_format_t format = SOLVE_FORMAT_RESIDUE_VECTOR;
  solve_trace_format_t trace_format = SOLVE_TRACE_NONE;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--format") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --format requires a value\n", stderr);
        return 2;
      }
      const char *format_value = argv[++i];
      if (strcmp(format_value, "residue-vector") != 0) {
        if (strcmp(format_value, "stats") == 0) {
          format = SOLVE_FORMAT_STATS;
        } else {
          fprintf(stderr, "error: unsupported format '%s'\n", format_value);
          return 2;
        }
      }
      continue;
    }
    if (strcmp(argv[i], "--max-vars") == 0) {
      if (i + 1 >= argc || !parse_max_vars(argv[i + 1], &max_vars)) {
        fputs("error: --max-vars requires a non-negative uint32 value\n", stderr);
        return 2;
      }
      i++;
      continue;
    }
    if (strcmp(argv[i], "--trace") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --trace requires a value\n", stderr);
        return 2;
      }
      const char *trace_value = argv[++i];
      if (strcmp(trace_value, "csv") == 0) {
        trace_format = SOLVE_TRACE_CSV;
      } else {
        fprintf(stderr, "error: unsupported trace format '%s'\n", trace_value);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--branch-heuristic") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-heuristic requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "split") == 0) {
        branch_heuristic = QSOP_BRANCH_HEURISTIC_SPLIT;
      } else if (strcmp(value, "treewidth") == 0) {
        branch_heuristic = QSOP_BRANCH_HEURISTIC_TREEWIDTH;
      } else if (strcmp(value, "linear-rankwidth") == 0) {
        branch_heuristic = QSOP_BRANCH_HEURISTIC_LINEAR_RANKWIDTH;
      } else {
        fprintf(stderr, "error: unsupported branch heuristic '%s'\n", value);
        return 2;
      }
      branch_heuristic_set = true;
      continue;
    }
    if (strcmp(argv[i], "--backend") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --backend requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "components") == 0) {
        backend = SOLVE_BACKEND_COMPONENTS;
      } else if (strcmp(value, "brute-force") == 0) {
        backend = SOLVE_BACKEND_BRUTE_FORCE;
      } else if (strcmp(value, "branch") == 0) {
        backend = SOLVE_BACKEND_BRANCH;
      } else if (strcmp(value, "rankwidth") == 0) {
        backend = SOLVE_BACKEND_RANKWIDTH;
      } else {
        fprintf(stderr, "error: unsupported backend '%s'\n", value);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-decomposition") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-decomposition requires a path\n", stderr);
        return 2;
      }
      rankwidth_decomposition_path = argv[++i];
      continue;
    }
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-") == 0 && input_path == NULL) {
        input_path = argv[i];
        continue;
      }
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
    if (input_path != NULL) {
      fputs("error: expected at most one input path\n", stderr);
      print_usage(stderr);
      return 2;
    }
    input_path = argv[i];
  }
  if (branch_heuristic_set && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --branch-heuristic requires --backend branch\n", stderr);
    return 2;
  }
  if (backend == SOLVE_BACKEND_RANKWIDTH && rankwidth_decomposition_path == NULL) {
    fputs("error: --backend rankwidth requires --rankwidth-decomposition\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rankwidth_decomposition_path != NULL) {
    fputs("error: --rankwidth-decomposition requires --backend rankwidth\n", stderr);
    return 2;
  }

  FILE *input = stdin;
  const char *diagnostic_path = "<stdin>";
  if (input_path != NULL && strcmp(input_path, "-") != 0) {
    input = fopen(input_path, "r");
    diagnostic_path = input_path;
    if (input == NULL) {
      fprintf(stderr, "error: %s: %s\n", input_path, strerror(errno));
      return 1;
    }
  }

  qsop_error_t error = {0};
  qsop_instance_t *qsop = NULL;
  bool ok = qsop_parse_file(input, diagnostic_path, &qsop, &error);
  if (input != stdin) {
    fclose(input);
  }
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  qsop_result_t *result = NULL;
  qsop_solve_stats_t solve_stats = {0};
  qsop_rankwidth_decomposition_t *rankwidth_decomposition = NULL;
  csv_trace_writer_t csv_trace = {
      .file = stderr,
  };
  qsop_solve_trace_t trace = {
      .emit = trace_format == SOLVE_TRACE_CSV ? write_csv_trace_event : NULL,
      .user = &csv_trace,
  };
  qsop_solve_trace_t *trace_ptr = trace_format == SOLVE_TRACE_NONE ? NULL : &trace;
  if (backend == SOLVE_BACKEND_COMPONENTS) {
    ok = qsop_solve_components_bruteforce_trace_stats(qsop, max_vars, &result, &solve_stats,
                                                      trace_ptr, &error);
  } else if (backend == SOLVE_BACKEND_BRUTE_FORCE) {
    ok =
        qsop_solve_bruteforce_trace_stats(qsop, max_vars, &result, &solve_stats, trace_ptr, &error);
  } else if (backend == SOLVE_BACKEND_BRANCH) {
    ok = qsop_solve_residual_branch_heuristic_trace_stats(
        qsop, max_vars, branch_heuristic, &result, &solve_stats, trace_ptr, &error);
  } else {
    FILE *decomposition_file = fopen(rankwidth_decomposition_path, "r");
    if (decomposition_file == NULL) {
      fprintf(stderr, "error: %s: %s\n", rankwidth_decomposition_path, strerror(errno));
      qsop_free(qsop);
      return 1;
    }
    ok = qsop_rankwidth_decomposition_parse_file(decomposition_file, rankwidth_decomposition_path,
                                                 qsop->nvars, &rankwidth_decomposition, &error);
    fclose(decomposition_file);
    if (!ok) {
      print_error(&error, rankwidth_decomposition_path);
      qsop_free(qsop);
      return 1;
    }
    ok = qsop_solve_rankwidth_trace_stats(qsop, rankwidth_decomposition, max_vars, &result,
                                          &solve_stats, trace_ptr, &error);
  }
  qsop_rankwidth_decomposition_free(rankwidth_decomposition);
  qsop_free(qsop);
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  if (format == SOLVE_FORMAT_RESIDUE_VECTOR) {
    ok = qsop_result_write_residue_vector(stdout, result, &error);
  } else {
    ok = write_solver_stats(stdout, backend, &solve_stats, &error);
    if (ok && backend == SOLVE_BACKEND_BRANCH && branch_heuristic_set) {
      printf("branch_heuristic: %s\n", branch_heuristic_name(branch_heuristic));
    }
  }
  qsop_result_free(result);
  if (!ok) {
    print_error(&error, "<stdout>");
    return 1;
  }

  return 0;
}
