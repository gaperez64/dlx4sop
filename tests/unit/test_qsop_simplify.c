#include "dlx4sop/qsop.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Exhaustive amplitude: sum_x omega^P(x), omega = exp(2*pi*i/r),
 * P(x) = constant + sum_v unary[v]*x_v + (r/2)*sum_e x_u*x_v (mod r). */
static void exhaustive_amplitude(const qsop_instance_t *qsop, double *re_out, double *im_out) {
  const uint32_t n = qsop->nvars;
  const uint32_t r = (uint32_t)qsop->r;
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
        phase = (uint32_t)((phase + (r / 2U)) % r);
      }
    }
    const double angle = 2.0 * 3.14159265358979323846 * (double)phase / (double)r;
    total_re += cos(angle);
    total_im += sin(angle);
  }
  *re_out = total_re;
  *im_out = total_im;
}

/* Physical invariant preserved by the pass: probability = |amp|^2 * 2^-norm_h. */
static double probability(const qsop_instance_t *qsop) {
  double re = 0.0, im = 0.0;
  exhaustive_amplitude(qsop, &re, &im);
  return (re * re + im * im) * pow(2.0, -(double)qsop->norm_h);
}

static qsop_instance_t *make_instance(uint64_t r, uint32_t nvars, uint64_t norm_h,
                                      uint64_t constant, const uint64_t *unary, uint32_t nedges,
                                      const uint32_t *eu, const uint32_t *ev) {
  qsop_instance_t *q = calloc(1, sizeof(*q));
  q->unary = malloc((nvars == 0U ? 1U : nvars) * sizeof(*q->unary));
  q->edge_u = malloc((nedges == 0U ? 1U : nedges) * sizeof(*q->edge_u));
  q->edge_v = malloc((nedges == 0U ? 1U : nedges) * sizeof(*q->edge_v));
  if (q->unary == NULL || q->edge_u == NULL || q->edge_v == NULL) {
    qsop_free(q);
    return NULL;
  }
  q->r = r;
  q->nvars = nvars;
  q->norm_h = norm_h;
  q->constant = constant;
  q->nedges = nedges;
  for (uint32_t i = 0; i < nvars; i++) {
    q->unary[i] = unary[i];
  }
  for (uint32_t i = 0; i < nedges; i++) {
    q->edge_u[i] = eu[i];
    q->edge_v[i] = ev[i];
  }
  return q;
}

/* Simplify `q`, assert probability is preserved and the reported shape matches expectations. */
static int check_case(const char *name, qsop_instance_t *q, uint32_t want_nvars,
                      uint32_t want_nedges, uint64_t want_norm_h) {
  if (q == NULL) {
    fprintf(stderr, "%s: allocation failed\n", name);
    return 1;
  }
  const double before = probability(q);
  if (!qsop_simplify_hadamard(q)) {
    fprintf(stderr, "%s: qsop_simplify_hadamard reported allocation failure\n", name);
    qsop_free(q);
    return 1;
  }
  const double after = probability(q);
  int rc = 0;
  if (fabs(before - after) > 1e-9) {
    fprintf(stderr, "%s: probability changed %.12g -> %.12g\n", name, before, after);
    rc = 1;
  }
  if (q->nvars != want_nvars || q->nedges != want_nedges || q->norm_h != want_norm_h) {
    fprintf(stderr,
            "%s: shape nvars=%" PRIu32 " nedges=%" PRIu32 " norm_h=%" PRIu64
            " (wanted %" PRIu32 "/%" PRIu32 "/%" PRIu64 ")\n",
            name, q->nvars, q->nedges, q->norm_h, want_nvars, want_nedges, want_norm_h);
    rc = 1;
  }
  /* Idempotence: a second pass must not change anything. */
  const uint32_t nvars2 = q->nvars;
  const uint32_t nedges2 = q->nedges;
  const uint64_t norm_h2 = q->norm_h;
  if (!qsop_simplify_hadamard(q)) {
    fprintf(stderr, "%s: second pass reported allocation failure\n", name);
    rc = 1;
  } else if (q->nvars != nvars2 || q->nedges != nedges2 || q->norm_h != norm_h2) {
    fprintf(stderr, "%s: not idempotent\n", name);
    rc = 1;
  }
  qsop_free(q);
  return rc;
}

int main(void) {
  int rc = 0;

  /* cx;cx normalizes to the chain 0-1-2 (norm_h=4). Degree-1 elimination collapses it to the
   * single isolated variable of a single cx (nvars=1, norm_h=2). */
  {
    const uint64_t unary[] = {0, 0, 0};
    const uint32_t eu[] = {0, 1};
    const uint32_t ev[] = {1, 2};
    rc |= check_case("cx_cx_chain", make_instance(8, 3, 4, 0, unary, 2, eu, ev), 1, 0, 2);
  }

  /* Single sign edge with two Hadamards (a cz-like block) collapses to nvars=0. */
  {
    const uint64_t unary[] = {0, 0};
    const uint32_t eu[] = {0};
    const uint32_t ev[] = {1};
    rc |= check_case("edge_pair", make_instance(8, 2, 4, 0, unary, 1, eu, ev), 0, 0, 2);
  }

  /* Triangle with a linear phase: eliminating var 0 merges 2->1 (folding unary) and turns the
   * 1-2 chord into a self-loop (unary += r/2). Non-zero probability, no-op-free check. */
  {
    const uint64_t unary[] = {0, 0, 2};
    const uint32_t eu[] = {0, 1, 0};
    const uint32_t ev[] = {1, 2, 2};
    rc |= check_case("chord_self_loop", make_instance(8, 3, 2, 0, unary, 3, eu, ev), 1, 0, 0);
  }

  /* Star: a degree-1 leaf pins the high-degree center to 0, dropping every center edge. */
  {
    const uint64_t unary[] = {0, 0, 0, 0};
    const uint32_t eu[] = {0, 0, 0};
    const uint32_t ev[] = {1, 2, 3};
    rc |= check_case("star_center_pin", make_instance(8, 4, 6, 0, unary, 3, eu, ev), 2, 0, 4);
  }

  /* Non-zero unary on a degree-1 variable makes it ineligible: the pass is a no-op. */
  {
    const uint64_t unary[] = {1, 1};
    const uint32_t eu[] = {0};
    const uint32_t ev[] = {1};
    rc |= check_case("nonzero_unary_noop", make_instance(8, 2, 2, 0, unary, 1, eu, ev), 2, 1, 2);
  }

  /* norm_h < 2 guard: an eligible variable exists but cannot be removed without underflowing
   * the normalization, so the instance is left untouched. */
  {
    const uint64_t unary[] = {0, 0, 0};
    const uint32_t eu[] = {0, 1};
    const uint32_t ev[] = {1, 2};
    rc |= check_case("norm_h_guard", make_instance(8, 3, 1, 0, unary, 2, eu, ev), 3, 2, 1);
  }

  /* Odd modulus is rejected defensively (not a valid sign format): no-op. */
  {
    const uint64_t unary[] = {0, 0};
    const uint32_t eu[] = {0};
    const uint32_t ev[] = {1};
    qsop_instance_t *q = make_instance(7, 2, 4, 0, unary, 1, eu, ev);
    if (q == NULL || !qsop_simplify_hadamard(q)) {
      fprintf(stderr, "odd_modulus: unexpected failure\n");
      rc = 1;
    } else if (q->nvars != 2U || q->nedges != 1U || q->norm_h != 4U) {
      fprintf(stderr, "odd_modulus: instance changed\n");
      rc = 1;
    }
    qsop_free(q);
  }

  return rc == 0 ? 0 : 1;
}
