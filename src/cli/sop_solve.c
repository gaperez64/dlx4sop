#include "cli_common.h"
#include "dlx4sop/min_fill.h"
#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/simd.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum solve_backend {
  SOLVE_BACKEND_BRANCH,
  SOLVE_BACKEND_RANKWIDTH,
  SOLVE_BACKEND_TREEWIDTH,
} solve_backend_t;

typedef enum solve_output_format {
  SOLVE_FORMAT_AMPLITUDE,
  SOLVE_FORMAT_RESIDUE_VECTOR,
  SOLVE_FORMAT_STATS,
} solve_output_format_t;

typedef enum solve_trace_format {
  SOLVE_TRACE_NONE,
  SOLVE_TRACE_CSV,
} solve_trace_format_t;

typedef enum single_mode_precision_cli {
  SINGLE_MODE_PRECISION_AUTO = 0,
  SINGLE_MODE_PRECISION_DOUBLE,
  SINGLE_MODE_PRECISION_LONG_DOUBLE,
} single_mode_precision_cli_t;

typedef enum rankwidth_memory_policy {
  RW_MEMORY_POLICY_SKIP,
  RW_MEMORY_POLICY_FALLBACK,
  RW_MEMORY_POLICY_HARD_ERROR,
} rankwidth_memory_policy_t;

typedef struct csv_trace_writer {
  FILE *file;
  bool wrote_header;
} csv_trace_writer_t;

static void print_usage_mode(FILE *file, bool advanced) {
  static const char *const core[] = {
      "--format amplitude|residue-vector|stats",
      "--backend branch|treewidth|rankwidth",
      "--solve-mode auto|count-table|fourier|single-fourier",
      "--max-vars N",
      "--fourier-target-mode N",
      "--include-result",
      "--include-probability",
      "--version",
      "--help",
      "--help-advanced",
      "PATH|-",
  };
  static const char *const backend[] = {
      "--branch-heuristic delegation-depth|split|treewidth|cutrank-proxy",
      "--branch-rw-source none|native|from-treewidth|both|auto",
      "--rankwidth-decomposition PATH",
      "--rankwidth-generate "
      "left-deep|balanced|min-fill|min-fill-cut|from-treewidth|min-fill-search|best",
      "--rankwidth-dump PATH",
      "--rankwidth-mode count-table|fourier",
      "--treewidth-order min-fill|min-degree|min-fill-max-degree",
  };
  static const char *const kernels[] = {
      "--single-mode-precision auto|double|long-double",
      "--branch-single-fourier-fallback auto|delegate-only|always|never|off",
      "--branch-single-propagate auto|off",
      "--branch-single-materialized-reduction",
      "--branch-single-diagnose-conditioning",
      "--branch-single-kernel auto|scalar",
      "--rankwidth-single-kernel auto|streaming|materialized|dense",
      "--rankwidth-fourier-kernel auto|streaming|hybrid-even-fwht|dense-reference",
      "--print-kernels",
  };
  static const char *const tuning[] = {
      "--branch-rw-min-speedup FLOAT",
      "--branch-rw-fixed-overhead-ns N",
      "--branch-tw-fixed-overhead-ns N",
      "--branch-rw-memory-penalty-ns N",
      "--branch-single-max-fallback-vars N",
      "--branch-single-delegate-max-dp-work N",
      "--branch-single-max-search-nodes N",
      "--branch-single-cache-budget-mib N",
      "--branch-single-cache-min-vars N",
      "--branch-single-cutset-depth N",
      "--branch-single-lookahead-candidates N",
      "--branch-single-max-conditioning-nodes N",
      "--branch-single-delegate-reprobe-interval N",
      "--branch-single-max-stagnant-levels N",
      "--rankwidth-memory-budget-mib N",
      "--rankwidth-memory-budget-bytes N",
      "--rankwidth-memory-policy skip|fallback|hard-error",
      "--rankwidth-join-strategy auto|materialized|streaming",
      "--rankwidth-materialize-join-max-pairs N",
      "--stats-jsonl PATH",
      "--branch-calibrate-backends",
      "--trace csv",
  };
  const dlx4sop_cli_usage_section_t short_sections[] = {
      {.title = "Core", .items = core, .nitems = sizeof(core) / sizeof(core[0])},
      {.title = "Backends", .items = backend, .nitems = 3U},
  };
  const dlx4sop_cli_usage_section_t advanced_sections[] = {
      {.title = "Core", .items = core, .nitems = sizeof(core) / sizeof(core[0])},
      {.title = "Backends", .items = backend, .nitems = sizeof(backend) / sizeof(backend[0])},
      {.title = "Kernels", .items = kernels, .nitems = sizeof(kernels) / sizeof(kernels[0])},
      {.title = "Tuning", .items = tuning, .nitems = sizeof(tuning) / sizeof(tuning[0])},
  };
  if (advanced) {
    dlx4sop_cli_print_usage(file,
                            "usage: sop-solve [--format amplitude|residue-vector|stats] "
                            "[--backend branch|treewidth|rankwidth] [--solve-mode MODE] [PATH|-]",
                            advanced_sections,
                            sizeof(advanced_sections) / sizeof(advanced_sections[0]));
  } else {
    dlx4sop_cli_print_usage(file,
                            "usage: sop-solve [--format amplitude|residue-vector|stats] "
                            "[--backend branch|treewidth|rankwidth] [--solve-mode MODE] [PATH|-]",
                            short_sections, sizeof(short_sections) / sizeof(short_sections[0]));
  }
}

