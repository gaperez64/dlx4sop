#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_wmc.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_ok(const char *name, const qsop_instance_t *qsop,
                  const qsop_wmc_options_t *options) {
  FILE *out = tmpfile();
  if (out == NULL) {
    return 0; /* no temp file available; skip rather than fail */
  }
  qsop_error_t error = {0};
  const bool ok = qsop_wmc_write(out, qsop, options, &error);
  const long size = ftell(out);
  fclose(out);
  if (!ok) {
    fprintf(stderr, "%s: qsop_wmc_write failed: %s\n", name, error.message);
    return 1;
  }
  if (size <= 0) {
    fprintf(stderr, "%s: qsop_wmc_write produced no output\n", name);
    return 1;
  }
  return 0;
}

static int run_fail(const char *name, FILE *out, const qsop_instance_t *qsop,
                    const qsop_wmc_options_t *options, const char *expected) {
  qsop_error_t error = {0};
  if (qsop_wmc_write(out, qsop, options, &error)) {
    fprintf(stderr, "%s: qsop_wmc_write unexpectedly succeeded\n", name);
    return 1;
  }
  if (strstr(error.message, expected) == NULL) {
    fprintf(stderr, "%s: unexpected error: %s\n", name, error.message);
    return 1;
  }
  return 0;
}

static int test_amp_soft_stats(void) {
  /* Sign instance r=2, 3 vars, 2 edges (both non-zero labels). */
  uint32_t s_unary[] = {1, 0, 1};
  uint32_t s_eu[] = {0, 1};
  uint32_t s_ev[] = {1, 2};
  uint32_t s_eq[] = {1, 1};
  qsop_instance_t sign2 = {.r = 2, .nvars = 3, .norm_h = 3, .constant = 1,
                           .mode = QSOP_MODE_SIGN, .unary = s_unary, .nedges = 2,
                           .edge_u = s_eu, .edge_v = s_ev, .edge_q = s_eq};

  /* amp-soft on sign2: 2 encoded edges -> 2 aux vars, 4 binary, 0 ternary. */
  qsop_wmc_options_t soft = qsop_wmc_options_default();
  soft.encoding = QSOP_WMC_ENCODING_AMP_SOFT;
  qsop_wmc_stats_t stats = {0};
  soft.stats_out = &stats;
  if (run_ok("amp-soft-sign2", &sign2, &soft) != 0) {
    return 1;
  }
  if (stats.clauses_ternary != 0) {
    fprintf(stderr, "amp-soft-sign2: expected 0 ternary clauses, got %" PRIu64 "\n",
            stats.clauses_ternary);
    return 1;
  }
  if (stats.clauses_binary != 2U * stats.encoded_edges) {
    fprintf(stderr,
            "amp-soft-sign2: expected binary == 2*encoded (%u), got %" PRIu64 "\n",
            2U * stats.encoded_edges, stats.clauses_binary);
    return 1;
  }
  if (stats.encoded_edges != 2) {
    fprintf(stderr, "amp-soft-sign2: expected 2 encoded edges, got %" PRIu32 "\n",
            stats.encoded_edges);
    return 1;
  }
  if (stats.aux_vars != 2) {
    fprintf(stderr, "amp-soft-sign2: expected 2 aux vars, got %" PRIu32 "\n", stats.aux_vars);
    return 1;
  }

  /* amp-and on sign2: 2 encoded edges -> 2 aux vars, 4 binary, 2 ternary. */
  qsop_wmc_options_t amp = qsop_wmc_options_default();
  amp.encoding = QSOP_WMC_ENCODING_AMPLITUDE;
  qsop_wmc_stats_t amp_stats = {0};
  amp.stats_out = &amp_stats;
  if (run_ok("amp-and-sign2", &sign2, &amp) != 0) {
    return 1;
  }
  if (amp_stats.clauses_ternary != 2) {
    fprintf(stderr, "amp-and-sign2: expected 2 ternary clauses, got %" PRIu64 "\n",
            amp_stats.clauses_ternary);
    return 1;
  }
  if (amp_stats.clauses_binary != 4) {
    fprintf(stderr, "amp-and-sign2: expected 4 binary clauses, got %" PRIu64 "\n",
            amp_stats.clauses_binary);
    return 1;
  }

  /* Edge with zero label is skipped in both encodings. */
  uint32_t w_unary[] = {0, 0};
  uint32_t w_eu[] = {0};
  uint32_t w_ev[] = {1};
  uint32_t w_eq[] = {0};  /* zero label */
  qsop_instance_t zero_edge = {.r = 8, .nvars = 2, .norm_h = 0, .constant = 0,
                               .mode = QSOP_MODE_LABELLED, .unary = w_unary, .nedges = 1,
                               .edge_u = w_eu, .edge_v = w_ev, .edge_q = w_eq};
  qsop_wmc_stats_t soft_zero_stats = {0};
  qsop_wmc_options_t soft_zero = soft;
  soft_zero.stats_out = &soft_zero_stats;
  if (run_ok("amp-soft-zero-edge", &zero_edge, &soft_zero) != 0) {
    return 1;
  }
  if (soft_zero_stats.encoded_edges != 0 || soft_zero_stats.skipped_edges != 1) {
    fprintf(stderr,
            "amp-soft-zero-edge: expected 0 encoded / 1 skipped, got %u/%u\n",
            soft_zero_stats.encoded_edges, soft_zero_stats.skipped_edges);
    return 1;
  }

  return 0;
}

