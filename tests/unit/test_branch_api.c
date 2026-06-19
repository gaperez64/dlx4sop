#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <stdio.h>
#include <stdlib.h>

/* A tiny 3-variable QSOP instance.  nvars < BRANCH_TREEWIDTH_DELEGATE_MIN_VARS (16),
 * so branch_try_root_treewidth_fast_path returns immediately with root_handled=false,
 * which lets the main branch_solve_counts_once path run on every test call. */
static qsop_instance_t make_tiny(void) {
  static uint32_t unary[]  = {1, 2, 5};
  static uint32_t edge_u[] = {0, 1};
  static uint32_t edge_v[] = {1, 2};
  static uint32_t edge_q[] = {4, 3};
  return (qsop_instance_t){
      .r        = 8,
      .nvars    = 3,
      .norm_h   = 4,
      .constant = 7,
      .mode     = QSOP_MODE_LABELLED,
      .unary    = unary,
      .nedges   = 2,
      .edge_u   = edge_u,
      .edge_v   = edge_v,
      .edge_q   = edge_q,
  };
}

/* qsop_solve_branch with NULL options — zero-init defaults (split, native, count-table). */
static int test_null_options(void) {
  qsop_instance_t inst = make_tiny();
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (!qsop_solve_branch(&inst, 64, NULL, &res, NULL, &err)) {
    fprintf(stderr, "FAIL null_options: %s\n",
            err.message ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS null_options\n");
  return 0;
}

/* qsop_solve_branch with explicit stats. */
static int test_with_stats(void) {
  qsop_instance_t    inst  = make_tiny();
  qsop_result_t     *res   = NULL;
  qsop_solve_stats_t stats = {0};
  qsop_error_t       err   = {0};
  if (!qsop_solve_branch(&inst, 64, NULL, &res, &stats, &err)) {
    fprintf(stderr, "FAIL with_stats: %s\n",
            err.message ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS with_stats\n");
  return 0;
}

/* qsop_solve_branch with rw_source=none (no rankwidth delegation). */
static int test_rw_source_none(void) {
  qsop_instance_t inst = make_tiny();
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (!qsop_solve_branch(&inst, 64,
                         &(qsop_branch_solve_options_t){
                             .heuristic = QSOP_BRANCH_HEURISTIC_SPLIT,
                             .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
                             .mode      = QSOP_SOLVE_MODE_COUNT_TABLE,
                         },
                         &res, NULL, &err)) {
    fprintf(stderr, "FAIL rw_source_none: %s\n",
            err.message ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS rw_source_none\n");
  return 0;
}

/* Error path: out == NULL must be rejected. */
static int test_null_out_rejected(void) {
  qsop_instance_t inst = make_tiny();
  qsop_error_t    err  = {0};
  if (qsop_solve_branch(&inst, 64, NULL, NULL, NULL, &err)) {
    fprintf(stderr, "FAIL null_out_rejected: expected false\n");
    return 1;
  }
  fprintf(stderr, "PASS null_out_rejected\n");
  return 0;
}

/* Error path: nvars > max_vars must be rejected. */
static int test_nvars_exceeds_max(void) {
  qsop_instance_t inst = make_tiny(); /* nvars = 3 */
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (qsop_solve_branch(&inst, 2, NULL, &res, NULL, &err)) {
    fprintf(stderr, "FAIL nvars_exceeds_max: expected false\n");
    return 1;
  }
  fprintf(stderr, "PASS nvars_exceeds_max\n");
  return 0;
}

/* Error path: null out with explicit options. */
static int test_null_out_with_options(void) {
  qsop_instance_t inst = make_tiny();
  qsop_error_t    err  = {0};
  if (qsop_solve_branch(&inst, 64,
                        &(qsop_branch_solve_options_t){
                            .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
                        },
                        NULL, NULL, &err)) {
    fprintf(stderr, "FAIL null_out_with_options: expected false\n");
    return 1;
  }
  fprintf(stderr, "PASS null_out_with_options\n");
  return 0;
}

int main(void) {
  int failures = 0;
  failures += test_null_options();
  failures += test_with_stats();
  failures += test_rw_source_none();
  failures += test_null_out_rejected();
  failures += test_nvars_exceeds_max();
  failures += test_null_out_with_options();
  if (failures > 0) {
    fprintf(stderr, "\n%d test(s) FAILED\n", failures);
    return 1;
  }
  fprintf(stderr, "\n6 test(s) passed\n");
  return 0;
}
