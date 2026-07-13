#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* A tiny 3-variable QSOP instance.  nvars < BRANCH_TREEWIDTH_DELEGATE_MIN_VARS (16),
 * so branch_try_root_treewidth_fast_path returns immediately with root_handled=false,
 * which lets the main branch_solve_counts_once path run on every test call. */
static qsop_instance_t make_tiny(void) {
  static uint64_t unary[]  = {1, 2, 5};
  static uint32_t edge_u[] = {0, 1};
  static uint32_t edge_v[] = {1, 2};
  return (qsop_instance_t){
      .r        = 8,
      .nvars    = 3,
      .norm_h   = 4,
      .constant = 7,
      .unary    = unary,
      .nedges   = 2,
      .edge_u   = edge_u,
      .edge_v   = edge_v,
  };
}

static qsop_instance_t make_k10_10(uint64_t unary[20], uint32_t edge_u[100],
                                   uint32_t edge_v[100]) {
  for (uint32_t i = 0; i < 20; ++i) {
    unary[i] = 0;
  }
  uint32_t edge = 0;
  for (uint32_t u = 0; u < 10; ++u) {
    for (uint32_t v = 10; v < 20; ++v) {
      edge_u[edge] = u;
      edge_v[edge] = v;
      ++edge;
    }
  }
  return (qsop_instance_t){
      .r        = 8,
      .nvars    = 20,
      .norm_h   = 0,
      .constant = 0,
      .unary    = unary,
      .nedges   = 100,
      .edge_u   = edge_u,
      .edge_v   = edge_v,
  };
}

/* qsop_solve_branch with NULL options — zero-init defaults (split, auto, count-table). */
static int test_null_options(void) {
  qsop_instance_t inst = make_tiny();
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (!qsop_solve_branch(&inst, 64, NULL, &res, NULL, &err)) {
    fprintf(stderr, "FAIL null_options: %s\n",
            err.message[0] ? err.message : "(no message)");
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
            err.message[0] ? err.message : "(no message)");
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
            err.message[0] ? err.message : "(no message)");
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS rw_source_none\n");
  return 0;
}

