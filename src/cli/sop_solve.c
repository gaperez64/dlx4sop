#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum solve_backend {
  SOLVE_BACKEND_COMPONENTS,
  SOLVE_BACKEND_BRUTE_FORCE,
  SOLVE_BACKEND_BRANCH,
  SOLVE_BACKEND_RANKWIDTH,
  SOLVE_BACKEND_TREEWIDTH,
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
        "[--backend components|brute-force|branch|rankwidth|treewidth] "
        "[--branch-heuristic split|treewidth|cutrank-proxy] "
        "[--branch-rw-source native|from-treewidth|both|auto] "
        "[--rankwidth-decomposition PATH] [--rankwidth-generate left-deep|balanced|min-fill|min-fill-cut|from-treewidth|min-fill-search|best] "
        "[--rankwidth-dump PATH] "
        "[--rankwidth-table v1|v2] "
        "[--solve-mode count-table|fourier] [--rankwidth-mode count-table|fourier] "
        "[--treewidth-order min-fill|min-degree|min-fill-max-degree] "
        "[--include-result] [--include-probability] "
        "[--stats-jsonl PATH] [--branch-calibrate-backends] "
        "[--max-vars N] [--trace csv] [PATH|-]\n",
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
  case SOLVE_BACKEND_TREEWIDTH:
    return "treewidth";
  }
  return "unknown";
}

static const char *branch_heuristic_name(qsop_branch_heuristic_t heuristic) {
  switch (heuristic) {
  case QSOP_BRANCH_HEURISTIC_SPLIT:
    return "split";
  case QSOP_BRANCH_HEURISTIC_TREEWIDTH:
    return "treewidth";
  case QSOP_BRANCH_HEURISTIC_CUTRANK_PROXY:
    return "cutrank-proxy";
  }
  return "unknown";
}

static const char *rankwidth_generator_name(qsop_rankwidth_generator_t generator) {
  switch (generator) {
  case QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP:
    return "left-deep";
  case QSOP_RANKWIDTH_GENERATOR_BALANCED:
    return "balanced";
  case QSOP_RANKWIDTH_GENERATOR_MIN_FILL:
    return "min-fill";
  case QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT:
    return "min-fill-cut";
  case QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH:
    return "from-treewidth";
  case QSOP_RANKWIDTH_GENERATOR_BEST:
    return "best";
  case QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH:
    return "min-fill-search";
  }
  return "unknown";
}

static const char *rankwidth_mode_name(qsop_rankwidth_solve_mode_t mode) {
  switch (mode) {
  case QSOP_RANKWIDTH_SOLVE_COUNT_TABLE:
    return "count-table";
  case QSOP_RANKWIDTH_SOLVE_FOURIER:
    return "fourier";
  }
  return "unknown";
}

static const char *solve_mode_name(qsop_solve_mode_t mode) {
  switch (mode) {
  case QSOP_SOLVE_MODE_COUNT_TABLE:
    return "count-table";
  case QSOP_SOLVE_MODE_FOURIER:
    return "fourier";
  }
  return "unknown";
}

static const char *treewidth_order_name(qsop_treewidth_order_t order) {
  switch (order) {
  case QSOP_TREEWIDTH_ORDER_MIN_FILL:
    return "min-fill";
  case QSOP_TREEWIDTH_ORDER_MIN_DEGREE:
    return "min-degree";
  case QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE:
    return "min-fill-max-degree";
  }
  return "unknown";
}

static qsop_rankwidth_solve_mode_t rankwidth_mode_from_solve_mode(qsop_solve_mode_t mode) {
  return mode == QSOP_SOLVE_MODE_FOURIER ? QSOP_RANKWIDTH_SOLVE_FOURIER
                                         : QSOP_RANKWIDTH_SOLVE_COUNT_TABLE;
}

