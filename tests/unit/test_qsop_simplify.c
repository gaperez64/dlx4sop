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
            "%s: shape nvars=%" PRIu32 " nedges=%" PRIu32 " norm_h=%" PRIu64 " (wanted %" PRIu32
            "/%" PRIu32 "/%" PRIu64 ")\n",
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

static int expect_zero_amplitude(const char *name, const qsop_instance_t *q) {
  double re = 0.0, im = 0.0;
  exhaustive_amplitude(q, &re, &im);
  if (fabs(re) > 1e-9 || fabs(im) > 1e-9) {
    fprintf(stderr, "%s: fixture amplitude is not zero (%g,%g)\n", name, re, im);
    return 1;
  }
  return 0;
}

static uint64_t xorshift(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

/* The pass rescales the raw amplitude by 2 per elimination and compensates with norm_h -= 2, so
 * the quantity it must preserve exactly is the normalized amplitude amp * 2^(-norm_h/2) -- both
 * components, not just the modulus. Random instances are biased towards unary in {0, r/2} so the
 * pin, merge and zero-certificate rules all fire, and duplicate edges plus self-loops are injected
 * so the up-front canonicalization is exercised. */
static int normalized_amplitude(const qsop_instance_t *q, double *re_out, double *im_out) {
  double re = 0.0, im = 0.0;
  exhaustive_amplitude(q, &re, &im);
  const double scale = pow(2.0, -0.5 * (double)q->norm_h);
  *re_out = re * scale;
  *im_out = im * scale;
  return 0;
}

static int check_random(void) {
  static const uint64_t moduli[] = {2, 4, 8, 16};
  uint64_t seed = UINT64_C(0x9E3779B97F4A7C15);
  int rc = 0;
  uint32_t zero_cases = 0;

  for (uint32_t trial = 0; trial < 3000U; trial++) {
    const uint64_t r = moduli[xorshift(&seed) % 4U];
    const uint32_t n = 1U + (uint32_t)(xorshift(&seed) % 8U);
    const uint64_t norm_h = 2U * n;
    const uint64_t constant = xorshift(&seed) % r;

    uint64_t unary[8];
    for (uint32_t v = 0; v < n; v++) {
      unary[v] =
          (xorshift(&seed) % 2U) == 0U ? (xorshift(&seed) % 2U) * (r / 2U) : xorshift(&seed) % r;
    }

    uint32_t eu[64];
    uint32_t ev[64];
    uint32_t m = 0;
    for (uint32_t a = 0; a < n && m < 60U; a++) {
      for (uint32_t b = a; b < n && m < 60U; b++) {
        /* a == b injects a self-loop; a second copy of an edge exercises parity cancellation. */
        const uint64_t roll = xorshift(&seed) % 100U;
        if (a == b ? roll < 8U : roll < 30U) {
          eu[m] = a;
          ev[m] = b;
          m++;
          if (roll < 3U && m < 60U) {
            eu[m] = b;
            ev[m] = a;
            m++;
          }
        }
      }
    }

    qsop_instance_t *q = make_instance(r, n, norm_h, constant, unary, m, eu, ev);
    if (q == NULL) {
      fprintf(stderr, "random: allocation failed\n");
      return 1;
    }
    double re0 = 0.0, im0 = 0.0;
    normalized_amplitude(q, &re0, &im0);
    if (fabs(re0) < 1e-12 && fabs(im0) < 1e-12) {
      zero_cases++;
    }

    if (!qsop_simplify_hadamard(q)) {
      fprintf(stderr, "random trial %" PRIu32 ": simplify reported allocation failure\n", trial);
      qsop_free(q);
      return 1;
    }
    double re1 = 0.0, im1 = 0.0;
    normalized_amplitude(q, &re1, &im1);

    if (fabs(re0 - re1) > 1e-9 || fabs(im0 - im1) > 1e-9) {
      fprintf(stderr,
              "random trial %" PRIu32 " (r=%" PRIu64 ", n=%" PRIu32
              "): normalized amplitude changed (%.12g,%.12g) -> (%.12g,%.12g)\n",
              trial, r, n, re0, im0, re1, im1);
      rc = 1;
    }
    qsop_free(q);
    if (rc != 0) {
      return rc;
    }
  }

  /* Guard against a degenerate generator that never reaches the interesting rules. */
  if (zero_cases < 100U) {
    fprintf(stderr, "random: only %" PRIu32 " zero-amplitude cases; generator is too tame\n",
            zero_cases);
    return 1;
  }
  return 0;
}

int main(void) {
  int rc = 0;

  /* cx;cx normalizes to the chain 0-1-2 (norm_h=4). Degree-1 elimination pins 1 := 0, leaving 2
   * isolated with unary 0, which the degree-0 rule then folds away as a factor of 2. */
  {
    const uint64_t unary[] = {0, 0, 0};
    const uint32_t eu[] = {0, 1};
    const uint32_t ev[] = {1, 2};
    rc |= check_case("cx_cx_chain", make_instance(8, 3, 4, 0, unary, 2, eu, ev), 0, 0, 0);
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

  /* Star: a degree-1 leaf pins the high-degree center to 0, dropping every center edge and
   * stranding the other two leaves, which the degree-0 rule then folds away. */
  {
    const uint64_t unary[] = {0, 0, 0, 0};
    const uint32_t eu[] = {0, 0, 0};
    const uint32_t ev[] = {1, 2, 3};
    rc |= check_case("star_center_pin", make_instance(8, 4, 6, 0, unary, 3, eu, ev), 0, 0, 0);
  }

  /* Non-zero unary on a degree-1 variable makes it ineligible: the pass is a no-op. */
  {
    const uint64_t unary[] = {1, 1};
    const uint32_t eu[] = {0};
    const uint32_t ev[] = {1};
    rc |= check_case("nonzero_unary_noop", make_instance(8, 2, 2, 0, unary, 1, eu, ev), 2, 1, 2);
  }

  /* unary == r/2 on a degree-1 variable pins its neighbour to 1 (not 0): unary[1] folds into the
   * constant, and every other edge at the pinned variable degenerates to a unary r/2 term. Here
   * variable 1 has no other edge, so only the constant moves. */
  {
    const uint64_t unary[] = {4, 3, 1};
    const uint32_t eu[] = {0};
    const uint32_t ev[] = {1};
    rc |= check_case("sign_unary_pin_one", make_instance(8, 3, 4, 0, unary, 1, eu, ev), 1, 0, 2);
  }

  /* unary == r/2 on a degree-2 variable is the negated merge x_2 := 1 - x_1: unary[2] folds into
   * the constant and out of unary[1], edge (2,3) migrates to (1,3), and variable 3 picks up r/2. */
  {
    const uint64_t unary[] = {4, 1, 2, 3};
    const uint32_t eu[] = {0, 0, 2};
    const uint32_t ev[] = {1, 2, 3};
    rc |= check_case("sign_unary_merge_negated", make_instance(8, 4, 4, 0, unary, 3, eu, ev), 2, 1,
                     2);
  }

  /* Negated merge across a chord: the (keep,drop) edge is x*(1-x) == 0 and must vanish, rather
   * than fold into unary the way the non-negated merge's self-loop does. */
  {
    const uint64_t unary[] = {4, 1, 5};
    const uint32_t eu[] = {0, 0, 1};
    const uint32_t ev[] = {1, 2, 2};
    rc |=
        check_case("sign_unary_merge_chord", make_instance(8, 3, 2, 0, unary, 3, eu, ev), 1, 0, 0);
  }

  /* An isolated variable with unary r/2 has factor 1 + omega^(r/2) == 0, so the whole amplitude
   * vanishes and the instance collapses to the canonical zero witness (norm_h untouched). */
  {
    const uint64_t unary[] = {4, 2, 6};
    const uint32_t eu[] = {1};
    const uint32_t ev[] = {2};
    qsop_instance_t *q = make_instance(8, 3, 6, 3, unary, 1, eu, ev);
    rc |= expect_zero_amplitude("zero_witness", q);
    rc |= check_case("zero_witness", q, 1, 0, 6);
  }

  /* The certificate also fires after a cascade: pinning variable 1 to 1 leaves variable 2 isolated
   * with unary 0 + r/2, which is the zero factor. */
  {
    const uint64_t unary[] = {4, 3, 0};
    const uint32_t eu[] = {0, 1};
    const uint32_t ev[] = {1, 2};
    qsop_instance_t *q = make_instance(8, 3, 4, 0, unary, 2, eu, ev);
    rc |= expect_zero_amplitude("zero_witness_cascade", q);
    rc |= check_case("zero_witness_cascade", q, 1, 0, 2);
  }

  /* An isolated variable with unary 0 contributes a factor of 2: drop it, norm_h -= 2. The
   * surviving variable's unary 3 is not a multiple of r/2, so the pass stops there. */
  {
    const uint64_t unary[] = {0, 3};
    rc |=
        check_case("isolated_zero_unary", make_instance(8, 2, 4, 1, unary, 0, NULL, NULL), 1, 0, 2);
  }

  /* Self-loops and parity-cancelling duplicate edges are folded away before any degree is read;
   * the two (0,1) edges cancel, leaving variable 0 isolated with unary 2 + r/2 == 6 != r/2. */
  {
    const uint64_t unary[] = {2, 0};
    const uint32_t eu[] = {0, 0, 1};
    const uint32_t ev[] = {0, 1, 0};
    rc |= check_case("canonicalize_before_degrees", make_instance(8, 2, 4, 0, unary, 3, eu, ev), 1,
                     0, 2);
  }

  rc |= check_random();

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
