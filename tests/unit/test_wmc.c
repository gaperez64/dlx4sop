#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_wmc.h"

#include <inttypes.h>
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

int main(void) {
  if (test_export_shapes() != 0) {
    return 1;
  }
  if (test_error_paths() != 0) {
    return 1;
  }
  return 0;
}
