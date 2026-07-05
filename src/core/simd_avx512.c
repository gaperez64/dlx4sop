#include "dlx4sop/simd.h"

#include "dlx4sop/bitset.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

static int avx512_cpu_available(void) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq") &&
         __builtin_cpu_supports("avx512vl");
#else
  return 0;
#endif
}

static uint32_t simd_avx512_popcount_and_u64(const uint64_t *a, const uint64_t *b,
                                             size_t words) {
  uint32_t count = 0;
  size_t w = 0;
#if defined(__AVX512VPOPCNTDQ__)
  for (; w + 8U <= words; w += 8U) {
    const __m512i av = _mm512_loadu_si512((const void *)(a + w));
    const __m512i bv = _mm512_loadu_si512((const void *)(b + w));
    const __m512i cnt = _mm512_popcnt_epi64(_mm512_and_si512(av, bv));
    count += (uint32_t)_mm512_reduce_add_epi64(cnt);
  }
#endif
  for (; w < words; w++) {
    count += qsop_popcount_u64(a[w] & b[w]);
  }
  return count;
}

static uint32_t simd_avx512_popcount_andnot_u64(const uint64_t *a, const uint64_t *b,
                                                size_t words) {
  uint32_t count = 0;
  size_t w = 0;
#if defined(__AVX512VPOPCNTDQ__)
  for (; w + 8U <= words; w += 8U) {
    const __m512i av = _mm512_loadu_si512((const void *)(a + w));
    const __m512i bv = _mm512_loadu_si512((const void *)(b + w));
    const __m512i cnt = _mm512_popcnt_epi64(_mm512_andnot_si512(bv, av));
    count += (uint32_t)_mm512_reduce_add_epi64(cnt);
  }
#endif
  for (; w < words; w++) {
    count += qsop_popcount_u64(a[w] & ~b[w]);
  }
  return count;
}

static void simd_avx512_xor_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 8U <= words; w += 8U) {
    const __m512i dv = _mm512_loadu_si512((const void *)(dst + w));
    const __m512i sv = _mm512_loadu_si512((const void *)(src + w));
    _mm512_storeu_si512((void *)(dst + w), _mm512_xor_si512(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] ^= src[w];
  }
}

static void simd_avx512_or_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 8U <= words; w += 8U) {
    const __m512i dv = _mm512_loadu_si512((const void *)(dst + w));
    const __m512i sv = _mm512_loadu_si512((const void *)(src + w));
    _mm512_storeu_si512((void *)(dst + w), _mm512_or_si512(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] |= src[w];
  }
}

static void simd_avx512_and_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 8U <= words; w += 8U) {
    const __m512i dv = _mm512_loadu_si512((const void *)(dst + w));
    const __m512i sv = _mm512_loadu_si512((const void *)(src + w));
    _mm512_storeu_si512((void *)(dst + w), _mm512_and_si512(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] &= src[w];
  }
}

static void simd_avx512_andnot_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 8U <= words; w += 8U) {
    const __m512i dv = _mm512_loadu_si512((const void *)(dst + w));
    const __m512i sv = _mm512_loadu_si512((const void *)(src + w));
    _mm512_storeu_si512((void *)(dst + w), _mm512_andnot_si512(sv, dv));
  }
  for (; w < words; w++) {
    dst[w] &= ~src[w];
  }
}

static void simd_avx512_complex_mul_assign_f64(double *restrict out_re,
                                               double *restrict out_im,
                                               const double *restrict left_re,
                                               const double *restrict left_im,
                                               const double *restrict right_re,
                                               const double *restrict right_im,
                                               size_t n) {
  size_t i = 0;
  for (; i + 8U <= n; i += 8U) {
    const __m512d lre = _mm512_loadu_pd(left_re + i);
    const __m512d lim = _mm512_loadu_pd(left_im + i);
    const __m512d rre = _mm512_loadu_pd(right_re + i);
    const __m512d rim = _mm512_loadu_pd(right_im + i);
    _mm512_storeu_pd(out_re + i,
                     _mm512_sub_pd(_mm512_mul_pd(lre, rre), _mm512_mul_pd(lim, rim)));
    _mm512_storeu_pd(out_im + i,
                     _mm512_add_pd(_mm512_mul_pd(lre, rim), _mm512_mul_pd(lim, rre)));
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

static void simd_avx512_complex_sum_out_pairs_f64(double *restrict out_re,
                                                  double *restrict out_im,
                                                  const double *restrict in_re,
                                                  const double *restrict in_im,
                                                  size_t pairs) {
  size_t i = 0;
  for (; i + 8U <= pairs; i += 8U) {
    _mm512_storeu_pd(out_re + i,
                     _mm512_add_pd(_mm512_loadu_pd(in_re + i),
                                   _mm512_loadu_pd(in_re + pairs + i)));
    _mm512_storeu_pd(out_im + i,
                     _mm512_add_pd(_mm512_loadu_pd(in_im + i),
                                   _mm512_loadu_pd(in_im + pairs + i)));
  }
  for (; i < pairs; i++) {
    out_re[i] = in_re[i] + in_re[pairs + i];
    out_im[i] = in_im[i] + in_im[pairs + i];
  }
}

const qsop_simd_vtable_t *qsop_simd_avx512_vtable_if_available(void) {
  if (!avx512_cpu_available()) {
    return NULL;
  }
  static const qsop_simd_vtable_t vt = {
      .name = "avx512",
      .popcount_and_u64 = simd_avx512_popcount_and_u64,
      .popcount_andnot_u64 = simd_avx512_popcount_andnot_u64,
      .xor_u64 = simd_avx512_xor_u64,
      .or_u64 = simd_avx512_or_u64,
      .and_u64 = simd_avx512_and_u64,
      .andnot_u64 = simd_avx512_andnot_u64,
      .complex_mul_assign_f64 = simd_avx512_complex_mul_assign_f64,
      .complex_sum_out_pairs_f64 = simd_avx512_complex_sum_out_pairs_f64,
  };
  return &vt;
}
#else
const qsop_simd_vtable_t *qsop_simd_avx512_vtable_if_available(void) {
  return NULL;
}
#endif
