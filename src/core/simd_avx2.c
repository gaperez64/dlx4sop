#include "dlx4sop/simd.h"

#include "dlx4sop/bitset.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

/* AVX2 has no vpopcntq (that is AVX-512 VPOPCNTDQ), so the widened AND/ANDNOT is spilled to the
 * stack and counted scalar-side. Still worth vectorizing the mask half. */
static uint32_t avx2_popcount_of(__m256i value) {
  uint64_t lanes[4];
  _mm256_storeu_si256((__m256i *)lanes, value);
  return qsop_popcount_u64(lanes[0]) + qsop_popcount_u64(lanes[1]) + qsop_popcount_u64(lanes[2]) +
         qsop_popcount_u64(lanes[3]);
}

static uint32_t simd_avx2_popcount_and_u64(const uint64_t *a, const uint64_t *b, size_t words) {
  uint32_t count = 0;
  size_t w = 0;
  for (; w + 4U <= words; w += 4U) {
    const __m256i av = _mm256_loadu_si256((const __m256i *)(a + w));
    const __m256i bv = _mm256_loadu_si256((const __m256i *)(b + w));
    count += avx2_popcount_of(_mm256_and_si256(av, bv));
  }
  for (; w < words; w++) {
    count += qsop_popcount_u64(a[w] & b[w]);
  }
  return count;
}

static uint32_t simd_avx2_popcount_andnot_u64(const uint64_t *a, const uint64_t *b, size_t words) {
  uint32_t count = 0;
  size_t w = 0;
  for (; w + 4U <= words; w += 4U) {
    const __m256i av = _mm256_loadu_si256((const __m256i *)(a + w));
    const __m256i bv = _mm256_loadu_si256((const __m256i *)(b + w));
    /* _mm256_andnot_si256(x, y) computes (~x) & y */
    count += avx2_popcount_of(_mm256_andnot_si256(bv, av));
  }
  for (; w < words; w++) {
    count += qsop_popcount_u64(a[w] & ~b[w]);
  }
  return count;
}

static void simd_avx2_xor_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 4U <= words; w += 4U) {
    const __m256i dv = _mm256_loadu_si256((const __m256i *)(dst + w));
    const __m256i sv = _mm256_loadu_si256((const __m256i *)(src + w));
    _mm256_storeu_si256((__m256i *)(dst + w), _mm256_xor_si256(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] ^= src[w];
  }
}

static void simd_avx2_or_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 4U <= words; w += 4U) {
    const __m256i dv = _mm256_loadu_si256((const __m256i *)(dst + w));
    const __m256i sv = _mm256_loadu_si256((const __m256i *)(src + w));
    _mm256_storeu_si256((__m256i *)(dst + w), _mm256_or_si256(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] |= src[w];
  }
}

static void simd_avx2_and_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 4U <= words; w += 4U) {
    const __m256i dv = _mm256_loadu_si256((const __m256i *)(dst + w));
    const __m256i sv = _mm256_loadu_si256((const __m256i *)(src + w));
    _mm256_storeu_si256((__m256i *)(dst + w), _mm256_and_si256(dv, sv));
  }
  for (; w < words; w++) {
    dst[w] &= src[w];
  }
}

static void simd_avx2_andnot_u64(uint64_t *dst, const uint64_t *src, size_t words) {
  size_t w = 0;
  for (; w + 4U <= words; w += 4U) {
    const __m256i dv = _mm256_loadu_si256((const __m256i *)(dst + w));
    const __m256i sv = _mm256_loadu_si256((const __m256i *)(src + w));
    _mm256_storeu_si256((__m256i *)(dst + w), _mm256_andnot_si256(sv, dv));
  }
  for (; w < words; w++) {
    dst[w] &= ~src[w];
  }
}

/* Deliberately mul-then-add/sub rather than fma: the AVX-512 and scalar kernels do the same, and
 * an fma here would silently change the last bit relative to them. That also keeps this
 * translation unit at -mavx2, so the runtime guard only has to check avx2. */