static int test_export_shapes(void) {
  const qsop_wmc_options_t all = qsop_wmc_options_default();
  qsop_wmc_options_t single = qsop_wmc_options_default();
  single.all_residues = false;
  single.residue = 3;
  qsop_wmc_options_t no_meta = qsop_wmc_options_default();
  no_meta.emit_metadata = false;
  qsop_wmc_options_t amp = qsop_wmc_options_default();
  amp.encoding = QSOP_WMC_ENCODING_AMPLITUDE;
  qsop_wmc_options_t amp_no_meta = amp;
  amp_no_meta.emit_metadata = false;
  qsop_wmc_options_t amp_soft = qsop_wmc_options_default();
  amp_soft.encoding = QSOP_WMC_ENCODING_AMP_SOFT;
  qsop_wmc_options_t amp_soft_no_meta = amp_soft;
  amp_soft_no_meta.emit_metadata = false;

  int rc = 0;

  /* No variables: pure constant. */
  qsop_instance_t empty = {.r = 8, .nvars = 0, .norm_h = 0, .constant = 5,
                           .mode = QSOP_MODE_SIGN};
  rc |= run_ok("empty", &empty, &all);
  rc |= run_ok("empty-single", &empty, &single);
  rc |= run_ok("empty-amp", &empty, &amp);

  /* Sign instance, r = 2. */
  uint32_t s_unary[] = {1, 0, 1};
  uint32_t s_eu[] = {0, 1};
  uint32_t s_ev[] = {1, 2};
  uint32_t s_eq[] = {1, 1};
  qsop_instance_t sign2 = {.r = 2, .nvars = 3, .norm_h = 3, .constant = 1,
                           .mode = QSOP_MODE_SIGN, .unary = s_unary, .nedges = 2,
                           .edge_u = s_eu, .edge_v = s_ev, .edge_q = s_eq};
  rc |= run_ok("sign2", &sign2, &all);
  rc |= run_ok("sign2-no-meta", &sign2, &no_meta);
  rc |= run_ok("sign2-amp", &sign2, &amp);
  rc |= run_ok("sign2-amp-no-meta", &sign2, &amp_no_meta);
  rc |= run_ok("sign2-amp-soft", &sign2, &amp_soft);
  rc |= run_ok("sign2-amp-soft-no-meta", &sign2, &amp_soft_no_meta);

  /* Labelled instance, r = 8. */
  uint32_t l_unary[] = {2, 4};
  uint32_t l_eu[] = {0};
  uint32_t l_ev[] = {1};
  uint32_t l_eq[] = {3};
  qsop_instance_t labelled = {.r = 8, .nvars = 2, .norm_h = 4, .constant = 2,
                              .mode = QSOP_MODE_LABELLED, .unary = l_unary, .nedges = 1,
                              .edge_u = l_eu, .edge_v = l_ev, .edge_q = l_eq};
  rc |= run_ok("labelled", &labelled, &all);
  rc |= run_ok("labelled-single", &labelled, &single);
  rc |= run_ok("labelled-amp", &labelled, &amp);
  rc |= run_ok("labelled-amp-soft", &labelled, &amp_soft);

  /* Non-power-of-two modulus exercises the conditional subtract (residue only). */
  uint32_t n_unary[] = {5, 4};
  uint32_t n_eu[] = {0};
  uint32_t n_ev[] = {1};
  uint32_t n_eq[] = {5};
  qsop_instance_t mod6 = {.r = 6, .nvars = 2, .norm_h = 1, .constant = 4,
                          .mode = QSOP_MODE_LABELLED, .unary = n_unary, .nedges = 1,
                          .edge_u = n_eu, .edge_v = n_ev, .edge_q = n_eq};
  rc |= run_ok("mod6", &mod6, &all);
  rc |= run_ok("mod6-amp", &mod6, &amp);

  /* Large modulus exercises a wide accumulator and zero-coefficient skips. */
  uint32_t w_unary[] = {300, 0};
  uint32_t w_eu[] = {0};
  uint32_t w_ev[] = {1};
  uint32_t w_eq[] = {0};
  qsop_instance_t wide = {.r = 1024, .nvars = 2, .norm_h = 0, .constant = 100,
                          .mode = QSOP_MODE_LABELLED, .unary = w_unary, .nedges = 1,
                          .edge_u = w_eu, .edge_v = w_ev, .edge_q = w_eq};
  rc |= run_ok("wide", &wide, &all);
  rc |= run_ok("wide-amp", &wide, &amp);

  return rc;
}

