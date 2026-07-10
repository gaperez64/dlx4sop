/* Every SIMD kernel compiled into this binary must agree with the scalar reference, and
 * qsop_simd_resolve must never hand back a kernel this CPU cannot execute -- doing so used to mean
 * SIGILL rather than a wrong answer. Both properties are checked here across the vector-width
 * boundary (n < width, n == width, n with a tail) because that boundary is where a hand-written
 * remainder loop goes wrong. */
#include "dlx4sop/bitset.h"
#include "dlx4sop/simd.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_N 40U

static uint64_t xorshift(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

/* Intrinsics may be contracted into fma on translation units whose -m flags enable it (AVX-512F
 * does; -mavx2 does not), so the complex kernels are compared to within a few ulp rather than
 * bit-for-bit. The integer kernels must match exactly. */
static bool close_enough(double a, double b) {
  const double scale = fabs(b) > 1.0 ? fabs(b) : 1.0;
  return fabs(a - b) <= 1e-12 * scale;
}

static int check_complex_mul(const qsop_simd_vtable_t *vt, const char *name, uint64_t *seed) {
  const qsop_simd_vtable_t *ref = qsop_simd_scalar_vtable();
  for (size_t n = 0; n <= MAX_N; n++) {
    double lre[MAX_N], lim[MAX_N], rre[MAX_N], rim[MAX_N];
    double ore[MAX_N], oim[MAX_N], xre[MAX_N], xim[MAX_N];
    for (size_t i = 0; i < n; i++) {
      lre[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
      lim[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
      rre[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
      rim[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
    }
    ref->complex_mul_assign_f64(xre, xim, lre, lim, rre, rim, n);
    vt->complex_mul_assign_f64(ore, oim, lre, lim, rre, rim, n);
    for (size_t i = 0; i < n; i++) {
      if (!close_enough(ore[i], xre[i]) || !close_enough(oim[i], xim[i])) {
        fprintf(stderr, "%s: complex_mul_assign_f64 mismatch at n=%zu i=%zu: (%g,%g) != (%g,%g)\n",
                name, n, i, ore[i], oim[i], xre[i], xim[i]);
        return 1;
      }
    }
  }
  return 0;
}

static int check_complex_scale(const qsop_simd_vtable_t *vt, const char *name, uint64_t *seed) {
  const qsop_simd_vtable_t *ref = qsop_simd_scalar_vtable();
  for (size_t n = 0; n <= MAX_N; n++) {
    double in_re[MAX_N], in_im[MAX_N];
    double ore[MAX_N], oim[MAX_N], xre[MAX_N], xim[MAX_N];
    for (size_t i = 0; i < n; i++) {
      in_re[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
      in_im[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
    }
    const double sre = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
    const double sim = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
    ref->complex_scale_f64(xre, xim, in_re, in_im, sre, sim, n);
    vt->complex_scale_f64(ore, oim, in_re, in_im, sre, sim, n);
    for (size_t i = 0; i < n; i++) {
      if (!close_enough(ore[i], xre[i]) || !close_enough(oim[i], xim[i])) {
        fprintf(stderr, "%s: complex_scale_f64 mismatch at n=%zu i=%zu: (%g,%g) != (%g,%g)\n", name,
                n, i, ore[i], oim[i], xre[i], xim[i]);
        return 1;
      }
    }
  }
  return 0;
}

static int check_sum_out_pairs(const qsop_simd_vtable_t *vt, const char *name, uint64_t *seed) {
  const qsop_simd_vtable_t *ref = qsop_simd_scalar_vtable();
  for (size_t pairs = 0; pairs <= MAX_N; pairs++) {
    double in_re[2U * MAX_N], in_im[2U * MAX_N];
    double ore[MAX_N], oim[MAX_N], xre[MAX_N], xim[MAX_N];
    for (size_t i = 0; i < 2U * pairs; i++) {
      in_re[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
      in_im[i] = (double)(int64_t)(xorshift(seed) % 2001U) - 1000.0;
    }
    ref->complex_sum_out_pairs_f64(xre, xim, in_re, in_im, pairs);
    vt->complex_sum_out_pairs_f64(ore, oim, in_re, in_im, pairs);
    for (size_t i = 0; i < pairs; i++) {
      if (ore[i] != xre[i] || oim[i] != xim[i]) {
        fprintf(stderr, "%s: complex_sum_out_pairs_f64 mismatch at pairs=%zu i=%zu\n", name, pairs,
                i);
        return 1;
      }
    }
  }
  return 0;
}

static int check_bitset_ops(const qsop_simd_vtable_t *vt, const char *name, uint64_t *seed) {
  const qsop_simd_vtable_t *ref = qsop_simd_scalar_vtable();
  for (size_t words = 0; words <= MAX_N; words++) {
    uint64_t a[MAX_N], b[MAX_N], got[MAX_N], want[MAX_N];
    for (size_t i = 0; i < words; i++) {
      a[i] = xorshift(seed);
      b[i] = xorshift(seed);
    }

    if (vt->popcount_and_u64(a, b, words) != ref->popcount_and_u64(a, b, words) ||
        vt->popcount_andnot_u64(a, b, words) != ref->popcount_andnot_u64(a, b, words)) {
      fprintf(stderr, "%s: popcount mismatch at words=%zu\n", name, words);
      return 1;
    }

    struct {
      const char *op;
      void (*fn)(uint64_t *, const uint64_t *, size_t);
      void (*ref_fn)(uint64_t *, const uint64_t *, size_t);
    } cases[] = {
        {"xor", vt->xor_u64, ref->xor_u64},
        {"or", vt->or_u64, ref->or_u64},
        {"and", vt->and_u64, ref->and_u64},
        {"andnot", vt->andnot_u64, ref->andnot_u64},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      memcpy(got, a, words * sizeof(*a));
      memcpy(want, a, words * sizeof(*a));
      cases[c].fn(got, b, words);
      cases[c].ref_fn(want, b, words);
      if (words != 0 && memcmp(got, want, words * sizeof(*a)) != 0) {
        fprintf(stderr, "%s: %s_u64 mismatch at words=%zu\n", name, cases[c].op, words);
        return 1;
      }
    }

    memcpy(got, a, words * sizeof(*a));
    memcpy(want, a, words * sizeof(*a));
    qsop_bitset_xor_simd(got, b, words, vt);
    ref->xor_u64(want, b, words);
    if (words != 0 && memcmp(got, want, words * sizeof(*a)) != 0) {
      fprintf(stderr, "%s: guarded xor wrapper mismatch at words=%zu\n", name, words);
      return 1;
    }
    memcpy(got, a, words * sizeof(*a));
    memcpy(want, a, words * sizeof(*a));
    qsop_bitset_or_simd(got, b, words, vt);
    ref->or_u64(want, b, words);
    if (words != 0 && memcmp(got, want, words * sizeof(*a)) != 0) {
      fprintf(stderr, "%s: guarded or wrapper mismatch at words=%zu\n", name, words);
      return 1;
    }
    memcpy(got, a, words * sizeof(*a));
    memcpy(want, a, words * sizeof(*a));
    qsop_bitset_and_simd(got, b, words, vt);
    ref->and_u64(want, b, words);
    if (words != 0 && memcmp(got, want, words * sizeof(*a)) != 0) {
      fprintf(stderr, "%s: guarded and wrapper mismatch at words=%zu\n", name, words);
      return 1;
    }
    memcpy(got, a, words * sizeof(*a));
    memcpy(want, a, words * sizeof(*a));
    qsop_bitset_and_not_simd(got, b, words, vt);
    ref->andnot_u64(want, b, words);
    if (words != 0 && memcmp(got, want, words * sizeof(*a)) != 0) {
      fprintf(stderr, "%s: guarded andnot wrapper mismatch at words=%zu\n", name, words);
      return 1;
    }
    if (qsop_bitset_popcount_intersection_simd(a, b, words, vt) !=
            ref->popcount_and_u64(a, b, words) ||
        qsop_bitset_popcount_andnot_simd(a, b, words, vt) !=
            ref->popcount_andnot_u64(a, b, words)) {
      fprintf(stderr, "%s: guarded popcount wrapper mismatch at words=%zu\n", name, words);
      return 1;
    }
  }
  if (vt->min_lanes > 0 &&
      qsop_bitset_simd_worthwhile(vt, vt->min_lanes - 1U)) {
    fprintf(stderr, "%s: bitset SIMD guard accepted a short row\n", name);
    return 1;
  }
  if (!qsop_bitset_simd_worthwhile(vt, vt->min_lanes)) {
    fprintf(stderr, "%s: bitset SIMD guard rejected one full vector\n", name);
    return 1;
  }
  return 0;
}

int main(void) {
  int failures = 0;
  uint64_t seed = UINT64_C(0x243F6A8885A308D3);

  const qsop_simd_vtable_t *chosen = qsop_simd_resolve(QSOP_SIMD_KERNEL_AUTO);
  if (chosen == NULL) {
    fprintf(stderr, "qsop_simd_resolve(AUTO) returned NULL; scalar must always be available\n");
    return 1;
  }
  fprintf(stderr, "compiled kernels: %s; runtime selection: %s\n", qsop_simd_compiled_arch(),
          qsop_simd_kernel_name(chosen));

  if (qsop_simd_resolve(QSOP_SIMD_KERNEL_SCALAR) != qsop_simd_scalar_vtable()) {
    fprintf(stderr, "scalar kernel must always resolve\n");
    failures++;
  }

  /* Whatever AUTO picked is, by construction, runnable here. Anything else that resolves is also
   * runnable, so every non-NULL vtable below is safe to call. */
  static const qsop_simd_kernel_t kernels[] = {
      QSOP_SIMD_KERNEL_SCALAR,
      QSOP_SIMD_KERNEL_NEON,
      QSOP_SIMD_KERNEL_AVX2,
      QSOP_SIMD_KERNEL_AVX512,
  };
  uint32_t exercised = 0;
  for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
    const qsop_simd_vtable_t *vt = qsop_simd_resolve(kernels[i]);
    if (vt == NULL) {
      continue; /* not compiled in, or this CPU cannot run it */
    }
    const char *name = qsop_simd_kernel_name(vt);
    exercised++;
    failures += check_complex_mul(vt, name, &seed);
    failures += check_sum_out_pairs(vt, name, &seed);
    failures += check_complex_scale(vt, name, &seed);
    failures += check_bitset_ops(vt, name, &seed);
  }

  if (exercised == 0) {
    fprintf(stderr, "no kernels exercised\n");
    return 1;
  }
  if (failures != 0) {
    fprintf(stderr, "%d SIMD kernel test(s) FAILED\n", failures);
    return 1;
  }
  fprintf(stderr, "SIMD kernel tests passed (%" PRIu32 " kernel(s))\n", exercised);
  return 0;
}