static void print_usage(FILE *file) {
  print_usage_mode(file, false);
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
  case QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH:
    return "delegation-depth";
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

static const char *rankwidth_fourier_kernel_name(qsop_rankwidth_fourier_kernel_t kernel) {
  switch (kernel) {
  case QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO:
    return "auto";
  case QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING:
    return "streaming";
  case QSOP_RANKWIDTH_FOURIER_KERNEL_HYBRID_EVEN_FWHT:
    return "hybrid-even-fwht";
  case QSOP_RANKWIDTH_FOURIER_KERNEL_DENSE_REFERENCE:
    return "dense-reference";
  }
  return "unknown";
}

static const char *simd_kernel_display_name(qsop_simd_kernel_t kernel) {
  switch (kernel) {
  case QSOP_SIMD_KERNEL_SCALAR:
    return "scalar";
  case QSOP_SIMD_KERNEL_AUTO:
    return "auto";
  case QSOP_SIMD_KERNEL_NEON:
    return "neon";
  case QSOP_SIMD_KERNEL_AVX512:
    return "avx512";
  case QSOP_SIMD_KERNEL_AVX2:
    return "avx2";
  }
  return "unknown";
}

static const char *single_mode_precision_name(single_mode_precision_cli_t precision) {
  switch (precision) {
  case SINGLE_MODE_PRECISION_AUTO:
    return "auto";
  case SINGLE_MODE_PRECISION_DOUBLE:
    return "double";
  case SINGLE_MODE_PRECISION_LONG_DOUBLE:
    return "long-double";
  }
  return "unknown";
}

static qsop_simd_kernel_t simd_kernel_from_vtable(const qsop_simd_vtable_t *simd) {
  const char *name = qsop_simd_kernel_name(simd);
  if (strcmp(name, "neon") == 0) {
    return QSOP_SIMD_KERNEL_NEON;
  }
  if (strcmp(name, "avx512") == 0) {
    return QSOP_SIMD_KERNEL_AVX512;
  }
  if (strcmp(name, "avx2") == 0) {
    return QSOP_SIMD_KERNEL_AVX2;
  }
  return QSOP_SIMD_KERNEL_SCALAR;
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

static const char *solve_mode_display_name(qsop_solve_mode_t mode, bool auto_mode) {
  return auto_mode ? "auto" : solve_mode_name(mode);
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
  return backend == SOLVE_BACKEND_RANKWIDTH || backend == SOLVE_BACKEND_TREEWIDTH
             ? "fourier"
             : "hybrid-fourier";
}

static bool branch_auto_refusal_is_safe_fallback(const qsop_error_t *error) {
  const char *message = error != NULL ? error->message : NULL;
  if (message == NULL) {
    return false;
  }
  return strstr(message, "requires R <= UINT32_MAX") != NULL ||
         strstr(message, "modulus > 2^32-1") != NULL || strstr(message, "forecast") != NULL ||
         strstr(message, "table") != NULL || strstr(message, "bag") != NULL ||
         strstr(message, "cap") != NULL || strstr(message, "refuses") != NULL ||
         strstr(message, "too large") != NULL || strstr(message, "memory-skip") != NULL ||
         strstr(message, "no delegate available") != NULL;
}

#define BRANCH_AUTO_SINGLE_FOURIER_MIN_VARS 16U
#define BRANCH_AUTO_SINGLE_FOURIER_MAX_WIDTH 25U
#define BRANCH_AUTO_COUNT_VECTOR_MAX_BYTES (UINT64_C(2) * 1024U * 1024U * 1024U)
/* What makes a single-Fourier solve affordable is the *width*, not the variable count: the DP table
 * is 2^(width+1) and the elimination is linear in nvars. This used to be 4096, which refused
 * qccq-gauntlet's qwalk-noancilla_8 -- 6715 variables, min-fill width 14, half a second of work --
 * purely for being big. It is now only a sanity bound against a pathological input; the width caps
 * and the delegate cost model are what decide solvability. */
#define BRANCH_AUTO_SINGLE_FOURIER_DEFAULT_MAX_VARS (1U << 24)

static bool branch_auto_count_vector_too_large(const qsop_instance_t *qsop) {
  /* Auto never exposes the residue vector, so avoid constructing a count table whose result
   * vector alone is already multi-gigabyte.  The branch count solver also needs working/cache
   * storage, so the measured peak is roughly twice this vector -- hence >= at the budget, not >:
   * a variable-free QFT at 27 qubits (r == 2^28, count vector == exactly 2 GiB, ~4 GiB resident)
   * collapses to a trivial constant-phase amplitude that single-Fourier returns in O(1) with
   * kilobytes, so there is never a reason to let the count table hit the budget on the nose. */
  return qsop != NULL && qsop->r >= BRANCH_AUTO_COUNT_VECTOR_MAX_BYTES / sizeof(uint64_t);
}

static bool branch_auto_should_start_single_fourier(const qsop_instance_t *qsop, uint32_t max_vars,
                                                    bool max_vars_set) {
  if (qsop == NULL || qsop->nedges == 0 || qsop->nvars < BRANCH_AUTO_SINGLE_FOURIER_MIN_VARS) {
    return false;
  }

  const uint32_t effective_max_vars =
      max_vars_set ? max_vars : BRANCH_AUTO_SINGLE_FOURIER_DEFAULT_MAX_VARS;
  if (effective_max_vars == 0 || qsop->nvars > effective_max_vars) {
    return false;
  }

  uint32_t *order = NULL;
  uint32_t width = 0;
  qsop_error_t probe_error = {0};
  const bool ok = qsop_treewidth_order_alloc(qsop, QSOP_TREEWIDTH_ORDER_MIN_FILL_MAX_DEGREE, &order,
                                             &width, NULL, &probe_error);
  free(order);
  return ok && width <= BRANCH_AUTO_SINGLE_FOURIER_MAX_WIDTH;
}

static bool branch_auto_prepare_treewidth_single(const qsop_instance_t *qsop, uint32_t max_vars,
                                                 bool max_vars_set,
                                                 qsop_branch_rw_source_t rw_source,
                                                 const qsop_branch_policy_t *policy,
                                                 uint32_t **order_out, uint32_t *width_out) {
  if (order_out == NULL || width_out == NULL) {
    return false;
  }
  *order_out = NULL;
  *width_out = 0;
  if (qsop == NULL || qsop->nedges == 0 || qsop->nvars < BRANCH_AUTO_SINGLE_FOURIER_MIN_VARS) {
    return false;
  }

  const uint32_t effective_max_vars =
      max_vars_set ? max_vars : BRANCH_AUTO_SINGLE_FOURIER_DEFAULT_MAX_VARS;
  if (effective_max_vars == 0 || qsop->nvars > effective_max_vars) {
    return false;
  }

  uint32_t *order = NULL;
  uint32_t width = 0;
  uint64_t dp_work = 0;
  qsop_error_t probe_error = {0};
  if (!qsop_treewidth_order_alloc(qsop, QSOP_TREEWIDTH_ORDER_MIN_FILL, &order, &width, &dp_work,
                                  &probe_error)) {
    free(order);
    return false;
  }
  if (width > BRANCH_AUTO_SINGLE_FOURIER_MAX_WIDTH) {
    free(order);
    return false;
  }
  /* With rankwidth enabled, only take the direct whole-instance treewidth path when the shared
   * cost model makes treewidth the clear winner even against the best feasible generated width;
   * otherwise defer to the branch recursion so rankwidth gets probed. Prefix cut-rank is cheap
   * (incremental GF(2)) but only distinguishes an edgeless rank-zero graph from a potentially
   * compressible one; if it cannot be computed, fall back to the direct treewidth path. */
  if (rw_source != QSOP_BRANCH_RW_SOURCE_NONE) {
    uint32_t cut_rank = 0;
    qsop_error_t cut_rank_error = {0};
    if (qsop_prefix_cut_rank(qsop->nvars, qsop->edge_u, qsop->edge_v, qsop->nedges, &cut_rank,
                             &cut_rank_error) &&
        !qsop_branch_single_treewidth_clearly_preferred(cut_rank, qsop->nvars, dp_work,
                                                        policy)) {
      free(order);
      return false;
    }
  }
  *order_out = order;
  *width_out = width;
  return true;
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

static const char *termination_reason_name(qsop_solve_termination_reason_t reason) {
  switch (reason) {
  case QSOP_SOLVE_TERMINATION_NONE:
    return "none";
  case QSOP_SOLVE_TERMINATION_MAX_FALLBACK_VARS:
    return "max_fallback_vars";
  case QSOP_SOLVE_TERMINATION_NO_DELEGATE:
    return "no_delegate";
  case QSOP_SOLVE_TERMINATION_CUTSET_BUDGET:
    return "cutset_budget";
  case QSOP_SOLVE_TERMINATION_MAX_SEARCH_NODES:
    return "max_search_nodes";
  case QSOP_SOLVE_TERMINATION_MAX_RECURSION_DEPTH:
    return "max_recursion_depth";
  case QSOP_SOLVE_TERMINATION_OTHER_ERROR:
    return "other_error";
  }
  return "other_error";
}

static const char *run_summary_reason_name(bool solved, const qsop_solve_stats_t *stats,
                                           const qsop_error_t *diagnostic) {
  const char *reason = termination_reason_name(stats->termination_reason);
  if (!solved &&
      (stats->termination_reason == QSOP_SOLVE_TERMINATION_NONE ||
       stats->termination_reason == QSOP_SOLVE_TERMINATION_OTHER_ERROR) &&
      diagnostic != NULL &&
      strstr(diagnostic->message, "pass a larger --max-vars") != NULL) {
    return "max_vars";
  }
  return reason;
}

static void jsonl_write_string(FILE *file, const char *text) {
  fputc('"', file);
  if (text != NULL) {
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
      switch (*p) {
      case '"':
        fputs("\\\"", file);
        break;
      case '\\':
        fputs("\\\\", file);
        break;
      case '\n':
        fputs("\\n", file);
        break;
      case '\r':
        fputs("\\r", file);
        break;
      case '\t':
        fputs("\\t", file);
        break;
      default:
        if (*p < 0x20U) {
          fprintf(file, "\\u%04x", (unsigned)*p);
        } else {
          fputc((int)*p, file);
        }
        break;
      }
    }
  }
  fputc('"', file);
}

static void write_run_summary_jsonl(FILE *file, const char *instance, bool solved,
                                    const qsop_solve_stats_t *stats,
                                    const qsop_error_t *diagnostic) {
  if (file == NULL || stats == NULL) {
    return;
  }
  const char *reason = run_summary_reason_name(solved, stats, diagnostic);
  const bool refused =
      !solved && strcmp(reason, "none") != 0 && strcmp(reason, "other_error") != 0;
  fputs("{\"schema\":\"sop_solve_run_stats_v1\",\"instance\":", file);
  jsonl_write_string(file, instance);
  fputs(",\"status\":", file);
  jsonl_write_string(file, solved ? "solved" : (refused ? "refused" : "error"));
  fputs(",\"reason\":", file);
  jsonl_write_string(file, reason);
  fprintf(file,
          ",\"search_nodes\":%" PRIu64 ",\"leaf_assignments\":%" PRIu64
          ",\"active_vars_at_failure\":%" PRIu32
          ",\"active_edges_at_failure\":%" PRIu32
          ",\"cache_hits\":%" PRIu64 ",\"cache_misses\":%" PRIu64
          ",\"treewidth_delegations\":%" PRIu64
          ",\"rankwidth_delegations\":%" PRIu64
          ",\"branch_fallthroughs\":%" PRIu64
          ",\"branch_propagations\":%" PRIu64 ",\"branch_zero_prunes\":%" PRIu64
          ",\"branch_materialized_calls\":%" PRIu64
          ",\"branch_materialized_eliminations\":%" PRIu64
          ",\"branch_materialized_degree2_merges\":%" PRIu64
          ",\"branch_materialized_reduction_ns\":%" PRIu64
          ",\"branch_conditioning_nodes\":%" PRIu64
          ",\"branch_conditioning_lookaheads\":%" PRIu64
          ",\"branch_delegate_probes\":%" PRIu64
          ",\"branch_delegate_probe_skips\":%" PRIu64
          ",\"branch_cutset_size\":%" PRIu64
          ",\"branch_max_cutset_depth\":%" PRIu32
          ",\"branch_cutset_initial_vars\":%" PRIu32
          ",\"branch_cutset_initial_edges\":%" PRIu32
          ",\"branch_cutset_final_vars\":%" PRIu32
          ",\"branch_cutset_final_edges\":%" PRIu32
          ",\"branch_cutset_stagnant_levels\":%" PRIu32
          ",\"branch_last_delegate_miss\":%" PRIu32
          ",\"treewidth_factor_scope_tests\":%" PRIu64
          ",\"treewidth_factor_bucket_visits\":%" PRIu64
          ",\"treewidth_factor_multiplications\":%" PRIu64
          ",\"treewidth_factor_allocations\":%" PRIu64
          ",\"treewidth_factor_discovery_ns\":%" PRIu64
          ",\"treewidth_numeric_join_ns\":%" PRIu64
          ",\"treewidth_sum_out_ns\":%" PRIu64
          ",\"treewidth_peak_live_bytes\":%" PRIu64
          ",\"treewidth_pool_retained_bytes\":%" PRIu64
          ",\"treewidth_largest_allocation_bytes\":%" PRIu64,
          stats->search_nodes, stats->leaf_assignments, stats->failure_active_vars,
          stats->failure_active_edges, stats->cache_hits, stats->cache_misses,
          stats->treewidth_delegations, stats->rankwidth_delegations, stats->branch_fallthroughs,
          stats->branch_propagations, stats->branch_zero_prunes,
          stats->branch_materialized_calls, stats->branch_materialized_eliminations,
          stats->branch_materialized_degree2_merges, stats->branch_materialized_reduction_ns,
          stats->branch_conditioning_nodes, stats->branch_conditioning_lookaheads,
          stats->branch_delegate_probes, stats->branch_delegate_probe_skips,
          stats->branch_cutset_size, stats->branch_max_cutset_depth,
          stats->branch_cutset_initial_vars, stats->branch_cutset_initial_edges,
          stats->branch_cutset_final_vars, stats->branch_cutset_final_edges,
          stats->branch_cutset_stagnant_levels, stats->branch_last_delegate_miss,
          stats->treewidth_factor_scope_tests, stats->treewidth_factor_bucket_visits,
          stats->treewidth_factor_multiplications, stats->treewidth_factor_allocations,
          stats->treewidth_factor_discovery_ns, stats->treewidth_numeric_join_ns,
          stats->treewidth_sum_out_ns, stats->treewidth_peak_live_bytes,
          stats->treewidth_pool_retained_bytes, stats->treewidth_largest_allocation_bytes);
  fputs(",\"diagnostic\":", file);
  if (!solved && diagnostic != NULL && diagnostic->message[0] != '\0') {
    jsonl_write_string(file, diagnostic->message);
  } else {
    fputs("null", file);
  }
  fputs("}\n", file);
  (void)fflush(file);
}

static bool write_solver_stats(FILE *file, solve_backend_t backend, const qsop_solve_stats_t *stats,
                               qsop_solve_mode_t solve_mode, bool solve_mode_set,
                               bool solve_mode_auto, qsop_rankwidth_solve_mode_t rankwidth_mode,
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
  if (solve_mode_auto || solve_mode_set || solve_mode != QSOP_SOLVE_MODE_COUNT_TABLE) {
    fprintf(file, "solve_mode: %s\n", solve_mode_display_name(solve_mode, solve_mode_auto));
    fprintf(file, "solve_mode_kernel: %s\n", solve_mode_kernel_name(backend, solve_mode));
  }
  if (stats->simd_kernel != 0) {
    fprintf(file, "simd_kernel: %s\n",
            simd_kernel_display_name((qsop_simd_kernel_t)stats->simd_kernel));
  }
  if (stats->single_mode_precision != 0) {
    fprintf(file, "single_mode_precision: %s\n",
            single_mode_precision_name((single_mode_precision_cli_t)stats->single_mode_precision));
  }
  if (stats->treewidth_single_complex_kernel != 0) {
    fprintf(file, "treewidth_single_complex_kernel: %" PRIu32 "\n",
            stats->treewidth_single_complex_kernel);
  }
  if (stats->rankwidth_single_complex_kernel != 0) {
    fprintf(file, "rankwidth_single_complex_kernel: %" PRIu32 "\n",
            stats->rankwidth_single_complex_kernel);
  }
  if (stats->bitset_kernel != 0) {
    fprintf(file, "bitset_kernel: %s\n",
            simd_kernel_display_name((qsop_simd_kernel_t)stats->bitset_kernel));
  }
  /* Printed whenever any complex op was counted, so a zero is visible: `simd_kernel: avx2` with no
   * vectorized ops means the active precision or backend has no kernels, not that they were fast.
   */
  if (stats->simd_vectorized_ops != 0 || stats->simd_scalar_fallback_ops != 0) {
    fprintf(file, "simd_vectorized_ops: %" PRIu64 "\n", stats->simd_vectorized_ops);
  }
  if (stats->simd_scalar_fallback_ops != 0 || stats->simd_vectorized_ops != 0) {
    fprintf(file, "simd_scalar_fallback_ops: %" PRIu64 "\n", stats->simd_scalar_fallback_ops);
  }
  if (stats->treewidth_factor_allocations != 0U) {
    fprintf(file, "treewidth_factor_scope_tests: %" PRIu64 "\n",
            stats->treewidth_factor_scope_tests);
    fprintf(file, "treewidth_factor_bucket_visits: %" PRIu64 "\n",
            stats->treewidth_factor_bucket_visits);
    fprintf(file, "treewidth_factor_multiplications: %" PRIu64 "\n",
            stats->treewidth_factor_multiplications);
    fprintf(file, "treewidth_factor_allocations: %" PRIu64 "\n",
            stats->treewidth_factor_allocations);
    fprintf(file, "treewidth_factor_discovery_ns: %" PRIu64 "\n",
            stats->treewidth_factor_discovery_ns);
    fprintf(file, "treewidth_numeric_join_ns: %" PRIu64 "\n", stats->treewidth_numeric_join_ns);
    fprintf(file, "treewidth_sum_out_ns: %" PRIu64 "\n", stats->treewidth_sum_out_ns);
    fprintf(file, "treewidth_peak_live_bytes: %" PRIu64 "\n", stats->treewidth_peak_live_bytes);
    fprintf(file, "treewidth_pool_retained_bytes: %" PRIu64 "\n",
            stats->treewidth_pool_retained_bytes);
    fprintf(file, "treewidth_largest_allocation_bytes: %" PRIu64 "\n",
            stats->treewidth_largest_allocation_bytes);
  }
  if (backend == SOLVE_BACKEND_BRANCH) {
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
      fprintf(file, "cache_canonical_lookups: %" PRIu64 "\n", stats->cache_canonical_lookups);
    }
    if (stats->cache_canonical_stores != 0) {
      fprintf(file, "cache_canonical_stores: %" PRIu64 "\n", stats->cache_canonical_stores);
    }
    fprintf(file, "cache_entries: %" PRIu64 "\n", stats->cache_entries);
    if (stats->cache_canonical_entries != 0) {
      fprintf(file, "cache_canonical_entries: %" PRIu64 "\n", stats->cache_canonical_entries);
    }
    fprintf(file, "cache_stored_residue_slots: %" PRIu64 "\n", stats->cache_stored_residue_slots);
    if (stats->cache_estimated_bytes != 0) {
      fprintf(file, "cache_key_bytes: %" PRIu64 "\n", stats->cache_key_bytes);
      fprintf(file, "cache_count_bytes: %" PRIu64 "\n", stats->cache_count_bytes);
      fprintf(file, "cache_estimated_bytes: %" PRIu64 "\n", stats->cache_estimated_bytes);
    }
    fprintf(file, "leaf_assignments: %" PRIu64 "\n", stats->leaf_assignments);
    if (stats->branch_materialized_calls != 0U || stats->branch_conditioning_nodes != 0U ||
        stats->branch_delegate_probe_skips != 0U) {
      fprintf(file, "branch_materialized_calls: %" PRIu64 "\n",
              stats->branch_materialized_calls);
      fprintf(file, "branch_materialized_eliminations: %" PRIu64 "\n",
              stats->branch_materialized_eliminations);
      fprintf(file, "branch_materialized_degree2_merges: %" PRIu64 "\n",
              stats->branch_materialized_degree2_merges);
      fprintf(file, "branch_materialized_reduction_ns: %" PRIu64 "\n",
              stats->branch_materialized_reduction_ns);
      fprintf(file, "branch_conditioning_nodes: %" PRIu64 "\n",
              stats->branch_conditioning_nodes);
      fprintf(file, "branch_conditioning_lookaheads: %" PRIu64 "\n",
              stats->branch_conditioning_lookaheads);
      fprintf(file, "branch_delegate_probes: %" PRIu64 "\n", stats->branch_delegate_probes);
      fprintf(file, "branch_delegate_probe_skips: %" PRIu64 "\n",
              stats->branch_delegate_probe_skips);
      fprintf(file, "branch_max_cutset_depth: %" PRIu32 "\n", stats->branch_max_cutset_depth);
    }
    if (stats->treewidth_delegations != 0 || stats->rankwidth_delegations != 0 ||
        stats->branch_fallthroughs != 0 || stats->branch_treewidth_skips != 0 ||
        stats->branch_rankwidth_skips != 0) {
      fprintf(file, "treewidth_delegations: %" PRIu64 "\n", stats->treewidth_delegations);
      fprintf(file, "rankwidth_delegations: %" PRIu64 "\n", stats->rankwidth_delegations);
      fprintf(file, "branch_fallthroughs: %" PRIu64 "\n", stats->branch_fallthroughs);
      fprintf(file, "branch_treewidth_skips: %" PRIu64 "\n", stats->branch_treewidth_skips);
      fprintf(file, "branch_rankwidth_skips: %" PRIu64 "\n", stats->branch_rankwidth_skips);
      if (stats->branch_propagations != 0 || stats->branch_zero_prunes != 0) {
        fprintf(file, "branch_propagations: %" PRIu64 "\n", stats->branch_propagations);
        fprintf(file, "branch_zero_prunes: %" PRIu64 "\n", stats->branch_zero_prunes);
      }
      if (stats->branch_width_probes != 0 || stats->branch_probe_skips != 0) {
        fprintf(file, "branch_width_probes: %" PRIu64 "\n", stats->branch_width_probes);
        fprintf(file, "branch_probe_skips: %" PRIu64 "\n", stats->branch_probe_skips);
      }
      if (stats->branch_cutset_size != 0) {
        fprintf(file, "branch_cutset_size: %" PRIu64 "\n", stats->branch_cutset_size);
      }
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
      if (stats->rankwidth_cutrank_width != 0) {
        fprintf(file, "rankwidth_cutrank_width: %" PRIu32 "\n", stats->rankwidth_cutrank_width);
        fprintf(file, "rankwidth_table_forecast: %" PRIu64 "\n", stats->rankwidth_table_forecast);
        fprintf(file, "rankwidth_join_pair_forecast: %" PRIu64 "\n",
                stats->rankwidth_join_pair_forecast);
      }
      fprintf(file, "table_entries: %" PRIu64 "\n", stats->table_entries);
      fprintf(file, "max_table_entries: %" PRIu64 "\n", stats->max_table_entries);
      fprintf(file, "join_pairs: %" PRIu64 "\n", stats->join_pairs);
    }
  } else if (backend == SOLVE_BACKEND_RANKWIDTH) {
    fprintf(file, "rankwidth_mode: %s\n", rankwidth_mode_name(rankwidth_mode));
    if (rankwidth_mode == QSOP_RANKWIDTH_SOLVE_FOURIER) {
      fprintf(file, "rankwidth_fourier_kernel: %s\n",
              rankwidth_fourier_kernel_name(
                  (qsop_rankwidth_fourier_kernel_t)stats->rankwidth_fourier_kernel));
    }
    fprintf(file, "rankwidth_decomposition: %s\n", rankwidth_decomposition);
    fprintf(file, "decomposition_width: %" PRIu32 "\n", stats->decomposition_width);
    fprintf(file, "rankwidth_cutrank_width: %" PRIu32 "\n", stats->rankwidth_cutrank_width);
    fprintf(file, "table_entries: %" PRIu64 "\n", stats->table_entries);
    fprintf(file, "max_table_entries: %" PRIu64 "\n", stats->max_table_entries);
    fprintf(file, "signature_entries: %" PRIu64 "\n", stats->signature_entries);
    fprintf(file, "max_signature_entries: %" PRIu64 "\n", stats->max_signature_entries);
    fprintf(file, "join_pairs: %" PRIu64 "\n", stats->join_pairs);
    fprintf(file, "join_signature_pairs: %" PRIu64 "\n", stats->join_signature_pairs);
    fprintf(file, "rankwidth_table_forecast: %" PRIu64 "\n", stats->rankwidth_table_forecast);
    fprintf(file, "rankwidth_join_pair_forecast: %" PRIu64 "\n",
            stats->rankwidth_join_pair_forecast);
    fprintf(file, "rankwidth_dense_table_forecast: %" PRIu64 "\n",
            stats->rankwidth_dense_table_forecast);
    fprintf(file, "rankwidth_dense_even_join_forecast: %" PRIu64 "\n",
            stats->rankwidth_dense_even_join_forecast);
    fprintf(file, "rankwidth_transition_bytes: %" PRIu64 "\n", stats->rankwidth_transition_bytes);
    fprintf(file, "rankwidth_transition_layout_u16_events: %" PRIu64 "\n",
            stats->rankwidth_transition_layout_u16_events);
    fprintf(file, "rankwidth_transition_layout_u32_events: %" PRIu64 "\n",
            stats->rankwidth_transition_layout_u32_events);
    fprintf(file, "rankwidth_dense_join_events: %" PRIu64 "\n", stats->rankwidth_dense_join_events);
    fprintf(file, "rankwidth_materialized_join_events: %" PRIu64 "\n",
            stats->rankwidth_materialized_join_events);
    fprintf(file, "rankwidth_streaming_join_events: %" PRIu64 "\n",
            stats->rankwidth_streaming_join_events);
    fprintf(file, "rankwidth_streaming_join_candidate_pairs: %" PRIu64 "\n",
            stats->rankwidth_streaming_join_candidate_pairs);
    fprintf(file, "rankwidth_streaming_join_emitted_pairs: %" PRIu64 "\n",
            stats->rankwidth_streaming_join_emitted_pairs);
    fprintf(file, "rankwidth_linear_transition_events: %" PRIu64 "\n",
            stats->rankwidth_linear_transition_events);
    fprintf(file, "rankwidth_table_assignment_bytes: %" PRIu64 "\n",
            stats->rankwidth_table_assignment_bytes);
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

static bool parse_decimal_limbs(const char *text, uint32_t **out_limbs, size_t *out_nlimbs,
                                qsop_error_t *error) {
  const size_t len = strlen(text);
  if (len == 0) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "empty exact count string");
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    if (text[i] < '0' || text[i] > '9') {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message), "invalid exact count string");
      return false;
    }
  }

  const size_t cap = (len + 8U) / 9U;
  uint32_t *limbs = calloc(cap == 0 ? 1U : cap, sizeof(*limbs));
  if (limbs == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "out of memory while parsing exact count");
    return false;
  }

  size_t nlimbs = 0;
  for (size_t end = len; end > 0;) {
    const size_t start = end > 9U ? end - 9U : 0U;
    uint32_t limb = 0;
    for (size_t i = start; i < end; i++) {
      limb = limb * 10U + (uint32_t)(text[i] - '0');
    }
    limbs[nlimbs++] = limb;
    end = start;
  }
  while (nlimbs > 0 && limbs[nlimbs - 1U] == 0) {
    nlimbs--;
  }

  *out_limbs = limbs;
  *out_nlimbs = nlimbs;
  return true;
}