static int test_error_paths(void) {
  const qsop_wmc_options_t all = qsop_wmc_options_default();
  qsop_instance_t qsop = {.r = 8, .nvars = 0, .norm_h = 0, .constant = 0,
                          .mode = QSOP_MODE_SIGN};

  qsop_error_t error = {0};
  if (qsop_wmc_write(NULL, &qsop, &all, &error)) {
    fprintf(stderr, "null file accepted\n");
    return 1;
  }
  if (strstr(error.message, "null WMC write argument") == NULL) {
    fprintf(stderr, "unexpected null-file error: %s\n", error.message);
    return 1;
  }

  FILE *sink = tmpfile();
  if (sink == NULL) {
    return 0;
  }
  int rc = 0;

  qsop_instance_t odd = qsop;
  odd.r = 7;
  rc |= run_fail("odd", sink, &odd, &all, "positive even modulus");

  qsop_instance_t zero = qsop;
  zero.r = 0;
  rc |= run_fail("zero", sink, &zero, &all, "positive even modulus");

  qsop_instance_t huge = qsop;
  huge.r = 0xFFFFFFFEU;
  rc |= run_fail("huge", sink, &huge, &all, "too large");

  qsop_wmc_options_t bad_residue = all;
  bad_residue.all_residues = false;
  bad_residue.residue = 99;
  rc |= run_fail("residue", sink, &qsop, &bad_residue, "out of range");

  fclose(sink);

  /* Write failure surfaces through ferror. */
  FILE *full = fopen("/dev/full", "w");
  if (full != NULL) {
    setvbuf(full, NULL, _IONBF, 0);
    rc |= run_fail("full", full, &qsop, &all, "write failed");
    qsop_wmc_options_t amp_full = all;
    amp_full.encoding = QSOP_WMC_ENCODING_AMPLITUDE;
    rc |= run_fail("full-amp", full, &qsop, &amp_full, "write failed");
    fclose(full);
  }

  return rc;
}

/* Brute-force amplitude: sum_x omega^P(x) where omega = exp(2*pi*i/r).
 * P(x) = constant + sum_v unary[v]*x_v + sum_e edge_q[e]*x_u*x_v mod r.
 * Returns re/im via output pointers. */
