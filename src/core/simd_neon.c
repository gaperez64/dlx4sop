#include "dlx4sop/simd.h"

#include "dlx4sop/bitset.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>

static uint32_t neon_popcount_u64x2(uint64x2_t value) {
  const uint8x16_t bytes = vreinterpretq_u8_u64(value);
  const uint8x16_t cnt8 = vcntq_u8(bytes);
  const uint16x8_t cnt16 = vpaddlq_u8(cnt8);
  const uint32x4_t cnt32 = vpaddlq_u16(cnt16);
  const uint64x2_t cnt64 = vpaddlq_u32(cnt32);
  return (uint32_t)(vgetq_lane_u64(cnt64, 0) + vgetq_lane_u64(cnt64, 1));
}

static uint32_t simd_neon_popcount_and_u64(const uint64_t *a, const uint64_t *b,
                                           size_t words) {
  uint32_t count = 0;
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    const uint64x2_t av = vld1q_u64(a + w);
    const uint64x2_t bv = vld1q_u64(b + w);
    count += neon_popcount_u64x2(vandq_u64(av, bv));
  }
  for (; w < words; w++) {
    count += qsop_popcount_u64(a[w] & b[w]);
  }
  return count;
}

static uint32_t simd_neon_popcount_andnot_u64(const uint64_t *a, const uint64_t *b,
                                              size_t words) {
  uint32_t count = 0;
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    const uint64x2_t av = vld1q_u64(a + w);
    const uint64x2_t bv = vld1q_u64(b + w);
    count += neon_popcount_u64x2(vbicq_u64(av, bv));
  }
  for (; w < words; w++) {
    count += qsop_popcount_u64(a[w] & ~b[w]);
  }
  return count;
}

static void simd_neon_xor_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    vst1q_u64(dst + w, veorq_u64(vld1q_u64(dst + w), vld1q_u64(src + w)));
  }
  for (; w < words; w++) {
    dst[w] ^= src[w];
  }
}

static void simd_neon_or_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    vst1q_u64(dst + w, vorrq_u64(vld1q_u64(dst + w), vld1q_u64(src + w)));
  }
  for (; w < words; w++) {
    dst[w] |= src[w];
  }
}

static void simd_neon_and_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    vst1q_u64(dst + w, vandq_u64(vld1q_u64(dst + w), vld1q_u64(src + w)));
  }
  for (; w < words; w++) {
    dst[w] &= src[w];
  }
}

static void simd_neon_andnot_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    vst1q_u64(dst + w, vbicq_u64(vld1q_u64(dst + w), vld1q_u64(src + w)));
  }
  for (; w < words; w++) {
    dst[w] &= ~src[w];
  }
}

static void simd_neon_complex_mul_assign_f64(double *restrict out_re,
                                             double *restrict out_im,
                                             const double *restrict left_re,
                                             const double *restrict left_im,
                                             const double *restrict right_re,
                                             const double *restrict right_im,
                                             size_t n) {
  size_t i = 0;
  for (; i + 2U <= n; i += 2U) {
    const float64x2_t lre = vld1q_f64(left_re + i);
    const float64x2_t lim = vld1q_f64(left_im + i);
    const float64x2_t rre = vld1q_f64(right_re + i);
    const float64x2_t rim = vld1q_f64(right_im + i);
    vst1q_f64(out_re + i, vsubq_f64(vmulq_f64(lre, rre), vmulq_f64(lim, rim)));
    vst1q_f64(out_im + i, vaddq_f64(vmulq_f64(lre, rim), vmulq_f64(lim, rre)));
  }
  for (; i < n; i++) {
    const double lre = left_re[i];
    const double lim = left_im[i];
    const double rre = right_re[i];
    const double rim = right_im[i];
    out_re[i] = lre * rre - lim * rim;
    out_im[i] = lre * rim + lim * rre;
  }
}

static void simd_neon_complex_sum_out_pairs_f64(double *restrict out_re,
                                                double *restrict out_im,
                                                const double *restrict in_re,
                                                const double *restrict in_im,
                                                size_t pairs) {
  size_t i = 0;
  for (; i + 2U <= pairs; i += 2U) {
    vst1q_f64(out_re + i, vaddq_f64(vld1q_f64(in_re + i), vld1q_f64(in_re + pairs + i)));
    vst1q_f64(out_im + i, vaddq_f64(vld1q_f64(in_im + i), vld1q_f64(in_im + pairs + i)));
  }
  for (; i < pairs; i++) {
    out_re[i] = in_re[i] + in_re[pairs + i];
    out_im[i] = in_im[i] + in_im[pairs + i];
  }
}

const qsop_simd_vtable_t *qsop_simd_neon_vtable_if_available(void) {
  static const qsop_simd_vtable_t vt = {
      .name = "neon",
      .popcount_and_u64 = simd_neon_popcount_and_u64,
      .popcount_andnot_u64 = simd_neon_popcount_andnot_u64,
      .xor_u64 = simd_neon_xor_u64,
      .or_u64 = simd_neon_or_u64,
      .and_u64 = simd_neon_and_u64,
      .andnot_u64 = simd_neon_andnot_u64,
      .complex_mul_assign_f64 = simd_neon_complex_mul_assign_f64,
      .complex_sum_out_pairs_f64 = simd_neon_complex_sum_out_pairs_f64,
  };
  return &vt;
}
#else
const qsop_simd_vtable_t *qsop_simd_neon_vtable_if_available(void) {
  return NULL;
}
#endif