static int compare_limbs(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
  if (na != nb) {
    return na > nb ? 1 : -1;
  }
  while (na > 0) {
    na--;
    if (a[na] != b[na]) {
      return a[na] > b[na] ? 1 : -1;
    }
  }
  return 0;
}

static void subtract_limbs(const uint32_t *larger, size_t nlarger, const uint32_t *smaller,
                           size_t nsmaller, uint32_t *out, size_t *out_n) {
  int64_t borrow = 0;
  for (size_t i = 0; i < nlarger; i++) {
    int64_t value = (int64_t)larger[i] - borrow;
    if (i < nsmaller) {
      value -= (int64_t)smaller[i];
    }
    if (value < 0) {
      value += 1000000000LL;
      borrow = 1;
    } else {
      borrow = 0;
    }
    out[i] = (uint32_t)value;
  }
  size_t n = nlarger;
  while (n > 0 && out[n - 1U] == 0) {
    n--;
  }
  *out_n = n;
}

static long double limbs_to_long_double(const uint32_t *limbs, size_t nlimbs) {
  long double value = 0.0L;
  while (nlimbs > 0) {
    nlimbs--;
    value = value * 1000000000.0L + (long double)limbs[nlimbs];
  }
  return value;
}

static bool result_count_difference_long_double(const qsop_result_t *result, uint32_t left,
                                                uint32_t right, long double *out,
                                                qsop_error_t *error) {
  if (result->count_strings == NULL) {
    long double a = 0.0L;
    long double b = 0.0L;
    if (!result_count_long_double(result, left, &a, error) ||
        !result_count_long_double(result, right, &b, error)) {
      return false;
    }
    *out = a - b;
    return true;
  }

  uint32_t *left_limbs = NULL;
  uint32_t *right_limbs = NULL;
  uint32_t *diff_limbs = NULL;
  size_t left_n = 0;
  size_t right_n = 0;
  bool ok = parse_decimal_limbs(result->count_strings[left], &left_limbs, &left_n, error) &&
            parse_decimal_limbs(result->count_strings[right], &right_limbs, &right_n, error);
  if (!ok) {
    free(left_limbs);
    free(right_limbs);
    return false;
  }

  const int cmp = compare_limbs(left_limbs, left_n, right_limbs, right_n);
  if (cmp == 0) {
    free(left_limbs);
    free(right_limbs);
    *out = 0.0L;
    return true;
  }

  const size_t diff_cap = left_n > right_n ? left_n : right_n;
  diff_limbs = calloc(diff_cap == 0 ? 1U : diff_cap, sizeof(*diff_limbs));
  if (diff_limbs == NULL) {
    free(left_limbs);
    free(right_limbs);
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "out of memory while subtracting exact counts");
    return false;
  }

  size_t diff_n = 0;
  if (cmp > 0) {
    subtract_limbs(left_limbs, left_n, right_limbs, right_n, diff_limbs, &diff_n);
    *out = limbs_to_long_double(diff_limbs, diff_n);
  } else {
    subtract_limbs(right_limbs, right_n, left_limbs, left_n, diff_limbs, &diff_n);
    *out = -limbs_to_long_double(diff_limbs, diff_n);
  }

  free(left_limbs);
  free(right_limbs);
  free(diff_limbs);
  return true;
}

