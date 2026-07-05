#ifndef DLX4SOP_SIMD_H
#define DLX4SOP_SIMD_H

#include <stddef.h>
#include <stdint.h>

typedef enum qsop_simd_kernel {
  QSOP_SIMD_KERNEL_AUTO = 0,
  QSOP_SIMD_KERNEL_SCALAR,
  QSOP_SIMD_KERNEL_NEON,
  QSOP_SIMD_KERNEL_AVX512,
} qsop_simd_kernel_t;

typedef struct qsop_simd_vtable {
  const char *name;

  uint32_t (*popcount_and_u64)(const uint64_t *a, const uint64_t *b, size_t words);
  uint32_t (*popcount_andnot_u64)(const uint64_t *a, const uint64_t *b, size_t words);

  void (*xor_u64)(uint64_t *dst, const uint64_t *src, size_t words);
  void (*or_u64)(uint64_t *dst, const uint64_t *src, size_t words);
  void (*and_u64)(uint64_t *dst, const uint64_t *src, size_t words);
  void (*andnot_u64)(uint64_t *dst, const uint64_t *src, size_t words);

  void (*complex_mul_assign_f64)(double *restrict out_re, double *restrict out_im,
                                 const double *restrict left_re,
                                 const double *restrict left_im,
                                 const double *restrict right_re,
                                 const double *restrict right_im,
                                 size_t n);

  void (*complex_sum_out_pairs_f64)(double *restrict out_re, double *restrict out_im,
                                    const double *restrict in_re,
                                    const double *restrict in_im,
                                    size_t pairs);
} qsop_simd_vtable_t;

const qsop_simd_vtable_t *qsop_simd_resolve(qsop_simd_kernel_t requested);
const char *qsop_simd_kernel_name(const qsop_simd_vtable_t *vt);

const qsop_simd_vtable_t *qsop_simd_scalar_vtable(void);
const qsop_simd_vtable_t *qsop_simd_neon_vtable_if_available(void);
const qsop_simd_vtable_t *qsop_simd_avx512_vtable_if_available(void);

#endif