static void simd_avx2_complex_mul_assign_f64(double *restrict out_re, double *restrict out_im,
                                             const double *restrict left_re,
                                             const double *restrict left_im,
                                             const double *restrict right_re,
                                             const double *restrict right_im, size_t n) {
  size_t i = 0;
  for (; i + 4U <= n; i += 4U) {
    const __m256d lre = _mm256_loadu_pd(left_re + i);
    const __m256d lim = _mm256_loadu_pd(left_im + i);
    const __m256d rre = _mm256_loadu_pd(right_re + i);
    const __m256d rim = _mm256_loadu_pd(right_im + i);
    _mm256_storeu_pd(out_re + i, _mm256_sub_pd(_mm256_mul_pd(lre, rre), _mm256_mul_pd(lim, rim)));
    _mm256_storeu_pd(out_im + i, _mm256_add_pd(_mm256_mul_pd(lre, rim), _mm256_mul_pd(lim, rre)));
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

static void simd_avx2_complex_scale_f64(double *restrict out_re, double *restrict out_im,
                                        const double *restrict in_re, const double *restrict in_im,
                                        double scale_re, double scale_im, size_t n) {
  const __m256d sre = _mm256_set1_pd(scale_re);
  const __m256d sim = _mm256_set1_pd(scale_im);
  size_t i = 0;
  for (; i + 4U <= n; i += 4U) {
    const __m256d re = _mm256_loadu_pd(in_re + i);
    const __m256d im = _mm256_loadu_pd(in_im + i);
    _mm256_storeu_pd(out_re + i, _mm256_sub_pd(_mm256_mul_pd(re, sre), _mm256_mul_pd(im, sim)));
    _mm256_storeu_pd(out_im + i, _mm256_add_pd(_mm256_mul_pd(re, sim), _mm256_mul_pd(im, sre)));
  }
  for (; i < n; i++) {
    const double re = in_re[i];
    const double im = in_im[i];
    out_re[i] = re * scale_re - im * scale_im;
    out_im[i] = re * scale_im + im * scale_re;
  }
}

static void simd_avx2_complex_sum_out_pairs_f64(double *restrict out_re, double *restrict out_im,
                                                const double *restrict in_re,
                                                const double *restrict in_im, size_t pairs) {
  size_t i = 0;
  for (; i + 4U <= pairs; i += 4U) {
    _mm256_storeu_pd(out_re + i,
                     _mm256_add_pd(_mm256_loadu_pd(in_re + i), _mm256_loadu_pd(in_re + pairs + i)));
    _mm256_storeu_pd(out_im + i,
                     _mm256_add_pd(_mm256_loadu_pd(in_im + i), _mm256_loadu_pd(in_im + pairs + i)));
  }
  for (; i < pairs; i++) {
    out_re[i] = in_re[i] + in_re[pairs + i];
    out_im[i] = in_im[i] + in_im[pairs + i];
  }
}

const qsop_simd_vtable_t *qsop_simd_avx2_vtable(void) {
  static const qsop_simd_vtable_t vt = {
      .name = "avx2",
      .min_lanes = 4,
      .popcount_and_u64 = simd_avx2_popcount_and_u64,
      .popcount_andnot_u64 = simd_avx2_popcount_andnot_u64,
      .xor_u64 = simd_avx2_xor_u64,
      .or_u64 = simd_avx2_or_u64,
      .and_u64 = simd_avx2_and_u64,
      .andnot_u64 = simd_avx2_andnot_u64,
      .complex_mul_assign_f64 = simd_avx2_complex_mul_assign_f64,
      .complex_scale_f64 = simd_avx2_complex_scale_f64,
      .complex_sum_out_pairs_f64 = simd_avx2_complex_sum_out_pairs_f64,
  };
  return &vt;
}
#else
const qsop_simd_vtable_t *qsop_simd_avx2_vtable(void) {
  return NULL;
}
#endif
