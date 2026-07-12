/* rankwidth's complex64 single-Fourier join batches wide CSR/streaming rows through
 * complex_scale_f64 (see rw_complex64_execute_row in rankwidth.c): each row is stably
 * partitioned by sign flag and each partition gathered, scaled, and scattered back as a unit
 * instead of one branch-per-item scalar multiply. Getting the front/back partition bookkeeping
 * or the gather/scatter indices wrong is a silent wrong answer, and it only engages once a row
 * reaches the kernel's lane count -- below that it falls back to the untouched scalar loop.
 *
 * So: random instances solved four ways -- f64 with the CPU's native kernel forced through the
 * materialized (CSR) single-mode kernel, f64 forced onto the scalar vtable, and the untouched
 * long-double DP -- all against an exhaustive sum over every assignment. The generator mixes
 * sizes/densities/moduli so some joins stay narrow (scalar fallback) and others widen enough to
 * vectorize; the tally at the end fails if either regime, or both sign partitions, went
 * unexercised. */
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
  static const uint32_t densities[] = {30, 55, 75, 95};
  static const qsop_rankwidth_generator_t generators[] = {
      QSOP_RANKWIDTH_GENERATOR_BALANCED,
      QSOP_RANKWIDTH_GENERATOR_MIN_FILL,
  };
  const qsop_simd_vtable_t *native = qsop_simd_resolve(QSOP_SIMD_KERNEL_AUTO);
  const qsop_simd_vtable_t *scalar = qsop_simd_scalar_vtable();
  uint64_t seed = UINT64_C(0x51A7E5C0FFEEBEEF);
  uint32_t trials = 0;
  uint64_t total_vectorized_ops = 0;
  uint64_t total_scalar_fallback_ops = 0;

  for (uint32_t trial = 0; trial < 320U; trial++) {
    const uint64_t r = moduli[xorshift(&seed) % 4U];
    const uint32_t n = 6U + (uint32_t)(xorshift(&seed) % (MAX_VARS - 5U));
    const uint32_t density = densities[xorshift(&seed) % 4U];
    const qsop_rankwidth_generator_t generator = generators[xorshift(&seed) % 2U];
    const uint32_t target_mode = 1U + (uint32_t)(xorshift(&seed) % 2U);
    if (target_mode >= r) {
      continue;
    }

    qsop_instance_t *q = random_instance(&seed, r, n, density);
    if (q == NULL) {
      fprintf(stderr, "allocation failed\n");
      return 1;
    }

    qsop_rankwidth_decomposition_t *decomposition = NULL;
    qsop_error_t error = {0};
    if (!qsop_rankwidth_decomposition_generate(q, generator, &decomposition, &error)) {
      fprintf(stderr, "trial %" PRIu32 ": decomposition failed: %s\n", trial, error.message);
      qsop_free(q);
      return 1;
    }
    trials++;

    double want_re = 0.0;
    double want_im = 0.0;
    exhaustive_amplitude(q, target_mode, &want_re, &want_im);

    const qsop_rankwidth_single_mode_options_t native_options = {
        .kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED, .simd = native};
    const qsop_rankwidth_single_mode_options_t scalar_options = {
        .kernel = QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED, .simd = scalar};

    qsop_amplitude_t native_amp = {0};
    qsop_amplitude_t scalar_amp = {0};
    qsop_amplitude_t wide_amp = {0};
    qsop_solve_stats_t native_stats = {0};
    qsop_solve_stats_t scalar_stats = {0};
    const bool ok =
        qsop_solve_rankwidth_single_mode_f64_options(q, decomposition, MAX_VARS + 1U, target_mode,
                                                     &native_options, &native_amp, &native_stats,
                                                     NULL, &error) &&
        qsop_solve_rankwidth_single_mode_f64_options(q, decomposition, MAX_VARS + 1U, target_mode,
                                                     &scalar_options, &scalar_amp, &scalar_stats,
                                                     NULL, &error) &&
        qsop_solve_rankwidth_single_mode(q, decomposition, MAX_VARS + 1U, target_mode, &wide_amp,
                                         NULL, NULL, &error);
    if (!ok) {
      fprintf(stderr, "trial %" PRIu32 ": solve failed: %s\n", trial, error.message);
      qsop_rankwidth_decomposition_free(decomposition);
      qsop_free(q);
      return 1;
    }

    total_vectorized_ops += native_stats.simd_vectorized_ops;
    total_scalar_fallback_ops += native_stats.simd_scalar_fallback_ops;
    /* Forcing the scalar vtable must never take the vectorized branch. */
    if (scalar_stats.simd_vectorized_ops != 0) {
      fprintf(stderr, "trial %" PRIu32 ": scalar vtable reported %" PRIu64 " vectorized ops\n",
              trial, scalar_stats.simd_vectorized_ops);
      qsop_rankwidth_decomposition_free(decomposition);
      qsop_free(q);
      return 1;
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
        qsop_rankwidth_decomposition_free(decomposition);
        qsop_free(q);
        return 1;
      }
    }
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_free(q);
  }

  /* A row only vectorizes once it reaches the kernel's lane count, and the scalar fallback path
   * only exercises on narrow rows -- without this check the whole run could silently pass on
   * just one of the two regimes. The coverage build (-Dsimd=scalar) has no real SIMD kernel
   * compiled in at all, so native == scalar there and vectorized_ops is legitimately always 0;
   * only require it on a build that actually has something to vectorize with. */
  const bool native_is_scalar = native == scalar;
  if (trials < 100U || total_scalar_fallback_ops == 0 ||
      (!native_is_scalar && total_vectorized_ops == 0)) {
    fprintf(stderr,
            "generator too tame: %" PRIu32 " trials, %" PRIu64 " vectorized ops, %" PRIu64
            " scalar-fallback ops\n",
            trials, total_vectorized_ops, total_scalar_fallback_ops);
    return 1;
  }

  fprintf(stderr,
          "rankwidth join tests passed (%" PRIu32 " instances, %" PRIu64 " vectorized ops, %"
          PRIu64 " scalar-fallback ops, kernel %s)\n",
          trials, total_vectorized_ops, total_scalar_fallback_ops, qsop_simd_kernel_name(native));
  return 0;
}
