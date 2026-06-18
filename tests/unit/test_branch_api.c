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

/* qsop_solve_residual_branch — the simplest public wrapper; chains through the
 * entire _stats / _trace_stats / _heuristic_* family. */
static int test_simple_wrapper(void) {
  qsop_instance_t inst = make_tiny();
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (!qsop_solve_residual_branch(&inst, 64, &res, &err)) {
    fprintf(stderr, "FAIL simple_wrapper: %s\n",
            err.message ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS simple_wrapper\n");
  return 0;
}

/* qsop_solve_residual_branch_stats — explicit stats variant. */
static int test_stats_wrapper(void) {
  qsop_instance_t   inst  = make_tiny();
  qsop_result_t    *res   = NULL;
  qsop_solve_stats_t stats = {0};
  qsop_error_t      err   = {0};
  if (!qsop_solve_residual_branch_stats(&inst, 64, &res, &stats, &err)) {
    fprintf(stderr, "FAIL stats_wrapper: %s\n",
            err.message ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS stats_wrapper\n");
  return 0;
}

/* qsop_solve_residual_branch_heuristic_rw_source_mode_trace_stats — the rw_source variant. */
static int test_rw_source_variant(void) {
  qsop_instance_t inst = make_tiny();
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (!qsop_solve_residual_branch_heuristic_rw_source_mode_trace_stats(
          &inst, 64, QSOP_BRANCH_HEURISTIC_SPLIT, QSOP_BRANCH_RW_SOURCE_NONE,
          QSOP_SOLVE_MODE_COUNT_TABLE, &res, NULL, NULL, &err)) {
    fprintf(stderr, "FAIL rw_source_variant: %s\n",
            err.message ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS rw_source_variant\n");
  return 0;
}

/* Error path: out == NULL must be rejected. */
static int test_null_out_rejected(void) {
  qsop_instance_t inst = make_tiny();
  qsop_error_t    err  = {0};
  if (qsop_solve_residual_branch(&inst, 64, NULL, &err)) {
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
  if (qsop_solve_residual_branch(&inst, 2, &res, &err)) {
    fprintf(stderr, "FAIL nvars_exceeds_max: expected false\n");
    return 1;
  }
  fprintf(stderr, "PASS nvars_exceeds_max\n");
  return 0;
}

/* Error path: rw_source variant with null out. */
static int test_rw_source_null_out(void) {
  qsop_instance_t inst = make_tiny();
  qsop_error_t    err  = {0};
  if (qsop_solve_residual_branch_heuristic_rw_source_mode_trace_stats(
          &inst, 64, QSOP_BRANCH_HEURISTIC_SPLIT, QSOP_BRANCH_RW_SOURCE_NONE,
          QSOP_SOLVE_MODE_COUNT_TABLE, NULL, NULL, NULL, &err)) {
    fprintf(stderr, "FAIL rw_source_null_out: expected false\n");
    return 1;
  }
  fprintf(stderr, "PASS rw_source_null_out\n");
  return 0;
}

int main(void) {
  int failures = 0;
  failures += test_simple_wrapper();
  failures += test_stats_wrapper();
  failures += test_rw_source_variant();
  failures += test_null_out_rejected();
  failures += test_nvars_exceeds_max();
  failures += test_rw_source_null_out();
  if (failures > 0) {
    fprintf(stderr, "\n%d test(s) FAILED\n", failures);
    return 1;
  }
  fprintf(stderr, "\n6 test(s) passed\n");
  return 0;
}