static bool result_to_amplitude(const qsop_result_t *result, uint32_t target_mode,
                                qsop_amplitude_t *out, qsop_error_t *error) {
  if (result == NULL || out == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "internal error: null amplitude conversion argument");
    return false;
  }
  static const long double two_pi = 6.283185307179586476925286766559005768394338798750211641949889L;
  long double real = 0.0L;
  long double imag = 0.0L;
  const uint32_t r = result->r;
  if (result->count_strings != NULL && r % 2U == 0U && target_mode % 2U == 1U) {
    const uint32_t half = r / 2U;
    for (uint32_t residue = 0; residue < half; residue++) {
      long double count = 0.0L;
      if (!result_count_difference_long_double(result, residue, residue + half, &count, error)) {
        return false;
      }
      const long double angle =
          two_pi * (long double)(((uint64_t)target_mode * residue) % r) / (long double)r;
      real += count * cosl(angle);
      imag += count * sinl(angle);
    }
    *out = (qsop_amplitude_t){
        .re = real,
        .im = imag,
        .numeric_error_bound = 0.0L,
    };
    return true;
  }

  for (uint32_t residue = 0; residue < r; residue++) {
    long double count = 0.0L;
    if (!result_count_long_double(result, residue, &count, error)) {
      return false;
    }
    const long double angle =
        two_pi * (long double)(((uint64_t)target_mode * residue) % r) / (long double)r;
    real += count * cosl(angle);
    imag += count * sinl(angle);
  }
  *out = (qsop_amplitude_t){
      .re = real,
      .im = imag,
      .numeric_error_bound = 0.0L,
  };
  return true;
}

/* amplitude_re/amplitude_im are the *normalized* amplitude, amp * 2^(-norm_h/2): the physical
 * <y|C|x>, whose modulus a QSOP's semantics bound by 1. The raw sum-over-paths value grows like
 * 2^nvars and is simply not representable on the larger instances (qccq-gauntlet's
 * qwalk-noancilla_11 has |amplitude| about 2^29670), which is why the solvers hand back a mantissa
 * and an exponent and the normalization happens here. norm_h is printed so the raw value stays
 * recoverable. */
static bool write_amplitude_output(FILE *file, const char *solve_mode,
                                   const char *solve_mode_kernel, uint32_t target_mode,
                                   const qsop_amplitude_t *amplitude, uint64_t norm_h,
                                   qsop_error_t *error) {
  if (file == NULL || solve_mode == NULL || solve_mode_kernel == NULL || amplitude == NULL) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "internal error: null amplitude-output argument");
    return false;
  }
  long double re = 0.0L;
  long double im = 0.0L;
  if (!qsop_amplitude_normalized(amplitude, norm_h, &re, &im)) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message),
             "normalized amplitude is not representable (scale 2^%" PRId32 ", norm_h %" PRIu64 ")",
             amplitude->scale_exp2, norm_h);
    return false;
  }
  fprintf(file, "mode: amplitude\n");
  fprintf(file, "solve_mode: %s\n", solve_mode);
  fprintf(file, "solve_mode_kernel: %s\n", solve_mode_kernel);
  fprintf(file, "fourier_target_mode: %" PRIu32 "\n", target_mode);
  fprintf(file, "norm_h: %" PRIu64 "\n", norm_h);
  fprintf(file, "amplitude_re: %.17Lg\n", re);
  fprintf(file, "amplitude_im: %.17Lg\n", im);
  fprintf(file, "numeric_error_bound: %.17Lg\n", amplitude->numeric_error_bound);
  if (ferror(file)) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "write failed: %s", strerror(errno));
    return false;
  }
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

  qsop_amplitude_t amplitude = {0};
  if (!result_to_amplitude(result, 1U, &amplitude, error)) {
    return false;
  }

  long double norm_re = 0.0L;
  long double norm_im = 0.0L;
  if (!qsop_amplitude_normalized(&amplitude, result->norm_h, &norm_re, &norm_im)) {
    error->path = NULL;
    error->line = 0;
    error->column = 0;
    snprintf(error->message, sizeof(error->message), "normalized amplitude is not representable");
    return false;
  }
  const long double probability = norm_re * norm_re + norm_im * norm_im;
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
  return dlx4sop_cli_parse_u32("--max-vars", text, out);
}