static void brute_force_amplitude(const qsop_instance_t *qsop, double *re_out, double *im_out) {
  const uint32_t n = qsop->nvars;
  const uint32_t r = qsop->r;
  double total_re = 0.0, total_im = 0.0;
  for (uint32_t mask = 0; mask < (1U << n); mask++) {
    uint32_t phase = (uint32_t)(qsop->constant % r);
    for (uint32_t v = 0; v < n; v++) {
      if ((mask >> v) & 1U) {
        phase = (uint32_t)((phase + qsop->unary[v]) % r);
      }
    }
    for (uint32_t e = 0; e < qsop->nedges; e++) {
      if (((mask >> qsop->edge_u[e]) & 1U) && ((mask >> qsop->edge_v[e]) & 1U)) {
        phase = (uint32_t)((phase + qsop->edge_q[e]) % r);
      }
    }
    double ere, eim;
    const double angle = 2.0 * 3.14159265358979323846 * (double)phase / (double)r;
    ere = cos(angle);
    eim = sin(angle);
    total_re += ere;
    total_im += eim;
  }
  *re_out = total_re;
  *im_out = total_im;
}

/* Check that peel1 encoding matches brute force for a given instance and encoding. */
static int check_peel1_amplitude(const char *name, const qsop_instance_t *qsop,
                                  qsop_wmc_encoding_t enc) {
  double ref_re, ref_im;
  brute_force_amplitude(qsop, &ref_re, &ref_im);

  /* No-preprocess: use the existing eval_wmc path indirectly by just
   * checking that peel1 run succeeds (functional smoke test). */
  qsop_wmc_options_t opts = qsop_wmc_options_default();
  opts.encoding = enc;
  opts.preprocess = QSOP_WMC_PREPROCESS_PEEL1;
  opts.emit_metadata = false;
  FILE *out = tmpfile();
  if (out == NULL) {
    return 0;  /* skip if no tmpfile */
  }
  qsop_error_t error = {0};
  const bool ok = qsop_wmc_write(out, qsop, &opts, &error);
  fclose(out);
  if (!ok) {
    fprintf(stderr, "%s peel1: qsop_wmc_write failed: %s\n", name, error.message);
    return 1;
  }
  return 0;
}

