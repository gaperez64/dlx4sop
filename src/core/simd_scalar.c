#include "dlx4sop/simd.h"

#include "dlx4sop/bitset.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t simd_scalar_popcount_and_u64(const uint64_t *a, const uint64_t *b, size_t words) {
  uint32_t count = 0;
  for (size_t w = 0; w < words; w++) {
    count += qsop_popcount_u64(a[w] & b[w]);
  }
  return count;
}

static uint32_t simd_scalar_popcount_andnot_u64(const uint64_t *a, const uint64_t *b,
                                                size_t words) {
  uint32_t count = 0;
  for (size_t w = 0; w < words; w++) {
    count += qsop_popcount_u64(a[w] & ~b[w]);
  }
  return count;
}

static void simd_scalar_xor_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] ^= src[w];
  }
}

static void simd_scalar_or_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] |= src[w];
  }
}

static void simd_scalar_and_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] &= src[w];
  }
}

static void simd_scalar_andnot_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] &= ~src[w];
  }
}

static void simd_scalar_complex_mul_assign_f64(double *restrict out_re, double *restrict out_im,
                                               const double *restrict left_re,
                                               const double *restrict left_im,
                                               const double *restrict right_re,
                                               const double *restrict right_im, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const double lre = left_re[i];
    const double lim = left_im[i];
    const double rre = right_re[i];
    const double rim = right_im[i];
    out_re[i] = lre * rre - lim * rim;
    out_im[i] = lre * rim + lim * rre;
  }
}

static void simd_scalar_complex_scale_f64(double *restrict out_re, double *restrict out_im,
                                          const double *restrict in_re, const double *restrict in_im,
                                          double scale_re, double scale_im, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const double re = in_re[i];
    const double im = in_im[i];
    out_re[i] = re * scale_re - im * scale_im;
    out_im[i] = re * scale_im + im * scale_re;
  }
}

static void simd_scalar_complex_sum_out_pairs_f64(double *restrict out_re, double *restrict out_im,
                                                  const double *restrict in_re,
                                                  const double *restrict in_im, size_t pairs) {
  for (size_t i = 0; i < pairs; i++) {
    out_re[i] = in_re[i] + in_re[pairs + i];
    out_im[i] = in_im[i] + in_im[pairs + i];
  }
}

const qsop_simd_vtable_t *qsop_simd_scalar_vtable(void) {
  static const qsop_simd_vtable_t vt = {
      .name = "scalar",
      .min_lanes = 1,
      .popcount_and_u64 = simd_scalar_popcount_and_u64,
      .popcount_andnot_u64 = simd_scalar_popcount_andnot_u64,
      .xor_u64 = simd_scalar_xor_u64,
      .or_u64 = simd_scalar_or_u64,
      .and_u64 = simd_scalar_and_u64,
      .andnot_u64 = simd_scalar_andnot_u64,
      .complex_mul_assign_f64 = simd_scalar_complex_mul_assign_f64,
      .complex_scale_f64 = simd_scalar_complex_scale_f64,
      .complex_sum_out_pairs_f64 = simd_scalar_complex_sum_out_pairs_f64,
  };
  return &vt;
}

/* "Is this kernel compiled in?" is a QSOP_SIMD_HAVE_* macro, not a weak symbol. The weak-stub
 * trick does not survive LTO: the stub lives in an LTO object while the real definition lives in a
 * separate, non-LTO archive (each kernel needs its own -m flags), and GCC's LTO plugin is entitled
 * to resolve the weak definition it can see and inline the NULL return -- silently disabling every
 * SIMD kernel in exactly the release builds that want them. It also ICEs gcc's IPA-ICF pass. */
#if !defined(QSOP_SIMD_HAVE_NEON)
const qsop_simd_vtable_t *qsop_simd_neon_vtable(void) {
  return NULL;
}
#endif

#if !defined(QSOP_SIMD_HAVE_AVX2)
const qsop_simd_vtable_t *qsop_simd_avx2_vtable(void) {
  return NULL;
}
#endif