static const char *solve_mode_kernel_name(solve_backend_t backend, qsop_solve_mode_t mode) {
  if (mode == QSOP_SOLVE_MODE_COUNT_TABLE) {
    return "count-table";
  }
  return backend == SOLVE_BACKEND_COMPONENTS || backend == SOLVE_BACKEND_BRUTE_FORCE ||
                 backend == SOLVE_BACKEND_RANKWIDTH || backend == SOLVE_BACKEND_TREEWIDTH
             ? "fourier"
             : "hybrid-fourier";
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
                               qsop_solve_mode_t solve_mode, bool solve_mode_set,
                               qsop_rankwidth_solve_mode_t rankwidth_mode,
                               const char *rankwidth_decomposition,
                               qsop_treewidth_order_t treewidth_order, qsop_error_t *error) {
  if (file == NULL || stats == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "internal error: null solver-stats write argument");
    return false;
  }

  fprintf(file, "backend: %s\n", backend_name(backend));
  if (solve_mode_set || solve_mode != QSOP_SOLVE_MODE_COUNT_TABLE) {
    fprintf(file, "solve_mode: %s\n", solve_mode_name(solve_mode));
    fprintf(file, "solve_mode_kernel: %s\n", solve_mode_kernel_name(backend, solve_mode));
  }
  if (backend == SOLVE_BACKEND_COMPONENTS) {
    fprintf(file, "components: %" PRIu32 "\n", stats->components);
    fprintf(file, "cache_hits: %" PRIu64 "\n", stats->cache_hits);
    fprintf(file, "cache_misses: %" PRIu64 "\n", stats->cache_misses);
    fprintf(file, "cache_entries: %" PRIu64 "\n", stats->cache_entries);
    fprintf(file, "cache_stored_residue_slots: %" PRIu64 "\n",
            stats->cache_stored_residue_slots);
    if (stats->cache_estimated_bytes != 0) {
      fprintf(file, "cache_key_bytes: %" PRIu64 "\n", stats->cache_key_bytes);
      fprintf(file, "cache_count_bytes: %" PRIu64 "\n", stats->cache_count_bytes);
      fprintf(file, "cache_estimated_bytes: %" PRIu64 "\n", stats->cache_estimated_bytes);
    }
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
  } else if (backend == SOLVE_BACKEND_BRUTE_FORCE) {
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
  } else if (backend == SOLVE_BACKEND_BRANCH) {
    fprintf(file, "search_nodes: %" PRIu64 "\n", stats->search_nodes);
    fprintf(file, "cache_hits: %" PRIu64 "\n", stats->cache_hits);
    fprintf(file, "cache_misses: %" PRIu64 "\n", stats->cache_misses);
    if (stats->cache_avoided_nodes != 0) {
      fprintf(file, "cache_avoided_nodes: %" PRIu64 "\n", stats->cache_avoided_nodes);
    }
    if (stats->cache_canonical_hits != 0) {
      fprintf(file, "cache_canonical_hits: %" PRIu64 "\n", stats->cache_canonical_hits);
    }
    if (stats->cache_canonical_lookups != 0) {
      fprintf(file, "cache_canonical_lookups: %" PRIu64 "\n",
              stats->cache_canonical_lookups);
    }
    if (stats->cache_canonical_stores != 0) {
      fprintf(file, "cache_canonical_stores: %" PRIu64 "\n",
              stats->cache_canonical_stores);
    }
    fprintf(file, "cache_entries: %" PRIu64 "\n", stats->cache_entries);
    if (stats->cache_canonical_entries != 0) {
      fprintf(file, "cache_canonical_entries: %" PRIu64 "\n", stats->cache_canonical_entries);
    }
    fprintf(file, "cache_stored_residue_slots: %" PRIu64 "\n",
            stats->cache_stored_residue_slots);
    if (stats->cache_estimated_bytes != 0) {
      fprintf(file, "cache_key_bytes: %" PRIu64 "\n", stats->cache_key_bytes);
      fprintf(file, "cache_count_bytes: %" PRIu64 "\n", stats->cache_count_bytes);
      fprintf(file, "cache_estimated_bytes: %" PRIu64 "\n", stats->cache_estimated_bytes);
    }
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
    if (stats->treewidth_delegations != 0 || stats->rankwidth_delegations != 0 ||
        stats->branch_fallthroughs != 0 || stats->branch_treewidth_skips != 0 ||
        stats->branch_rankwidth_skips != 0) {
      fprintf(file, "treewidth_delegations: %" PRIu64 "\n", stats->treewidth_delegations);
      fprintf(file, "rankwidth_delegations: %" PRIu64 "\n", stats->rankwidth_delegations);
      fprintf(file, "branch_fallthroughs: %" PRIu64 "\n", stats->branch_fallthroughs);
      fprintf(file, "branch_treewidth_skips: %" PRIu64 "\n", stats->branch_treewidth_skips);
      fprintf(file, "branch_rankwidth_skips: %" PRIu64 "\n", stats->branch_rankwidth_skips);
      fprintf(file, "max_residual_vars: %" PRIu32 "\n", stats->max_residual_vars);
      fprintf(file, "max_residual_edges: %" PRIu32 "\n", stats->max_residual_edges);
      fprintf(file, "max_residual_components: %" PRIu32 "\n", stats->max_residual_components);
      fprintf(file, "max_residual_largest_component: %" PRIu32 "\n",
              stats->max_residual_largest_component);
      fprintf(file, "max_residual_min_fill_width: %" PRIu32 "\n",
              stats->max_residual_min_fill_width);
      fprintf(file, "max_residual_prefix_cut_rank: %" PRIu32 "\n",
              stats->max_residual_prefix_cut_rank);
      fprintf(file, "decomposition_width: %" PRIu32 "\n", stats->decomposition_width);
      if (stats->rankwidth_support_width != 0 || stats->rankwidth_labelled_width != 0) {
        fprintf(file, "rankwidth_support_width: %" PRIu32 "\n",
                stats->rankwidth_support_width);
        fprintf(file, "rankwidth_labelled_width: %" PRIu32 "\n",
                stats->rankwidth_labelled_width);
        fprintf(file, "rankwidth_table_forecast: %" PRIu64 "\n",
                stats->rankwidth_table_forecast);
        fprintf(file, "rankwidth_join_pair_forecast: %" PRIu64 "\n",
                stats->rankwidth_join_pair_forecast);
        fprintf(file, "rankwidth_labelled_exact_cuts: %" PRIu64 "\n",
                stats->rankwidth_labelled_exact_cuts);
        fprintf(file, "rankwidth_labelled_proxy_cuts: %" PRIu64 "\n",
                stats->rankwidth_labelled_proxy_cuts);
        fprintf(file, "rankwidth_labelled_exact_assignments: %" PRIu64 "\n",
                stats->rankwidth_labelled_exact_assignments);
      }
      fprintf(file, "table_entries: %" PRIu64 "\n", stats->table_entries);
      fprintf(file, "max_table_entries: %" PRIu64 "\n", stats->max_table_entries);
      fprintf(file, "join_pairs: %" PRIu64 "\n", stats->join_pairs);
    }
  } else if (backend == SOLVE_BACKEND_RANKWIDTH) {
    fprintf(file, "rankwidth_mode: %s\n", rankwidth_mode_name(rankwidth_mode));
    fprintf(file, "rankwidth_decomposition: %s\n", rankwidth_decomposition);
    fprintf(file, "decomposition_width: %" PRIu32 "\n", stats->decomposition_width);
    fprintf(file, "rankwidth_support_width: %" PRIu32 "\n",
            stats->rankwidth_support_width);
    fprintf(file, "rankwidth_labelled_width: %" PRIu32 "\n",
            stats->rankwidth_labelled_width);
    fprintf(file, "table_entries: %" PRIu64 "\n", stats->table_entries);
    fprintf(file, "max_table_entries: %" PRIu64 "\n", stats->max_table_entries);
    fprintf(file, "signature_entries: %" PRIu64 "\n", stats->signature_entries);
    fprintf(file, "max_signature_entries: %" PRIu64 "\n", stats->max_signature_entries);
    fprintf(file, "join_pairs: %" PRIu64 "\n", stats->join_pairs);
    fprintf(file, "join_signature_pairs: %" PRIu64 "\n", stats->join_signature_pairs);
    fprintf(file, "rankwidth_table_forecast: %" PRIu64 "\n",
            stats->rankwidth_table_forecast);
    fprintf(file, "rankwidth_join_pair_forecast: %" PRIu64 "\n",
            stats->rankwidth_join_pair_forecast);
    fprintf(file, "rankwidth_labelled_exact_cuts: %" PRIu64 "\n",
            stats->rankwidth_labelled_exact_cuts);
    fprintf(file, "rankwidth_labelled_proxy_cuts: %" PRIu64 "\n",
            stats->rankwidth_labelled_proxy_cuts);
    fprintf(file, "rankwidth_labelled_exact_assignments: %" PRIu64 "\n",
            stats->rankwidth_labelled_exact_assignments);
  } else {
    fprintf(file, "treewidth_order: %s\n", treewidth_order_name(treewidth_order));
    fprintf(file, "decomposition_width: %" PRIu32 "\n", stats->decomposition_width);
    fprintf(file, "table_entries: %" PRIu64 "\n", stats->table_entries);
    fprintf(file, "max_table_entries: %" PRIu64 "\n", stats->max_table_entries);
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

static bool write_result_stats(FILE *file, const qsop_result_t *result, qsop_error_t *error) {
  if (file == NULL || result == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "internal error: null result-stats write argument");
    return false;
  }

  fprintf(file, "result_modulus: %" PRIu32 "\n", result->r);
  fprintf(file, "result_norm_h: %" PRIu64 "\n", result->norm_h);
  fputs("result_counts:", file);
  for (uint32_t residue = 0; residue < result->r; residue++) {
    if (result->count_strings != NULL) {
      fprintf(file, " %s", result->count_strings[residue]);
    } else {
      fprintf(file, " %" PRIu64, result->counts[residue]);
    }
  }
  fputc('\n', file);

  if (ferror(file)) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

static bool result_count_long_double(const qsop_result_t *result, uint32_t residue,
                                     long double *out, qsop_error_t *error) {
  if (result->count_strings != NULL) {
    errno = 0;
    char *end = NULL;
    long double value = strtold(result->count_strings[residue], &end);
    if (end == result->count_strings[residue] || *end != '\0') {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message),
               "could not parse exact count for residue %" PRIu32, residue);
      return false;
    }
    *out = value;
    return true;
  }

  *out = (long double)result->counts[residue];
  return true;
}

