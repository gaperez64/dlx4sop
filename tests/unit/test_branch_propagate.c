/* qsop_residual_propagate rewrites the residual mid-search, so the only guard that matters is that
 * the amplitude it produces is the amplitude. Random instances are checked against an exhaustive
 * sum, with propagation on and off, over odd and even target modes -- an even mode must disable the
 * rule (omega^(r/2) raised to an even power is 1, and the XOR constraint the rule rests on
 * disappears), and the solver has to notice that by itself. */
#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t xorshift(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static void exhaustive_amplitude(const qsop_instance_t *q, uint32_t target_mode, double *re_out,
                                 double *im_out) {
  const uint32_t n = q->nvars;
  const uint32_t r = (uint32_t)q->r;
  double total_re = 0.0, total_im = 0.0;
  for (uint32_t mask = 0; mask < (1U << n); mask++) {
    uint64_t phase = q->constant % r;
    for (uint32_t v = 0; v < n; v++) {
      if ((mask >> v) & 1U) {
        phase = (phase + q->unary[v]) % r;
      }
    }
    for (uint32_t e = 0; e < q->nedges; e++) {
      if (((mask >> q->edge_u[e]) & 1U) && ((mask >> q->edge_v[e]) & 1U)) {
        phase = (phase + (r / 2U)) % r;
      }
    }
    const double angle =
        2.0 * 3.14159265358979323846 * (double)((phase * target_mode) % r) / (double)r;
    total_re += cos(angle);
    total_im += sin(angle);
  }
  *re_out = total_re;
  *im_out = total_im;
}

static qsop_instance_t *random_instance(uint64_t *seed, uint64_t r, uint32_t n) {
  qsop_instance_t *q = calloc(1, sizeof(*q));
  if (q == NULL) {
    return NULL;
  }
  uint32_t eu[64], ev[64];
  uint32_t m = 0;
  for (uint32_t a = 0; a < n && m < 60U; a++) {
    for (uint32_t b = a + 1U; b < n && m < 60U; b++) {
      if (xorshift(seed) % 100U < 32U) {
        eu[m] = a;
        ev[m] = b;
        m++;
      }
    }
  }
  q->r = r;
  q->nvars = n;
  q->nedges = m;
  q->norm_h = 2U * n;
  q->constant = xorshift(seed) % r;
  q->unary = malloc((n == 0U ? 1U : n) * sizeof(*q->unary));
  q->edge_u = malloc((m == 0U ? 1U : m) * sizeof(*q->edge_u));
  q->edge_v = malloc((m == 0U ? 1U : m) * sizeof(*q->edge_v));
  if (q->unary == NULL || q->edge_u == NULL || q->edge_v == NULL) {
    qsop_free(q);
    return NULL;
  }
  for (uint32_t v = 0; v < n; v++) {
    /* Bias hard towards {0, r/2}: those are the variables the propagation rule can eliminate, and
     * an unbiased generator almost never produces the pin cascade or the zero certificate. */
    q->unary[v] =
        (xorshift(seed) % 4U) != 0U ? (xorshift(seed) % 2U) * (r / 2U) : xorshift(seed) % r;
  }
  for (uint32_t i = 0; i < m; i++) {
    q->edge_u[i] = eu[i];
    q->edge_v[i] = ev[i];
  }
  return q;
}

static bool solve(const qsop_instance_t *q, uint32_t target_mode,
                  qsop_branch_single_propagate_t propagate, qsop_amplitude_t *amp,
                  qsop_solve_stats_t *stats) {
  const qsop_branch_single_mode_options_t options = {
      .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
      .propagate = propagate,
      .max_fallback_vars = 64,
      .cache_min_vars = 2,
  };
  qsop_error_t error = {0};
  if (!qsop_solve_branch_single_mode(q, 64U, target_mode, &options, amp, stats, &error)) {
    fprintf(stderr, "solve failed: %s\n", error.message);
    return false;
  }
  return true;
}

int main(void) {
  static const uint64_t moduli[] = {2, 4, 8, 16};
  static const uint32_t target_modes[] = {1, 2, 3};
  uint64_t seed = UINT64_C(0xC0FFEE123456789);
  uint64_t propagations = 0;
  uint64_t zero_prunes = 0;
  uint32_t nonzero_cases = 0;

  for (uint32_t trial = 0; trial < 1500U; trial++) {
    const uint64_t r = moduli[xorshift(&seed) % 4U];
    const uint32_t n = 1U + (uint32_t)(xorshift(&seed) % 9U);
    const uint32_t target_mode = target_modes[xorshift(&seed) % 3U];
    if (target_mode >= r) {
      continue;
    }

    qsop_instance_t *q = random_instance(&seed, r, n);
    if (q == NULL) {
      fprintf(stderr, "allocation failed\n");
      return 1;
    }

    double want_re = 0.0, want_im = 0.0;
    exhaustive_amplitude(q, target_mode, &want_re, &want_im);

    qsop_amplitude_t on = {0}, off = {0};
    qsop_solve_stats_t on_stats = {0}, off_stats = {0};
    if (!solve(q, target_mode, QSOP_BRANCH_SINGLE_PROPAGATE_AUTO, &on, &on_stats) ||
        !solve(q, target_mode, QSOP_BRANCH_SINGLE_PROPAGATE_OFF, &off, &off_stats)) {
      qsop_free(q);
      return 1;
    }
    propagations += on_stats.branch_propagations;
    zero_prunes += on_stats.branch_zero_prunes;
    if (fabs(want_re) > 1e-6 || fabs(want_im) > 1e-6) {
      nonzero_cases++;
    }

    const double tol = 1e-6 * (1.0 + fabs(want_re) + fabs(want_im));
    if (fabs((double)on.re - want_re) > tol || fabs((double)on.im - want_im) > tol) {
      fprintf(stderr,
              "trial %" PRIu32 " (r=%" PRIu64 " n=%" PRIu32 " mode=%" PRIu32
              "): propagate=auto gave (%.9g,%.9g), exhaustive says (%.9g,%.9g)\n",
              trial, r, n, target_mode, (double)on.re, (double)on.im, want_re, want_im);
      qsop_free(q);
      return 1;
    }
    if (fabs((double)off.re - want_re) > tol || fabs((double)off.im - want_im) > tol) {
      fprintf(stderr,
              "trial %" PRIu32 ": propagate=off gave (%.9g,%.9g), exhaustive says (%.9g,%.9g)\n",
              trial, (double)off.re, (double)off.im, want_re, want_im);
      qsop_free(q);
      return 1;
    }

    /* Even target modes must not propagate: the rule is unsound there. */
    if (target_mode % 2U == 0U && on_stats.branch_propagations != 0) {
      fprintf(stderr, "trial %" PRIu32 ": propagated on even target mode %" PRIu32 "\n", trial,
              target_mode);
      qsop_free(q);
      return 1;
    }
    qsop_free(q);
  }

  /* A generator that never fires the rules would pass the checks above vacuously. */
  if (propagations < 500U || zero_prunes < 20U || nonzero_cases < 100U) {
    fprintf(stderr,
            "generator too tame: %" PRIu64 " propagations, %" PRIu64 " zero prunes, %" PRIu32
            " non-zero amplitudes\n",
            propagations, zero_prunes, nonzero_cases);
    return 1;
  }

  fprintf(stderr,
          "branch propagation tests passed (%" PRIu64 " eliminations, %" PRIu64 " zero prunes)\n",
          propagations, zero_prunes);
  return 0;
}