static bool parse_u32_arg(const char *flag, const char *text, uint32_t *out) {
  return dlx4sop_cli_parse_u32(flag, text, out);
}

static bool parse_u64_arg(const char *flag, const char *text, uint64_t *out) {
  return dlx4sop_cli_parse_u64(flag, text, out);
}

static bool parse_double_arg(const char *flag, const char *text, double *out) {
  return dlx4sop_cli_parse_double(flag, text, out);
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *rankwidth_decomposition_path = NULL;
  const char *rankwidth_decomposition_label = "left-deep";
  const char *rankwidth_dump_path = NULL;
  uint32_t max_vars = 24;
  solve_backend_t backend = SOLVE_BACKEND_BRANCH;
  qsop_solve_mode_t solve_mode = QSOP_SOLVE_MODE_COUNT_TABLE;
  qsop_branch_heuristic_t branch_heuristic = QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH;
  qsop_branch_rw_source_t branch_rw_source = QSOP_BRANCH_RW_SOURCE_AUTO;
  qsop_branch_policy_t branch_policy = {0}; /* zeros → defaults applied in branch.c */
  qsop_branch_single_fallback_t branch_single_fallback = QSOP_BRANCH_SINGLE_FALLBACK_AUTO;
  qsop_branch_single_propagate_t branch_single_propagate = QSOP_BRANCH_SINGLE_PROPAGATE_AUTO;
  qsop_branch_single_precision_t branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_AUTO;
  qsop_branch_single_kernel_t branch_single_kernel = QSOP_BRANCH_SINGLE_KERNEL_AUTO;
  uint64_t branch_single_max_search_nodes = 0;
  uint32_t branch_single_max_fallback_vars = 0;
  uint64_t branch_single_max_dp_work = 0;
  uint64_t branch_single_cache_budget_mib = 0;
  uint32_t branch_single_cache_min_vars = 0;
  bool branch_single_materialized_reduction = false;
  bool branch_single_diagnose_conditioning = false;
  uint32_t branch_single_cutset_depth = 0;
  uint32_t branch_single_lookahead_candidates = 0;
  uint64_t branch_single_max_conditioning_nodes = 0;
  uint32_t branch_single_delegate_reprobe_interval = 0;
  uint32_t branch_single_max_stagnant_levels = 0;
  bool branch_single_option_set = false;
  bool branch_single_fallback_set = false;
  bool branch_single_precision_set = false;
  qsop_rankwidth_generator_t rankwidth_generator = QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP;
  qsop_rankwidth_solve_mode_t rankwidth_mode = QSOP_RANKWIDTH_SOLVE_COUNT_TABLE;
  qsop_treewidth_order_t treewidth_order = QSOP_TREEWIDTH_ORDER_MIN_FILL;
  bool branch_heuristic_set = false;
  bool branch_rw_source_set = false;
  bool rankwidth_generator_set = false;
  bool rankwidth_mode_set = false;
  bool solve_mode_set = false;
  bool solve_mode_auto = false;
  bool max_vars_set = false;
  bool single_fourier_mode = false;
  uint32_t fourier_target_mode = 1;
  bool fourier_target_mode_set = false;
  single_mode_precision_cli_t single_mode_precision = SINGLE_MODE_PRECISION_AUTO;
  bool single_mode_precision_set = false;
  bool print_kernels = false;
  bool treewidth_order_set = false;
  bool include_result = false;
  bool include_probability = false;
  const char *stats_jsonl_path = NULL;
  bool calibrate_backends = false;
  uint64_t rw_memory_budget_bytes = 0; /* 0 = no limit */
  rankwidth_memory_policy_t rw_memory_policy = RW_MEMORY_POLICY_SKIP;
  qsop_rankwidth_join_strategy_t rw_join_strategy = QSOP_RANKWIDTH_JOIN_AUTO;
  qsop_rankwidth_single_kernel_t rw_single_kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO;
  bool rw_single_kernel_set = false;
  qsop_rankwidth_fourier_kernel_t rw_fourier_kernel = QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO;
  bool rw_fourier_kernel_set = false;
  uint64_t rw_materialize_join_max_pairs = 0; /* 0 = use built-in default */
  solve_output_format_t format = SOLVE_FORMAT_AMPLITUDE;
  solve_trace_format_t trace_format = SOLVE_TRACE_NONE;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--help-advanced") == 0) {
      print_usage_mode(stdout, true);
      return 0;
    }
    if (dlx4sop_cli_is_version_arg(argv[i])) {
      dlx4sop_cli_print_version(stdout, "sop-solve");
      return 0;
    }
    if (strcmp(argv[i], "--format") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --format requires a value\n", stderr);
        return 2;
      }
      const char *format_value = argv[++i];
      if (strcmp(format_value, "amplitude") == 0) {
        format = SOLVE_FORMAT_AMPLITUDE;
      } else if (strcmp(format_value, "residue-vector") == 0) {
        format = SOLVE_FORMAT_RESIDUE_VECTOR;
      } else if (strcmp(format_value, "stats") == 0) {
        format = SOLVE_FORMAT_STATS;
      } else {
        fprintf(stderr, "error: unsupported format '%s'\n", format_value);
        return 2;
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
      if (i + 1 >= argc) {
        fputs("error: --max-vars requires a non-negative uint32 value\n", stderr);
        return 2;
      }
      if (!parse_max_vars(argv[++i], &max_vars)) {
        return 2;
      }
      max_vars_set = true;
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
    if (strcmp(argv[i], "--rankwidth-memory-budget-mib") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-memory-budget-mib requires an integer value\n", stderr);
        return 2;
      }
      uint64_t mib;
      if (!parse_u64_arg("--rankwidth-memory-budget-mib", argv[++i], &mib))
        return 2;
      rw_memory_budget_bytes = mib * 1024ULL * 1024ULL;
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-memory-budget-bytes") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-memory-budget-bytes requires an integer value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--rankwidth-memory-budget-bytes", argv[++i], &rw_memory_budget_bytes))
        return 2;
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-memory-policy") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-memory-policy requires skip|fallback|hard-error\n", stderr);
        return 2;
      }
      const char *pol = argv[++i];
      if (strcmp(pol, "skip") == 0) {
        rw_memory_policy = RW_MEMORY_POLICY_SKIP;
      } else if (strcmp(pol, "fallback") == 0) {
        rw_memory_policy = RW_MEMORY_POLICY_FALLBACK;
      } else if (strcmp(pol, "hard-error") == 0) {
        rw_memory_policy = RW_MEMORY_POLICY_HARD_ERROR;
      } else {
        fprintf(stderr, "error: unknown memory policy '%s' (expected skip|fallback|hard-error)\n",
                pol);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-join-strategy") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-join-strategy requires auto|materialized|streaming\n", stderr);
        return 2;
      }
      const char *val = argv[++i];
      if (strcmp(val, "auto") == 0) {
        rw_join_strategy = QSOP_RANKWIDTH_JOIN_AUTO;
      } else if (strcmp(val, "materialized") == 0) {
        rw_join_strategy = QSOP_RANKWIDTH_JOIN_MATERIALIZED;
      } else if (strcmp(val, "streaming") == 0) {
        rw_join_strategy = QSOP_RANKWIDTH_JOIN_STREAMING;
      } else {
        fprintf(stderr,
                "error: unknown join strategy '%s' (expected auto|materialized|streaming)\n", val);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-materialize-join-max-pairs") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-materialize-join-max-pairs requires an integer value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--rankwidth-materialize-join-max-pairs", argv[++i],
                         &rw_materialize_join_max_pairs))
        return 2;
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-single-kernel") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-single-kernel requires auto|streaming|materialized|dense\n",
              stderr);
        return 2;
      }
      const char *val = argv[++i];
      if (strcmp(val, "auto") == 0) {
        rw_single_kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO;
      } else if (strcmp(val, "streaming") == 0) {
        rw_single_kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_STREAMING;
      } else if (strcmp(val, "materialized") == 0) {
        rw_single_kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED;
      } else if (strcmp(val, "dense") == 0) {
        rw_single_kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_DENSE;
      } else {
        fprintf(stderr,
                "error: unknown rankwidth single-mode kernel '%s' "
                "(expected auto|streaming|materialized|dense)\n",
                val);
        return 2;
      }
      rw_single_kernel_set = true;
      continue;
    }
    if (strcmp(argv[i], "--rankwidth-fourier-kernel") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --rankwidth-fourier-kernel requires "
              "auto|streaming|hybrid-even-fwht|dense-reference\n",
              stderr);
        return 2;
      }
      const char *val = argv[++i];
      if (strcmp(val, "auto") == 0) {
        rw_fourier_kernel = QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO;
      } else if (strcmp(val, "streaming") == 0) {
        rw_fourier_kernel = QSOP_RANKWIDTH_FOURIER_KERNEL_STREAMING;
      } else if (strcmp(val, "hybrid-even-fwht") == 0) {
        rw_fourier_kernel = QSOP_RANKWIDTH_FOURIER_KERNEL_HYBRID_EVEN_FWHT;
      } else if (strcmp(val, "dense-reference") == 0) {
        rw_fourier_kernel = QSOP_RANKWIDTH_FOURIER_KERNEL_DENSE_REFERENCE;
      } else {
        fprintf(stderr,
                "error: unknown rankwidth Fourier kernel '%s' "
                "(expected auto|streaming|hybrid-even-fwht|dense-reference)\n",
                val);
        return 2;
      }
      rw_fourier_kernel_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-heuristic") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-heuristic requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "delegation-depth") == 0) {
        branch_heuristic = QSOP_BRANCH_HEURISTIC_DELEGATION_DEPTH;
      } else if (strcmp(value, "split") == 0) {
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
      } else if (strcmp(value, "none") == 0) {
        branch_rw_source = QSOP_BRANCH_RW_SOURCE_NONE;
      } else {
        fprintf(stderr, "error: unsupported --branch-rw-source '%s'\n", value);
        return 2;
      }
      branch_rw_source_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-rw-min-speedup") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-rw-min-speedup requires a value\n", stderr);
        return 2;
      }
      if (!parse_double_arg("--branch-rw-min-speedup", argv[++i], &branch_policy.rw_min_speedup))
        return 2;
      continue;
    }
    if (strcmp(argv[i], "--branch-rw-fixed-overhead-ns") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-rw-fixed-overhead-ns requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-rw-fixed-overhead-ns", argv[++i],
                         &branch_policy.rw_fixed_overhead_ns))
        return 2;
      continue;
    }
    if (strcmp(argv[i], "--branch-tw-fixed-overhead-ns") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-tw-fixed-overhead-ns requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-tw-fixed-overhead-ns", argv[++i],
                         &branch_policy.tw_fixed_overhead_ns))
        return 2;
      continue;
    }
    if (strcmp(argv[i], "--branch-rw-memory-penalty-ns") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-rw-memory-penalty-ns requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-rw-memory-penalty-ns", argv[++i],
                         &branch_policy.rw_memory_penalty_ns))
        return 2;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-propagate") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-propagate requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "auto") == 0) {
        branch_single_propagate = QSOP_BRANCH_SINGLE_PROPAGATE_AUTO;
      } else if (strcmp(value, "off") == 0) {
        branch_single_propagate = QSOP_BRANCH_SINGLE_PROPAGATE_OFF;
      } else {
        fprintf(stderr, "error: unsupported --branch-single-propagate '%s' (expected auto|off)\n",
                value);
        return 2;
      }
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-materialized-reduction") == 0) {
      branch_single_materialized_reduction = true;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-diagnose-conditioning") == 0) {
      branch_single_diagnose_conditioning = true;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-cutset-depth") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-cutset-depth requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--branch-single-cutset-depth", argv[++i],
                         &branch_single_cutset_depth))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-lookahead-candidates") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-lookahead-candidates requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--branch-single-lookahead-candidates", argv[++i],
                         &branch_single_lookahead_candidates))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-max-conditioning-nodes") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-max-conditioning-nodes requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-single-max-conditioning-nodes", argv[++i],
                         &branch_single_max_conditioning_nodes))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-delegate-reprobe-interval") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-delegate-reprobe-interval requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--branch-single-delegate-reprobe-interval", argv[++i],
                         &branch_single_delegate_reprobe_interval))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-max-stagnant-levels") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-max-stagnant-levels requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--branch-single-max-stagnant-levels", argv[++i],
                         &branch_single_max_stagnant_levels))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-fourier-fallback") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-fourier-fallback requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "auto") == 0) {
        branch_single_fallback = QSOP_BRANCH_SINGLE_FALLBACK_AUTO;
      } else if (strcmp(value, "delegate-only") == 0 || strcmp(value, "never") == 0 ||
                 strcmp(value, "off") == 0) {
        branch_single_fallback = QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY;
      } else if (strcmp(value, "always") == 0) {
        branch_single_fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS;
      } else {
        fprintf(stderr,
                "error: unsupported --branch-single-fourier-fallback '%s' "
                "(expected auto|delegate-only|always)\n",
                value);
        return 2;
      }
      branch_single_option_set = true;
      branch_single_fallback_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-max-fallback-vars") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-max-fallback-vars requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--branch-single-max-fallback-vars", argv[++i],
                         &branch_single_max_fallback_vars))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-delegate-max-dp-work") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-delegate-max-dp-work requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-single-delegate-max-dp-work", argv[++i],
                         &branch_single_max_dp_work))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-max-search-nodes") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-max-search-nodes requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-single-max-search-nodes", argv[++i],
                         &branch_single_max_search_nodes))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-cache-budget-mib") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-cache-budget-mib requires a value\n", stderr);
        return 2;
      }
      if (!parse_u64_arg("--branch-single-cache-budget-mib", argv[++i],
                         &branch_single_cache_budget_mib))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-cache-min-vars") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-cache-min-vars requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--branch-single-cache-min-vars", argv[++i],
                         &branch_single_cache_min_vars))
        return 2;
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-kernel") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-kernel requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "auto") == 0) {
        branch_single_kernel = QSOP_BRANCH_SINGLE_KERNEL_AUTO;
      } else if (strcmp(value, "scalar") == 0) {
        branch_single_kernel = QSOP_BRANCH_SINGLE_KERNEL_SCALAR;
      } else {
        fprintf(stderr,
                "error: unsupported --branch-single-kernel '%s' "
                "(expected auto|scalar)\n",
                value);
        return 2;
      }
      branch_single_option_set = true;
      continue;
    }
    if (strcmp(argv[i], "--branch-single-precision") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --branch-single-precision requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "auto") == 0) {
        branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_AUTO;
      } else if (strcmp(value, "double") == 0) {
        branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_DOUBLE;
      } else if (strcmp(value, "long-double") == 0) {
        branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE;
      } else {
        fprintf(stderr,
                "error: unsupported --branch-single-precision '%s' "
                "(expected auto|double|long-double)\n",
                value);
        return 2;
      }
      branch_single_option_set = true;
      branch_single_precision_set = true;
      continue;
    }
    if (strcmp(argv[i], "--backend") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --backend requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "branch") == 0) {
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
    if (strcmp(argv[i], "--solve-mode") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --solve-mode requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "auto") == 0) {
        solve_mode_auto = true;
        solve_mode_set = true;
      } else if (strcmp(value, "count-table") == 0) {
        solve_mode = QSOP_SOLVE_MODE_COUNT_TABLE;
        solve_mode_auto = false;
        solve_mode_set = true;
      } else if (strcmp(value, "fourier") == 0) {
        solve_mode = QSOP_SOLVE_MODE_FOURIER;
        solve_mode_auto = false;
        solve_mode_set = true;
      } else if (strcmp(value, "single-fourier") == 0) {
        single_fourier_mode = true;
        solve_mode_auto = false;
        solve_mode_set = true;
      } else {
        fprintf(stderr, "error: unsupported solve mode '%s'\n", value);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--fourier-target-mode") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --fourier-target-mode requires a value\n", stderr);
        return 2;
      }
      if (!parse_u32_arg("--fourier-target-mode", argv[++i], &fourier_target_mode)) {
        return 2;
      }
      fourier_target_mode_set = true;
      continue;
    }
    if (strcmp(argv[i], "--single-mode-precision") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --single-mode-precision requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "auto") == 0) {
        single_mode_precision = SINGLE_MODE_PRECISION_AUTO;
      } else if (strcmp(value, "double") == 0) {
        single_mode_precision = SINGLE_MODE_PRECISION_DOUBLE;
      } else if (strcmp(value, "long-double") == 0) {
        single_mode_precision = SINGLE_MODE_PRECISION_LONG_DOUBLE;
      } else {
        fprintf(stderr,
                "error: unsupported --single-mode-precision '%s' "
                "(expected auto|double|long-double)\n",
                value);
        return 2;
      }
      single_mode_precision_set = true;
      continue;
    }
    if (strcmp(argv[i], "--print-kernels") == 0) {
      print_kernels = true;
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
  if (!solve_mode_set && backend == SOLVE_BACKEND_BRANCH && format != SOLVE_FORMAT_RESIDUE_VECTOR) {
    solve_mode_auto = true;
    solve_mode_set = true;
  }
  if (solve_mode_auto && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --solve-mode auto is currently supported only with --backend branch\n", stderr);
    return 2;
  }
  if (branch_heuristic_set && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --branch-heuristic requires --backend branch\n", stderr);
    return 2;
  }
  if (branch_rw_source_set && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --branch-rw-source requires --backend branch\n", stderr);
    return 2;
  }
  if (branch_single_option_set && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --branch-single-* options require --backend branch\n", stderr);
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
  if (branch_single_diagnose_conditioning && stats_jsonl_path == NULL) {
    fputs("error: --branch-single-diagnose-conditioning requires --stats-jsonl\n", stderr);
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
  if (backend != SOLVE_BACKEND_RANKWIDTH && rw_fourier_kernel_set) {
    fputs("error: --rankwidth-fourier-kernel requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (backend != SOLVE_BACKEND_RANKWIDTH && rw_single_kernel_set) {
    fputs("error: --rankwidth-single-kernel requires --backend rankwidth\n", stderr);
    return 2;
  }
  if (rankwidth_mode_set && solve_mode_set && !solve_mode_auto &&
      rankwidth_mode != rankwidth_mode_from_solve_mode(solve_mode)) {
    fputs("error: --solve-mode conflicts with --rankwidth-mode\n", stderr);
    return 2;
  }
  if (backend == SOLVE_BACKEND_RANKWIDTH && solve_mode_set && !solve_mode_auto) {
    rankwidth_mode = rankwidth_mode_from_solve_mode(solve_mode);
  }
  if (rankwidth_mode_set && !solve_mode_set) {
    solve_mode = rankwidth_mode == QSOP_RANKWIDTH_SOLVE_FOURIER ? QSOP_SOLVE_MODE_FOURIER
                                                                : QSOP_SOLVE_MODE_COUNT_TABLE;
    solve_mode_set = true;
  }
  if (rw_fourier_kernel_set && rankwidth_mode != QSOP_RANKWIDTH_SOLVE_FOURIER) {
    fputs("error: --rankwidth-fourier-kernel requires --rankwidth-mode fourier\n", stderr);
    return 2;
  }
  if (rw_single_kernel_set && !single_fourier_mode) {
    fputs("error: --rankwidth-single-kernel requires --solve-mode single-fourier\n", stderr);
    return 2;
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
  if (single_fourier_mode && backend != SOLVE_BACKEND_TREEWIDTH &&
      backend != SOLVE_BACKEND_RANKWIDTH && backend != SOLVE_BACKEND_BRANCH) {
    fputs("error: --solve-mode single-fourier requires --backend treewidth, rankwidth, or "
          "branch\n",
          stderr);
    return 2;
  }
  if (branch_single_option_set && !single_fourier_mode && !solve_mode_auto) {
    fputs("error: --branch-single-* options require --solve-mode single-fourier or auto\n", stderr);
    return 2;
  }
  if (single_fourier_mode && calibrate_backends) {
    fputs("error: --branch-calibrate-backends is not yet supported with "
          "--solve-mode single-fourier\n",
          stderr);
    return 2;
  }
  if (fourier_target_mode_set && !single_fourier_mode && format != SOLVE_FORMAT_AMPLITUDE) {
    fputs("error: --fourier-target-mode requires --format amplitude or --solve-mode "
          "single-fourier\n",
          stderr);
    return 2;
  }
  if (single_fourier_mode && format == SOLVE_FORMAT_RESIDUE_VECTOR) {
    fputs("error: --format residue-vector is incompatible with --solve-mode single-fourier\n",
          stderr);
    return 2;
  }
  if (single_fourier_mode && (include_result || include_probability)) {
    fputs("error: --solve-mode single-fourier is incompatible with --include-result/"
          "--include-probability (it already reports the amplitude directly)\n",
          stderr);
    return 2;
  }
  /* --max-vars's default (24) is a count-table safety valve, since nvars directly
   * drives exact residual search cost there. single-fourier mode has no such blowup -- table
   * size is O(2^bagwidth), independent of nvars -- so raise the default when it applies and the
   * caller hasn't overridden it. 4096 is an empirically-tested ceiling, not a guess: real
   * qasm2sop --approx gauntlet circuits up to ~2300 variables were confirmed to complete their
   * width/cutrank diagnostic in well under a minute; circuits in the several-thousand range
   * (e.g. ~6600 variables) did not complete within 60s, so this stays comfortably below that
   * cliff rather than trading "clean refusal" for "slow hang" on the largest real circuits.
   * Callers needing more can still pass --max-vars explicitly. */
  if (single_fourier_mode && !max_vars_set) {
    max_vars = 4096;
  }

  if (single_mode_precision_set && !single_fourier_mode && !solve_mode_auto && !print_kernels) {
    fputs("error: --single-mode-precision requires --solve-mode single-fourier or auto\n", stderr);
    return 2;
  }
  if (single_mode_precision_set && !branch_single_precision_set) {
    switch (single_mode_precision) {
    case SINGLE_MODE_PRECISION_AUTO:
      branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_AUTO;
      break;
    case SINGLE_MODE_PRECISION_DOUBLE:
      branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_DOUBLE;
      break;
    case SINGLE_MODE_PRECISION_LONG_DOUBLE:
      branch_single_precision = QSOP_BRANCH_SINGLE_PRECISION_LONG_DOUBLE;
      break;
    }
  }

  /* AUTO always resolves -- scalar is the floor -- but keep the guard so a future kernel that
   * fails its own runtime probe surfaces here instead of dereferencing NULL. */
  const qsop_simd_vtable_t *simd = qsop_simd_resolve(QSOP_SIMD_KERNEL_AUTO);
  if (simd == NULL) {
    fprintf(stderr, "error: no usable SIMD kernel; compiled kernels are '%s'\n",
            qsop_simd_compiled_arch());
    return 2;
  }

  if (print_kernels) {
    printf("simd_compiled=%s\n", qsop_simd_compiled_arch());
    printf("simd_kernel=%s\n", qsop_simd_kernel_name(simd));
    printf("bitset_popcount_kernel=%s\n", qsop_simd_kernel_name(simd));
    printf("single_mode_precision=%s\n", single_mode_precision_name(single_mode_precision));
    return 0;
  }

  if ((single_fourier_mode || solve_mode_auto) &&
      single_mode_precision == SINGLE_MODE_PRECISION_LONG_DOUBLE &&
      backend == SOLVE_BACKEND_BRANCH &&
      branch_single_fallback != QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY) {
    fputs("error: --single-mode-precision long-double is not implemented for branch residual "
          "fallback yet; use --branch-single-fourier-fallback delegate-only\n",
          stderr);
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
  bool result_ready = false;
  bool auto_fallback_single_fourier = false;
  bool auto_direct_treewidth_single = false;
  uint32_t *auto_treewidth_order = NULL;
  uint32_t auto_treewidth_order_width = 0;

  qsop_backend_stats_sink_t sink = {
      .file = jsonl_file,
      .instance = diagnostic_path,
      .next_id = 0,
      .calibrate_backends = calibrate_backends,
  };
  qsop_backend_stats_sink_t *sink_ptr = jsonl_file != NULL ? &sink : NULL;

  if (solve_mode_auto) {
    if (format == SOLVE_FORMAT_RESIDUE_VECTOR) {
      fputs("error: --format residue-vector is incompatible with --solve-mode auto; use "
            "--format amplitude or an explicit exact solve mode\n",
            stderr);
      qsop_free(qsop);
      if (jsonl_file != NULL) {
        fclose(jsonl_file);
      }
      return 2;
    }
    qsop_error_t count_error = {0};
    if (!calibrate_backends && !branch_single_option_set &&
        branch_auto_prepare_treewidth_single(qsop, max_vars, max_vars_set, branch_rw_source,
                                             &branch_policy, &auto_treewidth_order,
                                             &auto_treewidth_order_width)) {
      auto_fallback_single_fourier = true;
      auto_direct_treewidth_single = true;
    } else if (!calibrate_backends && branch_auto_count_vector_too_large(qsop)) {
      /* This is a memory preflight, not a delegation judgment.  Preserve the ordinary AUTO
       * residual fallback so opt-in cutset conditioning remains available on large-modulus hard
       * graphs.  The pre-existing width probe still selects delegate-only for instances that
       * AUTO would have routed directly to single-Fourier before the count attempt. */
      auto_fallback_single_fourier = true;
      if (!branch_single_fallback_set &&
          branch_auto_should_start_single_fourier(qsop, max_vars, max_vars_set)) {
        branch_single_fallback = QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY;
      }
    } else if (!calibrate_backends &&
               branch_auto_should_start_single_fourier(qsop, max_vars, max_vars_set)) {
      auto_fallback_single_fourier = true;
      if (!branch_single_fallback_set) {
        branch_single_fallback = QSOP_BRANCH_SINGLE_FALLBACK_DELEGATE_ONLY;
      }
    } else if (qsop->r <= UINT32_MAX) {
      ok = qsop_solve_branch(qsop, max_vars,
                             &(qsop_branch_solve_options_t){
                                 .heuristic = branch_heuristic,
                                 .rw_source = branch_rw_source,
                                 .mode = QSOP_SOLVE_MODE_COUNT_TABLE,
                                 .policy = branch_policy,
                                 .sink = sink_ptr,
                                 .trace = trace_ptr,
                             },
                             &result, &solve_stats, &count_error);
      if (ok) {
        result_ready = true;
        solve_mode = QSOP_SOLVE_MODE_COUNT_TABLE;
      } else if (!branch_auto_refusal_is_safe_fallback(&count_error)) {
        if (solve_stats.termination_reason == QSOP_SOLVE_TERMINATION_NONE) {
          solve_stats.termination_reason = QSOP_SOLVE_TERMINATION_OTHER_ERROR;
        }
        write_run_summary_jsonl(jsonl_file, sink.instance, false, &solve_stats, &count_error);
        if (jsonl_file != NULL) {
          fclose(jsonl_file);
        }
        print_error(&count_error, diagnostic_path);
        qsop_free(qsop);
        return 1;
      } else {
        auto_fallback_single_fourier = true;
        error = (qsop_error_t){0};
      }
    } else {
      auto_fallback_single_fourier = true;
    }
    if (auto_fallback_single_fourier) {
      single_fourier_mode = true;
      if (!max_vars_set) {
        max_vars = 4096;
      }
    }
  }

  if (single_fourier_mode && !result_ready) {
    qsop_amplitude_t amplitude = {0};
    qsop_solve_stats_t amp_stats = {0};
    if (backend == SOLVE_BACKEND_RANKWIDTH) {
      qsop_rankwidth_decomposition_t *single_mode_decomposition = NULL;
      if (rankwidth_decomposition_path != NULL) {
        FILE *decomposition_file = fopen(rankwidth_decomposition_path, "r");
        if (decomposition_file == NULL) {
          fprintf(stderr, "error: %s: %s\n", rankwidth_decomposition_path, strerror(errno));
          qsop_free(qsop);
          return 1;
        }
        ok = qsop_rankwidth_decomposition_parse_file(decomposition_file,
                                                     rankwidth_decomposition_path, qsop->nvars,
                                                     &single_mode_decomposition, &error);
        fclose(decomposition_file);
      } else {
        ok = qsop_rankwidth_decomposition_generate(qsop, rankwidth_generator,
                                                   &single_mode_decomposition, &error);
      }
      if (!ok) {
        print_error(&error, rankwidth_decomposition_path != NULL ? rankwidth_decomposition_path
                                                                 : diagnostic_path);
        qsop_free(qsop);
        return 1;
      }
      const qsop_rankwidth_single_mode_options_t rankwidth_single_options = {
          .kernel = rw_single_kernel,
          .materialize_join_max_pairs = rw_materialize_join_max_pairs,
          .simd = simd,
      };
      if (single_mode_precision != SINGLE_MODE_PRECISION_LONG_DOUBLE) {
        ok = qsop_solve_rankwidth_single_mode_f64_options(
            qsop, single_mode_decomposition, max_vars, fourier_target_mode,
            &rankwidth_single_options, &amplitude, &amp_stats, trace_ptr, &error);
      } else {
        ok = qsop_solve_rankwidth_single_mode_options(
            qsop, single_mode_decomposition, max_vars, fourier_target_mode,
            &rankwidth_single_options, &amplitude, &amp_stats, trace_ptr, &error);
      }
      qsop_rankwidth_decomposition_free(single_mode_decomposition);
    } else if (backend == SOLVE_BACKEND_BRANCH && auto_direct_treewidth_single) {
      /* AUTO takes the f64 tables. Long double used to be the safe default because the raw
       * amplitude overflowed a double past ~1024 variables; the DP now carries a binary exponent,
       * so double has all the range it needs -- and it is the only precision with SIMD kernels,
       * at half the table memory. */
      if (single_mode_precision != SINGLE_MODE_PRECISION_LONG_DOUBLE) {
        ok = qsop_solve_treewidth_precomputed_order_single_mode_f64(
            qsop, BRANCH_AUTO_SINGLE_FOURIER_MAX_WIDTH + 1U, auto_treewidth_order,
            auto_treewidth_order_width, fourier_target_mode, simd, &amplitude, &amp_stats,
            trace_ptr, &error);
      } else {
        ok = qsop_solve_treewidth_precomputed_order_single_mode(
            qsop, BRANCH_AUTO_SINGLE_FOURIER_MAX_WIDTH + 1U, auto_treewidth_order,
            auto_treewidth_order_width, fourier_target_mode, &amplitude, &amp_stats, trace_ptr,
            &error);
      }
      if (ok) {
        amp_stats.treewidth_delegations = 1;
      }
    } else if (backend == SOLVE_BACKEND_BRANCH) {
      const qsop_branch_single_mode_options_t branch_single_mode_options = {
          .rw_source = branch_rw_source,
          .policy = branch_policy,
          .heuristic = branch_heuristic,
          .fallback = branch_single_fallback,
          .precision = branch_single_precision,
          .kernel = branch_single_kernel,
          .propagate = branch_single_propagate,
          .simd = simd,
          .max_search_nodes = branch_single_max_search_nodes,
          .max_fallback_vars = branch_single_max_fallback_vars,
          .treewidth_delegate_max_dp_work = branch_single_max_dp_work,
          .cache_budget_mib = branch_single_cache_budget_mib,
          .cache_min_vars = branch_single_cache_min_vars,
          .materialized_reduction = branch_single_materialized_reduction,
          .diagnose_conditioning = branch_single_diagnose_conditioning,
          .max_cutset_depth = branch_single_cutset_depth,
          .lookahead_candidates = branch_single_lookahead_candidates,
          .max_conditioning_nodes = branch_single_max_conditioning_nodes,
          .delegate_reprobe_interval = branch_single_delegate_reprobe_interval,
          .max_stagnant_levels = branch_single_max_stagnant_levels,
          .sink = sink_ptr,
          .trace = trace_ptr,
      };
      ok = qsop_solve_branch_single_mode(qsop, max_vars, fourier_target_mode,
                                         &branch_single_mode_options, &amplitude, &amp_stats,
                                         &error);
    } else if (single_mode_precision != SINGLE_MODE_PRECISION_LONG_DOUBLE) {
      ok =
          qsop_solve_treewidth_single_mode_f64(qsop, max_vars, treewidth_order, fourier_target_mode,
                                               simd, &amplitude, &amp_stats, trace_ptr, &error);
    } else {
      ok = qsop_solve_treewidth_single_mode(qsop, max_vars, treewidth_order, fourier_target_mode,
                                            &amplitude, &amp_stats, trace_ptr, &error);
    }
    free(auto_treewidth_order);
    auto_treewidth_order = NULL;
    amp_stats.simd_kernel = (uint32_t)simd_kernel_from_vtable(simd);
    amp_stats.bitset_kernel = (uint32_t)simd_kernel_from_vtable(simd);
    if (single_mode_precision_set) {
      amp_stats.single_mode_precision = (uint32_t)single_mode_precision;
    }
    if (!ok && amp_stats.termination_reason == QSOP_SOLVE_TERMINATION_NONE) {
      amp_stats.termination_reason = QSOP_SOLVE_TERMINATION_OTHER_ERROR;
    }
    write_run_summary_jsonl(jsonl_file, sink.instance, ok, &amp_stats, &error);
    /* The amplitude is normalized against norm_h below, after the instance is gone. */
    const uint64_t amplitude_norm_h = qsop->norm_h;
    qsop_free(qsop);
    if (jsonl_file != NULL) {
      fclose(jsonl_file);
    }
    if (!ok) {
      print_error(&error, diagnostic_path);
      return 1;
    }
    if (format == SOLVE_FORMAT_STATS) {
      ok = write_amplitude_output(stdout, solve_mode_auto ? "auto" : "single-fourier",
                                  "single-fourier", fourier_target_mode, &amplitude,
                                  amplitude_norm_h, &error);
      if (ok) {
        ok = write_solver_stats(stdout, backend, &amp_stats, solve_mode, false, false,
                                rankwidth_mode, rankwidth_decomposition_label, treewidth_order,
                                &error);
      }
      if (!ok) {
        print_error(&error, "<stdout>");
        return 1;
      }
    } else {
      ok = write_amplitude_output(stdout, solve_mode_auto ? "auto" : "single-fourier",
                                  "single-fourier", fourier_target_mode, &amplitude,
                                  amplitude_norm_h, &error);
      if (!ok) {
        print_error(&error, "<stdout>");
        return 1;
      }
    }
    return 0;
  }

  if (!result_ready) {
    if (backend == SOLVE_BACKEND_BRANCH) {
      ok = qsop_solve_branch(qsop, max_vars,
                             &(qsop_branch_solve_options_t){
                                 .heuristic = branch_heuristic,
                                 .rw_source = branch_rw_source,
                                 .mode = solve_mode,
                                 .policy = branch_policy,
                                 .sink = sink_ptr,
                                 .trace = trace_ptr,
                             },
                             &result, &solve_stats, &error);
    } else if (backend == SOLVE_BACKEND_RANKWIDTH) {
      if (rankwidth_decomposition_path != NULL) {
        FILE *decomposition_file = fopen(rankwidth_decomposition_path, "r");
        if (decomposition_file == NULL) {
          fprintf(stderr, "error: %s: %s\n", rankwidth_decomposition_path, strerror(errno));
          qsop_free(qsop);
          return 1;
        }
        ok = qsop_rankwidth_decomposition_parse_file(decomposition_file,
                                                     rankwidth_decomposition_path, qsop->nvars,
                                                     &rankwidth_decomposition, &error);
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
      /* D1: memory budget gate — check forecast before allocating solve state. */
      if (ok && rw_memory_budget_bytes > 0) {
        uint64_t fe = 0;
        uint64_t jp = 0;
        qsop_error_t fe_err = {0};
        if (qsop_rankwidth_decomposition_forecast(qsop, rankwidth_decomposition, &fe, &jp,
                                                  &fe_err)) {
          const uint64_t forecast_bytes = fe * (uint64_t)qsop->r * sizeof(uint64_t);
          if (forecast_bytes > rw_memory_budget_bytes) {
            if (rw_memory_policy == RW_MEMORY_POLICY_HARD_ERROR) {
              fprintf(stderr,
                      "error: rankwidth memory forecast %" PRIu64 " MiB exceeds budget %" PRIu64
                      " MiB\n",
                      (uint64_t)(forecast_bytes / (1024ULL * 1024ULL)),
                      (uint64_t)(rw_memory_budget_bytes / (1024ULL * 1024ULL)));
              qsop_rankwidth_decomposition_free(rankwidth_decomposition);
              qsop_free(qsop);
              return 1;
            } else if (rw_memory_policy == RW_MEMORY_POLICY_SKIP) {
              if (format == SOLVE_FORMAT_STATS) {
                fprintf(stdout, "backend: %s\n", backend_name(backend));
                fprintf(stdout, "status: memory-skip\n");
                fprintf(stdout, "rankwidth_memory_forecast_bytes: %" PRIu64 "\n", forecast_bytes);
                fprintf(stdout, "rankwidth_memory_budget_bytes: %" PRIu64 "\n",
                        rw_memory_budget_bytes);
              } else {
                fprintf(stderr,
                        "rankwidth: memory-skip (forecast %" PRIu64 " MiB > budget %" PRIu64
                        " MiB)\n",
                        (uint64_t)(forecast_bytes / (1024ULL * 1024ULL)),
                        (uint64_t)(rw_memory_budget_bytes / (1024ULL * 1024ULL)));
              }
              qsop_rankwidth_decomposition_free(rankwidth_decomposition);
              qsop_free(qsop);
              return 0;
            } else {
              /* RW_MEMORY_POLICY_FALLBACK: fall through to treewidth below. */
              qsop_rankwidth_decomposition_free(rankwidth_decomposition);
              rankwidth_decomposition = NULL;
              ok = qsop_solve_treewidth_order_mode_trace_stats(qsop, max_vars, treewidth_order,
                                                               solve_mode, &result, &solve_stats,
                                                               trace_ptr, &error);
              goto rankwidth_done;
            }
          }
        }
      }
      if (ok) {
        ok = qsop_solve_rankwidth_options_mode_trace_stats(
            qsop, rankwidth_decomposition, max_vars, rankwidth_mode,
            &(qsop_rankwidth_solve_options_t){
                .join_strategy = rw_join_strategy,
                .materialize_join_max_pairs = rw_materialize_join_max_pairs,
                .fourier_kernel = rw_fourier_kernel,
            },
            &result, &solve_stats, trace_ptr, &error);
      }
    rankwidth_done:;
    } else {
      ok = qsop_solve_treewidth_order_mode_trace_stats(qsop, max_vars, treewidth_order, solve_mode,
                                                       &result, &solve_stats, trace_ptr, &error);
    }
  }

  if (!ok && solve_stats.termination_reason == QSOP_SOLVE_TERMINATION_NONE) {
    solve_stats.termination_reason = QSOP_SOLVE_TERMINATION_OTHER_ERROR;
  }
  write_run_summary_jsonl(jsonl_file, sink.instance, ok, &solve_stats, &error);
  qsop_rankwidth_decomposition_free(rankwidth_decomposition);
  qsop_free(qsop);
  if (jsonl_file != NULL) {
    fclose(jsonl_file);
  }
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  if (format == SOLVE_FORMAT_AMPLITUDE) {
    qsop_amplitude_t amplitude = {0};
    ok = result_to_amplitude(result, fourier_target_mode, &amplitude, &error);
    if (ok) {
      ok = write_amplitude_output(stdout, solve_mode_auto ? "auto" : solve_mode_name(solve_mode),
                                  solve_mode_kernel_name(backend, solve_mode), fourier_target_mode,
                                  &amplitude, result->norm_h, &error);
    }
  } else if (format == SOLVE_FORMAT_RESIDUE_VECTOR) {
    ok = qsop_result_write_residue_vector(stdout, result, &error);
  } else {
    ok = write_solver_stats(stdout, backend, &solve_stats, solve_mode, solve_mode_set,
                            solve_mode_auto, rankwidth_mode, rankwidth_decomposition_label,
                            treewidth_order, &error);
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
