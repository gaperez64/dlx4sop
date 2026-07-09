/* The single-Fourier amplitude cache keys on the residual *without* its additive constant, storing
 * amp * conj(omega^constant) and re-applying the phase on a hit. That is only sound because the
 * constant enters the amplitude as a unit-modulus factor and nothing else, and getting it backwards
 * is invisible to every other test: a small random component always delegates to the treewidth DP
 * on the first probe, so the recursion -- and therefore the cache -- is never entered.
 *
 * Forcing treewidth_delegate_max_width to 1 makes every component wider than a path branch instead,
 * which is what puts entries in the cache and hits on them. Amplitudes are then checked against an
 * exhaustive sum over all 2^n assignments, at cache_min_vars settings that both do and do not
 * memoise, and with propagation on and off. The final tallies fail the test if the search never
 * actually cached anything, since the checks above would then pass vacuously. */
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
  const uint32_t r = (uint32_t)q->r;
  double total_re = 0.0;
  double total_im = 0.0;
  for (uint32_t mask = 0; mask < (1U << q->nvars); mask++) {
    uint64_t phase = q->constant % r;
    for (uint32_t v = 0; v < q->nvars; v++) {
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

/* Denser than test_branch_propagate's generator, and with an unbiased unary: the point here is to
 * reach the recursion with a nonzero constant and a residual that recurs, not to fire the pin
 * cascade. A nonzero constant on most instances is what makes the phase bookkeeping observable. */
static qsop_instance_t *random_instance(uint64_t *seed, uint64_t r, uint32_t n) {
  qsop_instance_t *q = calloc(1, sizeof(*q));
  if (q == NULL) {
    return NULL;
  }
  uint32_t eu[128];
  uint32_t ev[128];
  uint32_t m = 0;
  for (uint32_t a = 0; a < n && m < 120U; a++) {
    for (uint32_t b = a + 1U; b < n && m < 120U; b++) {
      if (xorshift(seed) % 100U < 45U) {
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
    q->unary[v] = xorshift(seed) % r;
  }
  for (uint32_t i = 0; i < m; i++) {
    q->edge_u[i] = eu[i];
    q->edge_v[i] = ev[i];
  }
  return q;
}

static bool solve(const qsop_instance_t *q, uint32_t target_mode, uint32_t cache_min_vars,
                  qsop_branch_single_propagate_t propagate, qsop_amplitude_t *amp,
                  qsop_solve_stats_t *stats) {
  const qsop_branch_single_mode_options_t options = {
      .fallback = QSOP_BRANCH_SINGLE_FALLBACK_ALWAYS,
      .propagate = propagate,
      .max_fallback_vars = 64,
      .cache_min_vars = cache_min_vars,
      .treewidth_delegate_max_width = 1,
      .rankwidth_delegate_max_width = 1,
  };
  qsop_error_t error = {0};
  if (!qsop_solve_branch_single_mode(q, 64U, target_mode, &options, amp, stats, &error)) {
    fprintf(stderr, "solve failed: %s\n", error.message);
    return false;
  }
  return true;
}

static double raw(long double mantissa, int32_t exp) {
  return (double)ldexpl(mantissa, exp);
}

int main(void) {
  static const uint64_t moduli[] = {2, 4, 8, 16};
  static const uint32_t target_modes[] = {1, 2, 3};
  uint64_t seed = UINT64_C(0x5EED1234ABCDEF01);
  uint64_t cache_hits = 0;
  uint64_t cache_entries = 0;
  uint32_t nonzero_constants = 0;

  for (uint32_t trial = 0; trial < 900U; trial++) {
    const uint64_t r = moduli[xorshift(&seed) % 4U];
    const uint32_t n = 3U + (uint32_t)(xorshift(&seed) % 8U);
    const uint32_t target_mode = target_modes[xorshift(&seed) % 3U];
    if (target_mode >= r) {
      continue;
    }

    qsop_instance_t *q = random_instance(&seed, r, n);
    if (q == NULL) {
      fprintf(stderr, "allocation failed\n");
      return 1;
    }
    if (q->constant != 0) {
      nonzero_constants++;
    }

    double want_re = 0.0;
    double want_im = 0.0;
    exhaustive_amplitude(q, target_mode, &want_re, &want_im);

    /* cache_min_vars 2 memoises nearly every node; 64 memoises none. Both must agree with the
     * exhaustive sum, which is what pins the stored value to the constant-free class. */
    const uint32_t min_vars[] = {2U, 64U};
    const qsop_branch_single_propagate_t modes[] = {QSOP_BRANCH_SINGLE_PROPAGATE_AUTO,
                                                    QSOP_BRANCH_SINGLE_PROPAGATE_OFF};
    for (uint32_t i = 0; i < 2U; i++) {
      for (uint32_t j = 0; j < 2U; j++) {
        qsop_amplitude_t amp = {0};
        qsop_solve_stats_t stats = {0};
        if (!solve(q, target_mode, min_vars[i], modes[j], &amp, &stats)) {
          qsop_free(q);
          return 1;
        }
        if (min_vars[i] == 2U) {
          cache_hits += stats.cache_hits;
          cache_entries += stats.cache_entries;
        }
        const double got_re = raw(amp.re, amp.scale_exp2);
        const double got_im = raw(amp.im, amp.scale_exp2);
        const double tol = 1e-6 * (1.0 + fabs(want_re) + fabs(want_im));
        if (fabs(got_re - want_re) > tol || fabs(got_im - want_im) > tol) {
          fprintf(stderr,
                  "trial %" PRIu32 " (r=%" PRIu64 " n=%" PRIu32 " mode=%" PRIu32 " cst=%" PRIu64
                  " cache_min_vars=%" PRIu32 " propagate=%s): got (%.9g,%.9g), exhaustive says "
                  "(%.9g,%.9g)\n",
                  trial, r, n, target_mode, q->constant, min_vars[i],
                  modes[j] == QSOP_BRANCH_SINGLE_PROPAGATE_OFF ? "off" : "auto", got_re, got_im,
                  want_re, want_im);
          qsop_free(q);
          return 1;
        }
      }
    }
    qsop_free(q);
  }

  if (cache_hits < 100U || cache_entries < 100U || nonzero_constants < 100U) {
    fprintf(stderr,
            "search never exercised the cache: %" PRIu64 " hits, %" PRIu64 " entries, %" PRIu32
            " instances with a nonzero constant\n",
            cache_hits, cache_entries, nonzero_constants);
    return 1;
  }

  fprintf(stderr,
          "branch single-Fourier cache tests passed (%" PRIu64 " hits over %" PRIu64 " entries)\n",
          cache_hits, cache_entries);
  return 0;
}