static bool write_probability_stats(FILE *file, const qsop_result_t *result, qsop_error_t *error) {
  if (file == NULL || result == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "internal error: null probability-stats write argument");
    return false;
  }

  static const long double two_pi =
      6.283185307179586476925286766559005768394338798750211641949889L;
  long double real = 0.0L;
  long double imag = 0.0L;
  if (result->r % 2U == 0U) {
    const uint32_t half = result->r / 2U;
    for (uint32_t residue = 0; residue < half; residue++) {
      long double lower = 0.0L;
      long double upper = 0.0L;
      if (!result_count_long_double(result, residue, &lower, error) ||
          !result_count_long_double(result, residue + half, &upper, error)) {
        return false;
      }
      const long double count = lower - upper;
      const long double angle = two_pi * (long double)residue / (long double)result->r;
      real += count * cosl(angle);
      imag += count * sinl(angle);
    }
  } else {
    for (uint32_t residue = 0; residue < result->r; residue++) {
      long double count = 0.0L;
      if (!result_count_long_double(result, residue, &count, error)) {
        return false;
      }
      const long double angle = two_pi * (long double)residue / (long double)result->r;
      real += count * cosl(angle);
      imag += count * sinl(angle);
    }
  }

  const long double unnormalized = real * real + imag * imag;
  const long double probability = unnormalized * powl(2.0L, -(long double)result->norm_h);
  fprintf(file, "result_probability: %.17Lg\n", probability);

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
  const char *rankwidth_decomposition_label = "left-deep";
  const char *rankwidth_dump_path = NULL;
  uint32_t max_vars = 24;
  solve_backend_t backend = SOLVE_BACKEND_COMPONENTS;
  qsop_solve_mode_t solve_mode = QSOP_SOLVE_MODE_COUNT_TABLE;
  qsop_branch_heuristic_t branch_heuristic = QSOP_BRANCH_HEURISTIC_SPLIT;
  qsop_branch_rw_source_t branch_rw_source = QSOP_BRANCH_RW_SOURCE_NATIVE;
  qsop_rankwidth_generator_t rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP;
  qsop_rankwidth_solve_mode_t rankwidth_mode = QSOP_RANKWIDTH_SOLVE_COUNT_TABLE;
  qsop_treewidth_order_t treewidth_order = QSOP_TREEWIDTH_ORDER_MIN_FILL;
  bool branch_heuristic_set = false;
  bool branch_rw_source_set = false;
  bool rankwidth_generator_set = false;
  bool rankwidth_mode_set = false;
  bool rankwidth_table_v2 = false;
  bool solve_mode_set = false;
  bool treewidth_order_set = false;
  bool include_result = false;
  bool include_probability = false;
  const char *stats_jsonl_path = NULL;
  bool calibrate_backends = false;
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
    if (strcmp(argv[i], "--include-result") == 0) {
      include_result = true;
      continue;
    }
    if (strcmp(argv[i], "--include-probability") == 0) {
      include_probability = true;
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
    if (strcmp(argv[i], "--stats-jsonl") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --stats-jsonl requires a path\n", stderr);
        return 2;
      }
      stats_jsonl_path = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--branch-calibrate-backends") == 0) {
      calibrate_backends = true;
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
      } else if (strcmp(value, "cutrank-proxy") == 0) {
        branch_heuristic = QSOP_BRANCH_HEURISTIC_CUTRANK_PROXY;
      } else {
        fprintf(stderr, "error: unsupported branch heuristic '%s'\n", value);
        return 2;
      }
      branch_heuristic_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-rw-source") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-rw-source requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "native") == 0) {
        branch_rw_source = QSOP_BRANCH_RW_SOURCE_NATIVE;
      } else if (strcmp(value, "from-treewidth") == 0) {
        branch_rw_source = QSOP_BRANCH_RW_SOURCE_FROM_TREEWIDTH;
      } else if (strcmp(value, "both") == 0) {
        branch_rw_source = QSOP_BRANCH_RW_SOURCE_BOTH;
      } else if (strcmp(value, "auto") == 0) {
        branch_rw_source = QSOP_BRANCH_RW_SOURCE_AUTO;
      } else {
        fprintf(stderr, "error: unsupported --branch-rw-source '%s'\n", value);
        return 2;
      }
      branch_rw_source_set = true;
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
      } else if (strcmp(value, "treewidth") == 0) {
        backend = SOLVE_BACKEND_TREEWIDTH;
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
      rankwidth_decomposition_label = "explicit";
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-dump") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-dump requires a path\n", stderr);
        return 2;
      }
      rankwidth_dump_path = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-generate") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-generate requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "left-deep") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP;
      } else if (strcmp(value, "balanced") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_BALANCED;
      } else if (strcmp(value, "min-fill") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_MIN_FILL;
      } else if (strcmp(value, "min-fill-cut") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT;
      } else if (strcmp(value, "from-treewidth") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH;
      } else if (strcmp(value, "best") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_BEST;
      } else if (strcmp(value, "min-fill-search") == 0) {
        rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH;
      } else {
        fprintf(stderr, "error: unsupported rankwidth generator '%s'\n", value);
        return 2;
      }
      rankwidth_generator_set = true;
      rankwidth_decomposition_label = rankwidth_generator_name(rankwidth_generator);
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-mode") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-mode requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "count-table") == 0) {
        rankwidth_mode = QSOP_RANKWIDTH_SOLVE_COUNT_TABLE;
      } else if (strcmp(value, "fourier") == 0) {
        rankwidth_mode = QSOP_RANKWIDTH_SOLVE_FOURIER;
      } else {
        fprintf(stderr, "error: unsupported rankwidth mode '%s'\n", value);
        return 2;
      }
      rankwidth_mode_set = true;
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-table") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-table requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "v1") == 0) {
        rankwidth_table_v2 = false;
      } else if (strcmp(value, "v2") == 0) {
        rankwidth_table_v2 = true;
      } else {
        fprintf(stderr, "error: unsupported rankwidth table version '%s'\n", value);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--solve-mode") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --solve-mode requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "count-table") == 0) {
        solve_mode = QSOP_SOLVE_MODE_COUNT_TABLE;
      } else if (strcmp(value, "fourier") == 0) {
        solve_mode = QSOP_SOLVE_MODE_FOURIER;
      } else {
        fprintf(stderr, "error: unsupported solve mode '%s'\n", value);
        return 2;
      }
      solve_mode_set = true;
      continue;
    }
    if (strcmp(argv[i], "--treewidth-order") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --treewidth-order requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "min-fill") == 0) {
        treewidth_order = QSOP_TREEWIDTH_ORDER_MIN_FILL;
      } else if (strcmp(value, "min-degree") == 0) {
        treewidth_order = QSOP_TREEWIDTH_ORDER_MIN_DEGREE;
      } else if (strcmp(value, "min-fill-max-degree") == 0) {
        treewidth_order = QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE;
      } else {
        fprintf(stderr, "error: unsupported treewidth order '%s'\n", value);
        return 2;
      }
      treewidth_order_set = true;
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
  if (branch_rw_source_set && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --branch-rw-source requires --backend branch\n", stderr);
    return 2;
  }
  if (calibrate_backends && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --branch-calibrate-backends requires --backend branch\n", stderr);
    return 2;
  }
  if (calibrate_backends && stats_jsonl_path == NULL) {
    fputs("error: --branch-calibrate-backends requires --stats-jsonl\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rankwidth_decomposition_path != NULL) {
    fputs("error: --rankwidth-decomposition requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rankwidth_dump_path != NULL) {
    fputs("error: --rankwidth-dump requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rankwidth_generator_set) {
    fputs("error: --rankwidth-generate requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rankwidth_mode_set) {
    fputs("error: --rankwidth-mode requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rankwidth_table_v2) {
    fputs("error: --rankwidth-table requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (rankwidth_mode_set && solve_mode_set &&
      rankwidth_mode != rankwidth_mode_from_solve_mode(solve_mode)) {
    fputs("error: --solve-mode conflicts with --rankwidth-mode\n", stderr);
    return 2;
  }
  if (backend == SOLVE_BACKEND_RANKWIDTH && solve_mode_set) {
    rankwidth_mode = rankwidth_mode_from_solve_mode(solve_mode);
  }
  if (rankwidth_mode_set && !solve_mode_set) {
    solve_mode = rankwidth_mode == QSOP_RANKWIDTH_SOLVE_FOURIER ? QSOP_SOLVE_MODE_FOURIER
                                                                : QSOP_SOLVE_MODE_COUNT_TABLE;
    solve_mode_set = true;
  }
  if (backend != SOLVE_BACKEND_TREEWIDTH && treewidth_order_set) {
    fputs("error: --treewidth-order requires --backend treewidth\n", stderr);
    return 2;
  }
  if (rankwidth_decomposition_path != NULL && rankwidth_generator_set) {
    fputs("error: --rankwidth-generate cannot be combined with --rankwidth-decomposition\n",
          stderr);
    return 2;
  }
  if (include_result && format != SOLVE_FORMAT_STATS) {
    fputs("error: --include-result requires --format stats\n", stderr);
    return 2;
  }
  if (include_probability && format != SOLVE_FORMAT_STATS) {
    fputs("error: --include-probability requires --format stats\n", stderr);
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

  FILE *jsonl_file = NULL;
  if (stats_jsonl_path != NULL) {
    jsonl_file = fopen(stats_jsonl_path, "w");
    if (jsonl_file == NULL) {
      fprintf(stderr, "error: %s: %s\n", stats_jsonl_path, strerror(errno));
      if (input != stdin) {
        fclose(input);
      }
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
    if (jsonl_file != NULL) {
      fclose(jsonl_file);
    }
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
  qsop_backend_stats_sink_t sink = {
      .file = jsonl_file,
      .instance = diagnostic_path,
      .next_id = 0,
      .calibrate_backends = calibrate_backends,
  };
  qsop_backend_stats_sink_t *sink_ptr = jsonl_file != NULL ? &sink : NULL;
  if (backend == SOLVE_BACKEND_COMPONENTS) {
    ok = qsop_solve_components_bruteforce_mode_trace_stats(qsop, max_vars, solve_mode, &result,
                                                           &solve_stats, trace_ptr, &error);
  } else if (backend == SOLVE_BACKEND_BRUTE_FORCE) {
    ok = qsop_solve_bruteforce_mode_trace_stats(qsop, max_vars, solve_mode, &result,
                                                &solve_stats, trace_ptr, &error);
  } else if (backend == SOLVE_BACKEND_BRANCH) {
    if (branch_rw_source != QSOP_BRANCH_RW_SOURCE_NATIVE) {
      ok = qsop_solve_residual_branch_heuristic_rw_source_mode_trace_stats(
          qsop, max_vars, branch_heuristic, branch_rw_source, solve_mode, &result, &solve_stats,
          trace_ptr, &error);
    } else {
      ok = qsop_solve_residual_branch_heuristic_mode_sink_trace_stats(
          qsop, max_vars, branch_heuristic, solve_mode, sink_ptr, &result, &solve_stats, trace_ptr,
          &error);
    }
  } else if (backend == SOLVE_BACKEND_RANKWIDTH) {
    if (rankwidth_decomposition_path != NULL) {
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
    } else {
      ok = qsop_rankwidth_decomposition_generate(qsop, rankwidth_generator,
                                                 &rankwidth_decomposition, &error);
    }
    if (ok && rankwidth_dump_path != NULL) {
      FILE *dump_file = fopen(rankwidth_dump_path, "w");
      if (dump_file == NULL) {
        fprintf(stderr, "error: %s: %s\n", rankwidth_dump_path, strerror(errno));
        qsop_rankwidth_decomposition_free(rankwidth_decomposition);
        qsop_free(qsop);
        return 1;
      }
      ok = qsop_rankwidth_decomposition_write_file(dump_file, rankwidth_decomposition, &error);
      fclose(dump_file);
      if (!ok) {
        print_error(&error, rankwidth_dump_path);
        qsop_rankwidth_decomposition_free(rankwidth_decomposition);
        qsop_free(qsop);
        return 1;
      }
    }
    if (ok) {
      ok = rankwidth_table_v2
               ? qsop_solve_rankwidth_v2_mode_trace_stats(qsop, rankwidth_decomposition, max_vars,
                                                          rankwidth_mode, &result, &solve_stats,
                                                          trace_ptr, &error)
               : qsop_solve_rankwidth_mode_trace_stats(qsop, rankwidth_decomposition, max_vars,
                                                       rankwidth_mode, &result, &solve_stats,
                                                       trace_ptr, &error);
    }
  } else {
    ok = qsop_solve_treewidth_order_mode_trace_stats(qsop, max_vars, treewidth_order, solve_mode,
                                                     &result, &solve_stats, trace_ptr, &error);
  }
  qsop_rankwidth_decomposition_free(rankwidth_decomposition);
  qsop_free(qsop);
  if (jsonl_file != NULL) {
    fclose(jsonl_file);
  }
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  if (format == SOLVE_FORMAT_RESIDUE_VECTOR) {
    ok = qsop_result_write_residue_vector(stdout, result, &error);
  } else {
    ok = write_solver_stats(stdout, backend, &solve_stats, solve_mode, solve_mode_set,
                            rankwidth_mode,
                            rankwidth_decomposition_label, treewidth_order, &error);
    if (ok && backend == SOLVE_BACKEND_BRANCH && branch_heuristic_set) {
      printf("branch_heuristic: %s\n", branch_heuristic_name(branch_heuristic));
    }
    if (ok && include_result) {
      ok = write_result_stats(stdout, result, &error);
    }
    if (ok && include_probability) {
      ok = write_probability_stats(stdout, result, &error);
    }
  }
  qsop_result_free(result);
  if (!ok) {
    print_error(&error, "<stdout>");
    return 1;
  }

  return 0;
}
