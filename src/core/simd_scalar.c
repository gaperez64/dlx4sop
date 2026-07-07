#include "dlx4sop/simd.h"

#include "dlx4sop/bitset.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t simd_scalar_popcount_and_u64(const uint64_t *a, const uint64_t *b,
                                             size_t words) {
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

static void simd_scalar_complex_mul_assign_f64(double *restrict out_re,
                                               double *restrict out_im,
                                               const double *restrict left_re,
                                               const double *restrict left_im,
                                               const double *restrict right_re,
                                               const double *restrict right_im,
                                               size_t n) {
  for (size_t i = 0; i < n; i++) {
    const double lre = left_re[i];
    const double lim = left_im[i];
    const double rre = right_re[i];
    const double rim = right_im[i];
    out_re[i] = lre * rre - lim * rim;
    out_im[i] = lre * rim + lim * rre;
  }
}

static void simd_scalar_complex_sum_out_pairs_f64(double *restrict out_re,
                                                  double *restrict out_im,
                                                  const double *restrict in_re,
                                                  const double *restrict in_im,
                                                  size_t pairs) {
  for (size_t i = 0; i < pairs; i++) {
    out_re[i] = in_re[i] + in_re[pairs + i];
    out_im[i] = in_im[i] + in_im[pairs + i];
  }
}

const qsop_simd_vtable_t *qsop_simd_scalar_vtable(void) {
  static const qsop_simd_vtable_t vt = {
      .name = "scalar",
      .popcount_and_u64 = simd_scalar_popcount_and_u64,
      .popcount_andnot_u64 = simd_scalar_popcount_andnot_u64,
      .xor_u64 = simd_scalar_xor_u64,
      .or_u64 = simd_scalar_or_u64,
      .and_u64 = simd_scalar_and_u64,
      .andnot_u64 = simd_scalar_andnot_u64,
      .complex_mul_assign_f64 = simd_scalar_complex_mul_assign_f64,
      .complex_sum_out_pairs_f64 = simd_scalar_complex_sum_out_pairs_f64,
  };
  return &vt;
}

__attribute__((weak)) const qsop_simd_vtable_t *qsop_simd_sse_vtable(void) {
  return NULL;
}

__attribute__((weak)) const qsop_simd_vtable_t *qsop_simd_neon_vtable(void) {
  return NULL;
}

__attribute__((weak)) const qsop_simd_vtable_t *qsop_simd_avx512_vtable(void) {
  return NULL;
}

qsop_simd_kernel_t qsop_simd_compiled_kernel(void) {
#if defined(QSOP_SIMD_ARCH_AVX512)
  return QSOP_SIMD_KERNEL_AVX512;
#elif defined(QSOP_SIMD_ARCH_SSE)
  return QSOP_SIMD_KERNEL_SSE;
#elif defined(QSOP_SIMD_ARCH_NEON)
  return QSOP_SIMD_KERNEL_NEON;
#else
  return QSOP_SIMD_KERNEL_SCALAR;
#endif
}

const char *qsop_simd_compiled_arch(void) {
  return qsop_simd_kernel_name(qsop_simd_resolve(qsop_simd_compiled_kernel()));
}

static const qsop_simd_vtable_t *compiled_simd_vtable(void) {
#if defined(QSOP_SIMD_ARCH_AVX512)
  const qsop_simd_vtable_t *vt = qsop_simd_avx512_vtable();
  return vt != NULL ? vt : qsop_simd_scalar_vtable();
#elif defined(QSOP_SIMD_ARCH_SSE)
  const qsop_simd_vtable_t *vt = qsop_simd_sse_vtable();
  return vt != NULL ? vt : qsop_simd_scalar_vtable();
#elif defined(QSOP_SIMD_ARCH_NEON)
  const qsop_simd_vtable_t *vt = qsop_simd_neon_vtable();
  return vt != NULL ? vt : qsop_simd_scalar_vtable();
#else
  return qsop_simd_scalar_vtable();
#endif
}

const qsop_simd_vtable_t *qsop_simd_resolve(qsop_simd_kernel_t requested) {
  if (requested == QSOP_SIMD_KERNEL_AUTO || requested == qsop_simd_compiled_kernel()) {
    return compiled_simd_vtable();
  }
  if (requested == QSOP_SIMD_KERNEL_SCALAR) {
    return qsop_simd_scalar_vtable();
  }
  return NULL;
}

const char *qsop_simd_kernel_name(const qsop_simd_vtable_t *vt) {
  return vt == NULL || vt->name == NULL ? "unavailable" : vt->name;
}
