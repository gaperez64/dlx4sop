#include "dlx4sop/simd.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

static uint32_t sse_popcount_u64(uint64_t value) {
#if defined(__x86_64__)
  return (uint32_t)_mm_popcnt_u64((unsigned long long)value);
#else
  return (uint32_t)_mm_popcnt_u32((uint32_t)value) +
         (uint32_t)_mm_popcnt_u32((uint32_t)(value >> 32U));
#endif
}

static uint32_t simd_sse_popcount_and_u64(const uint64_t *a, const uint64_t *b,
                                          size_t words) {
  uint32_t count = 0;
  for (size_t w = 0; w < words; w++) {
    count += sse_popcount_u64(a[w] & b[w]);
  }
  return count;
}

static uint32_t simd_sse_popcount_andnot_u64(const uint64_t *a, const uint64_t *b,
                                             size_t words) {
  uint32_t count = 0;
  for (size_t w = 0; w < words; w++) {
    count += sse_popcount_u64(a[w] & ~b[w]);
  }
  return count;
}

static void simd_sse_xor_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    const __m128i dv = _mm_loadu_si128((const __m128i *)(const void *)(dst + w));
    const __m128i sv = _mm_loadu_si128((const __m128i *)(const void *)(src + w));
    _mm_storeu_si128((__m128i *)(void *)(dst + w), _mm_xor_si128(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] ^= src[w];
  }
}

static void simd_sse_or_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    const __m128i dv = _mm_loadu_si128((const __m128i *)(const void *)(dst + w));
    const __m128i sv = _mm_loadu_si128((const __m128i *)(const void *)(src + w));
    _mm_storeu_si128((__m128i *)(void *)(dst + w), _mm_or_si128(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] |= src[w];
  }
}

static void simd_sse_and_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    const __m128i dv = _mm_loadu_si128((const __m128i *)(const void *)(dst + w));
    const __m128i sv = _mm_loadu_si128((const __m128i *)(const void *)(src + w));
    _mm_storeu_si128((__m128i *)(void *)(dst + w), _mm_and_si128(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] &= src[w];
  }
}

static void simd_sse_andnot_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 2U <= words; w += 2U) {
    const __m128i dv = _mm_loadu_si128((const __m128i *)(const void *)(dst + w));
    const __m128i sv = _mm_loadu_si128((const __m128i *)(const void *)(src + w));
    _mm_storeu_si128((__m128i *)(void *)(dst + w), _mm_andnot_si128(sv, dv));
  }
  for (; w < words; w++) {
    dst[w] &= ~src[w];
  }
}

static void simd_sse_complex_mul_assign_f64(double *restrict out_re,
                                            double *restrict out_im,
                                            const double *restrict left_re,
                                            const double *restrict left_im,
                                            const double *restrict right_re,
                                            const double *restrict right_im,
                                            size_t n) {
  size_t i = 0;
  for (; i + 2U <= n; i += 2U) {
    const __m128d lre = _mm_loadu_pd(left_re + i);
    const __m128d lim = _mm_loadu_pd(left_im + i);
    const __m128d rre = _mm_loadu_pd(right_re + i);
    const __m128d rim = _mm_loadu_pd(right_im + i);
    _mm_storeu_pd(out_re + i, _mm_sub_pd(_mm_mul_pd(lre, rre), _mm_mul_pd(lim, rim)));
    _mm_storeu_pd(out_im + i, _mm_add_pd(_mm_mul_pd(lre, rim), _mm_mul_pd(lim, rre)));
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

static void simd_sse_complex_sum_out_pairs_f64(double *restrict out_re,
                                               double *restrict out_im,
                                               const double *restrict in_re,
                                               const double *restrict in_im,
                                               size_t pairs) {
  size_t i = 0;
  for (; i + 2U <= pairs; i += 2U) {
    _mm_storeu_pd(out_re + i,
                  _mm_add_pd(_mm_loadu_pd(in_re + i), _mm_loadu_pd(in_re + pairs + i)));
    _mm_storeu_pd(out_im + i,
                  _mm_add_pd(_mm_loadu_pd(in_im + i), _mm_loadu_pd(in_im + pairs + i)));
  }
  for (; i < pairs; i++) {
    out_re[i] = in_re[i] + in_re[pairs + i];
    out_im[i] = in_im[i] + in_im[pairs + i];
  }
}

const qsop_simd_vtable_t *qsop_simd_sse_vtable(void) {
  static const qsop_simd_vtable_t vt = {
      .name = "sse",
      .popcount_and_u64 = simd_sse_popcount_and_u64,
      .popcount_andnot_u64 = simd_sse_popcount_andnot_u64,
      .xor_u64 = simd_sse_xor_u64,
      .or_u64 = simd_sse_or_u64,
      .and_u64 = simd_sse_and_u64,
      .andnot_u64 = simd_sse_andnot_u64,
      .complex_mul_assign_f64 = simd_sse_complex_mul_assign_f64,
      .complex_sum_out_pairs_f64 = simd_sse_complex_sum_out_pairs_f64,
  };
  return &vt;
}
#else
const qsop_simd_vtable_t *qsop_simd_sse_vtable(void) {
  return NULL;
}
#endif
