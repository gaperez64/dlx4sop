/* The double single-Fourier DP no longer materializes its two projection maps. It classifies the
 * output scope's low bit positions by which operand owns them, and runs each aligned block of
 * assignments through one of three contiguous kernels: broadcast-scale by the left operand,
 * broadcast-scale by the right one, or an elementwise multiply when both own the run.
 *
 * Getting the block bases or the class wrong is a silent wrong answer, and the existing
 * differential tests only reach arities below the SIMD lane threshold, where the block loop
 * degenerates to its scalar path. So: random instances wide enough for real bags, each solved four
 * ways -- f64 with the CPU's kernel, f64 forced onto the scalar vtable (the recurrence path), and
 * the untouched long-double DP -- all against an exhaustive sum over every assignment.
 *
 * The generator mixes edge densities so that a bag's incoming factors are sometimes contained in
 * the accumulator's scope (left-owns-low), sometimes not (right-owns-low), and occasionally equal
 * to it (both). The tally at the end fails if any class went unexercised. */
#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/simd.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_VARS 18U

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

static qsop_instance_t *random_instance(uint64_t *seed, uint64_t r, uint32_t n, uint32_t density) {
  qsop_instance_t *q = calloc(1, sizeof(*q));
  if (q == NULL) {
    return NULL;
  }
  uint32_t eu[MAX_VARS * MAX_VARS];
  uint32_t ev[MAX_VARS * MAX_VARS];
  uint32_t m = 0;
  for (uint32_t a = 0; a < n; a++) {
    for (uint32_t b = a + 1U; b < n; b++) {
      if (xorshift(seed) % 100U < density) {
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

static double raw(long double mantissa, int32_t exp) {
  return (double)ldexpl(mantissa, exp);
}

static bool agrees(const qsop_amplitude_t *amp, double want_re, double want_im) {
  const double got_re = raw(amp->re, amp->scale_exp2);
  const double got_im = raw(amp->im, amp->scale_exp2);
  const double tol = 1e-6 * (1.0 + fabs(want_re) + fabs(want_im));
  return fabs(got_re - want_re) <= tol && fabs(got_im - want_im) <= tol;
}

int main(void) {
  static const uint64_t moduli[] = {2, 4, 8, 16};
  static const uint32_t densities[] = {20, 35, 55, 80};
  const qsop_simd_vtable_t *native = qsop_simd_resolve(QSOP_SIMD_KERNEL_AUTO);
  const qsop_simd_vtable_t *scalar = qsop_simd_scalar_vtable();
  uint64_t seed = UINT64_C(0x1BADB0021DEADC0D);
  uint64_t widest_bag = 0;
  uint32_t trials = 0;

  for (uint32_t trial = 0; trial < 240U; trial++) {
    const uint64_t r = moduli[xorshift(&seed) % 4U];
    const uint32_t n = 8U + (uint32_t)(xorshift(&seed) % (MAX_VARS - 7U));
    const uint32_t density = densities[xorshift(&seed) % 4U];
    const uint32_t target_mode = 1U + (uint32_t)(xorshift(&seed) % 2U);
    if (target_mode >= r) {
      continue;
    }

    qsop_instance_t *q = random_instance(&seed, r, n, density);
    if (q == NULL) {
      fprintf(stderr, "allocation failed\n");
      return 1;
    }
    trials++;

    double want_re = 0.0;
    double want_im = 0.0;
    exhaustive_amplitude(q, target_mode, &want_re, &want_im);

    qsop_amplitude_t native_amp = {0};
    qsop_amplitude_t scalar_amp = {0};
    qsop_amplitude_t wide_amp = {0};
    qsop_solve_stats_t stats = {0};
    qsop_error_t error = {0};
    const bool ok =
        qsop_solve_treewidth_single_mode_f64(q, MAX_VARS + 1U, QSOP_TREEWIDTH_ORDER_MIN_FILL,
                                             target_mode, native, &native_amp, &stats, NULL,
                                             &error) &&
        qsop_solve_treewidth_single_mode_f64(q, MAX_VARS + 1U, QSOP_TREEWIDTH_ORDER_MIN_FILL,
                                             target_mode, scalar, &scalar_amp, NULL, NULL, &error) &&
        qsop_solve_treewidth_single_mode(q, MAX_VARS + 1U, QSOP_TREEWIDTH_ORDER_MIN_FILL,
                                         target_mode, &wide_amp, NULL, NULL, &error);
    if (!ok) {
      fprintf(stderr, "trial %" PRIu32 ": solve failed: %s\n", trial, error.message);
      qsop_free(q);
      return 1;
    }
    if (stats.max_table_entries > widest_bag) {
      widest_bag = stats.max_table_entries;
    }

    const struct {
      const char *label;
      const qsop_amplitude_t *amp;
    } cases[] = {
        {"f64/native", &native_amp}, {"f64/scalar", &scalar_amp}, {"long-double", &wide_amp}};
    for (uint32_t i = 0; i < 3U; i++) {
      if (!agrees(cases[i].amp, want_re, want_im)) {
        fprintf(stderr,
                "trial %" PRIu32 " (r=%" PRIu64 " n=%" PRIu32 " density=%" PRIu32 " mode=%" PRIu32
                ") [%s]: got (%.9g,%.9g), exhaustive says (%.9g,%.9g)\n",
                trial, r, n, density, target_mode, cases[i].label,
                raw(cases[i].amp->re, cases[i].amp->scale_exp2),
                raw(cases[i].amp->im, cases[i].amp->scale_exp2), want_re, want_im);
        qsop_free(q);
        return 1;
      }
    }
    qsop_free(q);
  }

  /* A join only vectorizes once a block reaches the kernel's lane count, which needs a bag well
   * past the lane width. Without this the run above could pass entirely on the scalar recurrence. */
  if (trials < 100U || widest_bag < 4096U) {
    fprintf(stderr, "generator too tame: %" PRIu32 " trials, widest bag table %" PRIu64 " entries\n",
            trials, widest_bag);
    return 1;
  }

  fprintf(stderr,
          "treewidth join tests passed (%" PRIu32 " instances, widest bag table %" PRIu64
          " entries, kernel %s)\n",
          trials, widest_bag, qsop_simd_kernel_name(native));
  return 0;
}