#if !defined(QSOP_SIMD_HAVE_AVX512)
const qsop_simd_vtable_t *qsop_simd_avx512_vtable(void) {
  return NULL;
}
#endif

/* Compiling a kernel in says nothing about whether *this* CPU can execute it. A binary built with
 * -Dsimd=avx512 (or -Dsimd=auto on a machine that has it) used to call straight into zmm code and
 * die with SIGILL on any older host -- there was no runtime check anywhere. Everything below
 * exists so that the answer to "which kernel do we call?" depends on the CPU we are running on,
 * not the CPU we were built on. The feature strings must match the -m flags meson compiles each
 * translation unit with. */
#if defined(QSOP_SIMD_HAVE_AVX512)
static bool cpu_supports_avx512(void) {
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq") &&
         __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vpopcntdq");
}
#endif

#if defined(QSOP_SIMD_HAVE_AVX2)
static bool cpu_supports_avx2(void) {
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
}
#endif

static bool kernel_available(qsop_simd_kernel_t kernel) {
  switch (kernel) {
  case QSOP_SIMD_KERNEL_SCALAR:
    return true;
  case QSOP_SIMD_KERNEL_NEON:
    /* NEON is architecturally guaranteed on aarch64, and simd_neon.c only compiles there. */
    return qsop_simd_neon_vtable() != NULL;
  case QSOP_SIMD_KERNEL_AVX2:
#if defined(QSOP_SIMD_HAVE_AVX2)
    return cpu_supports_avx2();
#else
    return false;
#endif
  case QSOP_SIMD_KERNEL_AVX512:
#if defined(QSOP_SIMD_HAVE_AVX512)
    return cpu_supports_avx512();
#else
    return false;
#endif
  case QSOP_SIMD_KERNEL_AUTO:
  default:
    return false;
  }
}

static const qsop_simd_vtable_t *vtable_for(qsop_simd_kernel_t kernel) {
  switch (kernel) {
  case QSOP_SIMD_KERNEL_AVX512:
    return qsop_simd_avx512_vtable();
  case QSOP_SIMD_KERNEL_AVX2:
    return qsop_simd_avx2_vtable();
  case QSOP_SIMD_KERNEL_NEON:
    return qsop_simd_neon_vtable();
  case QSOP_SIMD_KERNEL_SCALAR:
    return qsop_simd_scalar_vtable();
  case QSOP_SIMD_KERNEL_AUTO:
  default:
    return NULL;
  }
}

qsop_simd_kernel_t qsop_simd_runtime_kernel(void) {
  static const qsop_simd_kernel_t by_preference[] = {
      QSOP_SIMD_KERNEL_AVX512,
      QSOP_SIMD_KERNEL_AVX2,
      QSOP_SIMD_KERNEL_NEON,
  };
  for (size_t i = 0; i < sizeof(by_preference) / sizeof(by_preference[0]); i++) {
    if (kernel_available(by_preference[i])) {
      return by_preference[i];
    }
  }
  return QSOP_SIMD_KERNEL_SCALAR;
}

const char *qsop_simd_compiled_arch(void) {
  return
#if defined(QSOP_SIMD_HAVE_AVX512)
      "avx512,"
#endif
#if defined(QSOP_SIMD_HAVE_AVX2)
      "avx2,"
#endif
#if defined(QSOP_SIMD_HAVE_NEON)
      "neon,"
#endif
      "scalar";
}

const qsop_simd_vtable_t *qsop_simd_resolve(qsop_simd_kernel_t requested) {
  const qsop_simd_kernel_t kernel =
      requested == QSOP_SIMD_KERNEL_AUTO ? qsop_simd_runtime_kernel() : requested;
  return kernel_available(kernel) ? vtable_for(kernel) : NULL;
}

const char *qsop_simd_kernel_name(const qsop_simd_vtable_t *vt) {
  return vt == NULL || vt->name == NULL ? "unavailable" : vt->name;
}