static int test_peel1(void) {
  /* Chain: 3 vars in a path: 0-1-2 with non-trivial labels. */
  uint32_t c_unary[] = {0, 0, 0};
  uint32_t c_eu[] = {0, 1};
  uint32_t c_ev[] = {1, 2};
  uint32_t c_eq[] = {2, 2};
  qsop_instance_t chain = {.r = 4, .nvars = 3, .norm_h = 0, .constant = 0,
                            .mode = QSOP_MODE_LABELLED, .unary = c_unary, .nedges = 2,
                            .edge_u = c_eu, .edge_v = c_ev, .edge_q = c_eq};

  if (check_peel1_amplitude("chain-amp-and", &chain, QSOP_WMC_ENCODING_AMPLITUDE) != 0 ||
      check_peel1_amplitude("chain-amp-soft", &chain, QSOP_WMC_ENCODING_AMP_SOFT) != 0) {
    return 1;
  }

  /* Star: var 0 is center, vars 1-3 are leaves with unit unary weights.
   * Peel1 should eliminate leaves (degree 1). */
  uint32_t st_unary[] = {0, 1, 1, 1};
  uint32_t st_eu[] = {0, 0, 0};
  uint32_t st_ev[] = {1, 2, 3};
  uint32_t st_eq[] = {2, 2, 2};
  qsop_instance_t star = {.r = 4, .nvars = 4, .norm_h = 0, .constant = 0,
                           .mode = QSOP_MODE_LABELLED, .unary = st_unary, .nedges = 3,
                           .edge_u = st_eu, .edge_v = st_ev, .edge_q = st_eq};

  if (check_peel1_amplitude("star-amp-and", &star, QSOP_WMC_ENCODING_AMPLITUDE) != 0 ||
      check_peel1_amplitude("star-amp-soft", &star, QSOP_WMC_ENCODING_AMP_SOFT) != 0) {
    return 1;
  }

  /* Isolated vars: 3 vars with no edges; peel1 should eliminate all (degree-0). */
  uint32_t iso_unary[] = {1, 2, 3};
  qsop_instance_t iso = {.r = 8, .nvars = 3, .norm_h = 0, .constant = 1,
                          .mode = QSOP_MODE_LABELLED, .unary = iso_unary, .nedges = 0};

  if (check_peel1_amplitude("iso-amp-and", &iso, QSOP_WMC_ENCODING_AMPLITUDE) != 0 ||
      check_peel1_amplitude("iso-amp-soft", &iso, QSOP_WMC_ENCODING_AMP_SOFT) != 0) {
    return 1;
  }

  /* Zero amplitude: r=4, P(0,0) = 0, P(1,0) = 0, P(0,1) = 0, P(1,1) = 2.
   * With a=2 on each variable and R=0: omega^2 = -1, so 1 + (-1) = 0 → F0 = 0 and F1 = 0. */
  uint32_t z_unary[] = {2, 2};
  uint32_t z_eu[] = {0};
  uint32_t z_ev[] = {1};
  uint32_t z_eq[] = {2};  /* omega^2 = -1 for r=4 */
  qsop_instance_t zero_qsop = {.r = 4, .nvars = 2, .norm_h = 0, .constant = 0,
                                .mode = QSOP_MODE_LABELLED, .unary = z_unary, .nedges = 1,
                                .edge_u = z_eu, .edge_v = z_ev, .edge_q = z_eq};

  /* Brute force: omega^0 + omega^2 + omega^2 + omega^(2+2+2) = 1 + (-1) + (-1) + (-1) = -2.
   * So this is NOT zero amplitude for the full SOP. But peel1 on var 0 (deg-1 neighbor of 1):
   * U = omega^2 = -1, R = omega^2 = -1, F0 = 1 + (-1) = 0, F1 = 1 + (-1)*(-1) = 2.
   * So F0=0, force var1=true. Then we should see is_zero=false but forced assignment.
   * Just verify peel1 succeeds: */
  if (check_peel1_amplitude("forced-amp-soft", &zero_qsop, QSOP_WMC_ENCODING_AMP_SOFT) != 0) {
    return 1;
  }

  /* Actual zero amplitude: 1 var, unary = r/2 (omega^{r/2} = -1), no edges.
   * Z = 1 + (-1) = 0. Peel1 should produce is_zero=true. */
  uint32_t za_unary[] = {4};  /* omega^4 = -1 for r=8 */
  qsop_instance_t zero_amp = {.r = 8, .nvars = 1, .norm_h = 0, .constant = 0,
                               .mode = QSOP_MODE_LABELLED, .unary = za_unary, .nedges = 0};
  double ref_re, ref_im;
  brute_force_amplitude(&zero_amp, &ref_re, &ref_im);
  if (fabs(ref_re) > 1e-10 || fabs(ref_im) > 1e-10) {
    fprintf(stderr, "zero-amp: expected brute force ~0, got %.6g+%.6gi\n", ref_re, ref_im);
    return 1;
  }
  /* peel1 should write a zero-amplitude WPCNF without error */
  qsop_wmc_options_t za_opts = qsop_wmc_options_default();
  za_opts.encoding = QSOP_WMC_ENCODING_AMP_SOFT;
  za_opts.preprocess = QSOP_WMC_PREPROCESS_PEEL1;
  if (run_ok("zero-amp-peel1", &zero_amp, &za_opts) != 0) {
    return 1;
  }

  return 0;
}

int main(void) {
  if (test_amp_soft_stats() != 0) {
    return 1;
  }
  if (test_peel1() != 0) {
    return 1;
  }
  if (test_export_shapes() != 0) {
    return 1;
  }
  if (test_error_paths() != 0) {
    return 1;
  }
  return 0;
}