/* rw_source=none on a low-cutrank K10,10 must skip rankwidth and delegate to treewidth. */
static int test_rw_source_none_treewidth_delegate(void) {
  uint64_t unary[20];
  uint32_t edge_u[100];
  uint32_t edge_v[100];
  qsop_instance_t    inst  = make_k10_10(unary, edge_u, edge_v);
  qsop_result_t     *res   = NULL;
  qsop_solve_stats_t stats = {0};
  qsop_error_t       err   = {0};
  if (!qsop_solve_branch(&inst, 64,
                         &(qsop_branch_solve_options_t){
                             .heuristic = QSOP_BRANCH_HEURISTIC_SPLIT,
                             .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
                             .mode      = QSOP_SOLVE_MODE_COUNT_TABLE,
                         },
                         &res, &stats, &err)) {
    fprintf(stderr, "FAIL rw_source_none_treewidth_delegate: %s\n",
            err.message[0] ? err.message : "(no message)");
    return 1;
  }
  if (stats.treewidth_delegations != 1 || stats.rankwidth_delegations != 0 ||
      stats.branch_fallthroughs != 0 || stats.max_residual_vars != 20) {
    fprintf(stderr,
            "FAIL rw_source_none_treewidth_delegate: tw=%" PRIu64
            " rw=%" PRIu64 " fallthrough=%" PRIu64 " max_vars=%" PRIu32 "\n",
            stats.treewidth_delegations, stats.rankwidth_delegations,
            stats.branch_fallthroughs, stats.max_residual_vars);
    qsop_result_free(res);
    return 1;
  }
  qsop_result_free(res);
  fprintf(stderr, "PASS rw_source_none_treewidth_delegate\n");
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

/* Error path: qsop == NULL must be rejected after the result pointer is validated. */
static int test_null_qsop_rejected(void) {
  qsop_result_t *res = NULL;
  qsop_error_t   err = {0};
  if (qsop_solve_branch(NULL, 64, NULL, &res, NULL, &err)) {
    fprintf(stderr, "FAIL null_qsop_rejected: expected false\n");
    qsop_result_free(res);
    return 1;
  }
  fprintf(stderr, "PASS null_qsop_rejected\n");
  return 0;
}

/* Error path: unsupported residual solve modes must be rejected before search starts. */
static int test_unsupported_mode_rejected(void) {
  qsop_instance_t inst = make_tiny();
  qsop_result_t  *res  = NULL;
  qsop_error_t    err  = {0};
  if (qsop_solve_branch(&inst, 64,
                        &(qsop_branch_solve_options_t){
                            .mode = (qsop_solve_mode_t)99,
                        },
                        &res, NULL, &err)) {
    fprintf(stderr, "FAIL unsupported_mode_rejected: expected false\n");
    qsop_result_free(res);
    return 1;
  }
  fprintf(stderr, "PASS unsupported_mode_rejected\n");
  return 0;
}

/* The single-Fourier treewidth delegate is admitted by min-fill DP work, not raw width. K10,10
 * (treewidth 10, tiny DP work) delegates to treewidth under a generous budget, but must fall back
 * to branching once the DP-work budget is set below its cost -- even though width 10 is far under
 * the ceiling. The amplitude must be identical across the two paths: the gate changes the route,
 * not the answer. */
static int solve_single_k10(uint64_t dp_work_budget, qsop_amplitude_t *amp,
                            qsop_solve_stats_t *stats) {
  uint64_t unary[20];
  uint32_t edge_u[100];
  uint32_t edge_v[100];
  qsop_instance_t inst = make_k10_10(unary, edge_u, edge_v);
  qsop_error_t err = {0};
  return qsop_solve_branch_single_mode(
      &inst, 64U, 1U,
      &(qsop_branch_single_mode_options_t){
          .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
          .max_fallback_vars = 64,
          .rankwidth_delegate_max_width = 1, /* rw_source defaults to none; keep rankwidth out */
          .treewidth_delegate_max_dp_work = dp_work_budget,
      },
      amp, stats, &err);
}

static int test_single_mode_dp_work_gate(void) {
  qsop_amplitude_t   amp_wide = {0};
  qsop_solve_stats_t wide     = {0};
  if (!solve_single_k10(UINT64_MAX, &amp_wide, &wide)) {
    fprintf(stderr, "FAIL single_mode_dp_work_gate: generous-budget solve failed\n");
    return 1;
  }
  if (wide.treewidth_delegations != 1 || wide.branch_fallthroughs != 0) {
    fprintf(stderr,
            "FAIL single_mode_dp_work_gate: expected treewidth delegate under a generous budget, "
            "got tw=%" PRIu64 " fallthrough=%" PRIu64 "\n",
            wide.treewidth_delegations, wide.branch_fallthroughs);
    return 1;
  }

  qsop_amplitude_t   amp_tight = {0};
  qsop_solve_stats_t tight     = {0};
  if (!solve_single_k10(1U, &amp_tight, &tight)) {
    fprintf(stderr, "FAIL single_mode_dp_work_gate: tight-budget solve failed\n");
    return 1;
  }
  if (tight.treewidth_delegations != 0 || tight.branch_fallthroughs == 0) {
    fprintf(stderr,
            "FAIL single_mode_dp_work_gate: expected branch fallback once DP work exceeds the "
            "budget, got tw=%" PRIu64 " fallthrough=%" PRIu64 "\n",
            tight.treewidth_delegations, tight.branch_fallthroughs);
    return 1;
  }

  long double re_wide = 0, im_wide = 0, re_tight = 0, im_tight = 0;
  if (!qsop_amplitude_normalized(&amp_wide, 0, &re_wide, &im_wide) ||
      !qsop_amplitude_normalized(&amp_tight, 0, &re_tight, &im_tight) ||
      fabsl(re_wide - re_tight) > 1e-9L || fabsl(im_wide - im_tight) > 1e-9L) {
    fprintf(stderr, "FAIL single_mode_dp_work_gate: amplitude differs across gate paths\n");
    return 1;
  }
  fprintf(stderr, "PASS single_mode_dp_work_gate\n");
  return 0;
}

static int expect_treewidth_preference(const char *name, uint32_t prefix_cut_rank, uint32_t nvars,
                                       uint64_t dp_work, bool expected) {
  const bool actual = qsop_branch_single_treewidth_clearly_preferred(
      prefix_cut_rank, nvars, dp_work, NULL);
  if (actual != expected) {
    fprintf(stderr,
            "FAIL policy_%s: prefix=%" PRIu32 " nvars=%" PRIu32 " dp_work=%" PRIu64
            " expected treewidth_preferred=%d, got %d\n",
            name, prefix_cut_rank, nvars, dp_work, expected, actual);
    return 1;
  }
  fprintf(stderr, "PASS policy_%s\n", name);
  return 0;
}

/* Guard the pre-probe model with shapes measured from the regression corpora. Prefix cut-rank is
 * intentionally only a zero/nonzero signal here: the InferQ graph's natural order has rank 21,
 * but the generated decomposition has width 10. Conversely, the two huge Shor graphs remain cheap
 * enough for treewidth that the O(n^2 * words) probe cost excludes rankwidth even at best-case
 * generated width one. The complete-bipartite win and cheap treewidth cases pin the old boundaries. */
static int test_rankwidth_preprobe_policy(void) {
  int failures = 0;
  failures += expect_treewidth_preference("inferq_472592", 21U, 123U, UINT64_C(4206906), false);
  failures +=
      expect_treewidth_preference("shor_15_4", 11U, 23768U, UINT64_C(53768770), true);
  failures +=
      expect_treewidth_preference("shor_9_4", 11U, 25992U, UINT64_C(99438302), true);
  failures += expect_treewidth_preference("k12_12", 1U, 24U, UINT64_C(106494), false);
  failures += expect_treewidth_preference("path32", 1U, 32U, UINT64_C(126), true);
  failures += expect_treewidth_preference("grid6", 6U, 36U, UINT64_C(1374), true);
  return failures;
}

static qsop_instance_t *make_complete(uint32_t n) {
  qsop_instance_t *q = calloc(1, sizeof(*q));
  const uint32_t m = n * (n - 1U) / 2U;
  if (q == NULL) {
    return NULL;
  }
  q->r = 8U;
  q->nvars = n;
  q->norm_h = 2U * n;
  q->constant = 1U;
  q->nedges = m;
  q->unary = calloc(n, sizeof(*q->unary));
  q->edge_u = malloc((m == 0U ? 1U : m) * sizeof(*q->edge_u));
  q->edge_v = malloc((m == 0U ? 1U : m) * sizeof(*q->edge_v));
  if (q->unary == NULL || q->edge_u == NULL || q->edge_v == NULL) {
    qsop_free(q);
    return NULL;
  }
  uint32_t e = 0;
  for (uint32_t u = 0; u < n; u++) {
    for (uint32_t v = u + 1U; v < n; v++) {
      q->edge_u[e] = u;
      q->edge_v[e] = v;
      e++;
    }
  }
  return q;
}

static bool solve_complete_single(const qsop_instance_t *q,
                                  const qsop_branch_single_mode_options_t *options,
                                  qsop_amplitude_t *amp, qsop_solve_stats_t *stats,
                                  qsop_error_t *error) {
  return qsop_solve_branch_single_mode(q, 64U, 1U, options, amp, stats, error);
}

static int test_materialized_cutset(void) {
  int failures = 0;
  qsop_instance_t *k4 = make_complete(4U);
  if (k4 == NULL) {
    return 1;
  }
  const qsop_branch_single_mode_options_t exhaustive_options = {
      .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
      .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
      .max_fallback_vars = 64U,
      .treewidth_delegate_max_dp_work = 1U,
  };
  qsop_amplitude_t want = {0};
  qsop_solve_stats_t want_stats = {0};
  qsop_error_t error = {0};
  if (!solve_complete_single(k4, &exhaustive_options, &want, &want_stats, &error)) {
    fprintf(stderr, "FAIL materialized_cutset reference: %s\n", error.message);
    qsop_free(k4);
    return 1;
  }

  const qsop_branch_single_mode_options_t cutset_options = {
      .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
      .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
      .max_fallback_vars = 2U,
      .treewidth_delegate_max_dp_work = 1U,
      .materialized_reduction = true,
      .max_cutset_depth = 1U,
      .lookahead_candidates = 1U,
  };
  qsop_amplitude_t got = {0};
  qsop_solve_stats_t got_stats = {0};
  error = (qsop_error_t){0};
  if (!solve_complete_single(k4, &cutset_options, &got, &got_stats, &error)) {
    fprintf(stderr, "FAIL materialized_cutset solve: %s\n", error.message);
    failures++;
  } else {
    const long double want_re = ldexpl(want.re, want.scale_exp2);
    const long double want_im = ldexpl(want.im, want.scale_exp2);
    const long double got_re = ldexpl(got.re, got.scale_exp2);
    const long double got_im = ldexpl(got.im, got.scale_exp2);
    if (fabsl(want_re - got_re) > 1e-9L || fabsl(want_im - got_im) > 1e-9L ||
        got_stats.branch_conditioning_nodes != 1U ||
        got_stats.branch_conditioning_lookaheads != 2U ||
        got_stats.branch_materialized_degree2_merges == 0U ||
        got_stats.branch_max_cutset_depth != 1U) {
      fprintf(stderr,
              "FAIL materialized_cutset result/counters: nodes=%" PRIu64
              " lookaheads=%" PRIu64 " merges=%" PRIu64 " depth=%" PRIu32 "\n",
              got_stats.branch_conditioning_nodes, got_stats.branch_conditioning_lookaheads,
              got_stats.branch_materialized_degree2_merges,
              got_stats.branch_max_cutset_depth);
      failures++;
    }
  }

  qsop_amplitude_t refused_amp = {0};
  qsop_solve_stats_t refused_stats = {0};
  error = (qsop_error_t){0};
  const qsop_branch_single_mode_options_t depth0 = {
      .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
      .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
      .max_fallback_vars = 2U,
      .treewidth_delegate_max_dp_work = 1U,
  };
  if (solve_complete_single(k4, &depth0, &refused_amp, &refused_stats, &error) ||
      refused_stats.termination_reason != QSOP_SOLVE_TERMINATION_MAX_FALLBACK_VARS) {
    fprintf(stderr, "FAIL materialized_cutset depth0 refusal\n");
    failures++;
  }
  qsop_free(k4);

  qsop_instance_t *k5 = make_complete(5U);
  if (k5 == NULL) {
    return failures + 1;
  }
  const qsop_branch_single_mode_options_t node_limited = {
      .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
      .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
      .max_fallback_vars = 2U,
      .treewidth_delegate_max_dp_work = 1U,
      .materialized_reduction = true,
      .max_cutset_depth = 3U,
      .lookahead_candidates = 1U,
      .max_conditioning_nodes = 1U,
  };
  refused_stats = (qsop_solve_stats_t){0};
  error = (qsop_error_t){0};
  if (solve_complete_single(k5, &node_limited, &refused_amp, &refused_stats, &error) ||
      refused_stats.termination_reason != QSOP_SOLVE_TERMINATION_CUTSET_BUDGET ||
      refused_stats.branch_conditioning_nodes != 1U) {
    fprintf(stderr, "FAIL materialized_cutset node budget\n");
    failures++;
  }
  qsop_free(k5);
  if (failures == 0) {
    fprintf(stderr, "PASS materialized_cutset\n");
  }
  return failures;
}

static uint64_t cutset_xorshift(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static int test_cutset_differential(void) {
  uint64_t seed = UINT64_C(0x91e10da5c79e7b1d);
  for (uint32_t trial = 0; trial < 100U; trial++) {
    const uint32_t n = 3U + (uint32_t)(cutset_xorshift(&seed) % 4U);
    qsop_instance_t *q = make_complete(n);
    if (q == NULL) {
      return 1;
    }
    q->constant = cutset_xorshift(&seed) % q->r;
    for (uint32_t v = 0; v < n; v++) {
      q->unary[v] = cutset_xorshift(&seed) % q->r;
    }
    const qsop_branch_single_mode_options_t reference_options = {
        .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
        .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
        .max_fallback_vars = 64U,
        .treewidth_delegate_max_dp_work = 1U,
    };
    const qsop_branch_single_mode_options_t cutset_options = {
        .rw_source = QSOP_BRANCH_RW_SOURCE_NONE,
        .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
        .max_fallback_vars = 2U,
        .treewidth_delegate_max_dp_work = 1U,
        .materialized_reduction = true,
        .max_cutset_depth = 8U,
        .lookahead_candidates = 3U,
        .max_conditioning_nodes = 4096U,
        .max_stagnant_levels = 8U,
    };
    qsop_amplitude_t reference = {0}, cutset = {0};
    qsop_solve_stats_t reference_stats = {0}, cutset_stats = {0};
    qsop_error_t error = {0};
    if (!solve_complete_single(q, &reference_options, &reference, &reference_stats, &error) ||
        !solve_complete_single(q, &cutset_options, &cutset, &cutset_stats, &error)) {
      fprintf(stderr, "FAIL cutset differential trial %" PRIu32 ": %s\n", trial, error.message);
      qsop_free(q);
      return 1;
    }
    const long double rr = ldexpl(reference.re, reference.scale_exp2);
    const long double ri = ldexpl(reference.im, reference.scale_exp2);
    const long double cr = ldexpl(cutset.re, cutset.scale_exp2);
    const long double ci = ldexpl(cutset.im, cutset.scale_exp2);
    if (fabsl(rr - cr) > 1e-8L || fabsl(ri - ci) > 1e-8L) {
      fprintf(stderr,
              "FAIL cutset differential trial %" PRIu32 ": (%Lg,%Lg) != (%Lg,%Lg)\n",
              trial, rr, ri, cr, ci);
      qsop_free(q);
      return 1;
    }
    qsop_free(q);
  }
  fprintf(stderr, "PASS cutset_differential\n");
  return 0;
}

int main(void) {
  int failures = 0;
  failures += test_null_options();
  failures += test_with_stats();
  failures += test_rw_source_none();
  failures += test_rw_source_none_treewidth_delegate();
  failures += test_null_out_rejected();
  failures += test_nvars_exceeds_max();
  failures += test_null_out_with_options();
  failures += test_null_qsop_rejected();
  failures += test_unsupported_mode_rejected();
  failures += test_single_mode_dp_work_gate();
  failures += test_rankwidth_preprobe_policy();
  failures += test_materialized_cutset();
  failures += test_cutset_differential();
  if (failures > 0) {
    fprintf(stderr, "\n%d test(s) FAILED\n", failures);
    return 1;
  }
  fprintf(stderr, "\n16 test(s) passed\n");
  return 0;
}
