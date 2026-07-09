#ifndef DLX4SOP_SIMD_H
#define DLX4SOP_SIMD_H

#include <stddef.h>
#include <stdint.h>

/* Numeric values are part of the --format stats output (simd_kernel / bitset_kernel), so new
 * kernels are appended rather than slotted in by capability order. */
typedef enum qsop_simd_kernel {
  QSOP_SIMD_KERNEL_AUTO = 0,
  QSOP_SIMD_KERNEL_SCALAR,
  QSOP_SIMD_KERNEL_NEON,
  QSOP_SIMD_KERNEL_AVX512,
  QSOP_SIMD_KERNEL_AVX2,
} qsop_simd_kernel_t;

typedef struct qsop_simd_vtable {
  const char *name;

  /* Doubles per vector. Call sites gate on this rather than a hard-coded 8: the guards were
   * written for AVX-512's 8-wide registers, so on a 4-wide AVX2 or 2-wide NEON kernel every run
   * of 4 to 7 elements fell to the scalar tail for no reason. */
  size_t min_lanes;

  uint32_t (*popcount_and_u64)(const uint64_t *a, const uint64_t *b, size_t words);
  uint32_t (*popcount_andnot_u64)(const uint64_t *a, const uint64_t *b, size_t words);

  void (*xor_u64)(uint64_t *dst, const uint64_t *src, size_t words);
  void (*or_u64)(uint64_t *dst, const uint64_t *src, size_t words);
  void (*and_u64)(uint64_t *dst, const uint64_t *src, size_t words);
  void (*andnot_u64)(uint64_t *dst, const uint64_t *src, size_t words);

  void (*complex_mul_assign_f64)(double *restrict out_re, double *restrict out_im,
                                 const double *restrict left_re, const double *restrict left_im,
                                 const double *restrict right_re, const double *restrict right_im,
                                 size_t n);

  /* out[i] = in[i] * (scale_re + i*scale_im). The DP's factor joins read one operand through a
   * bit-projection of the output assignment, and over the block of assignments that agree outside
   * that operand's variables it does not move at all -- so most of the join work is this, a
   * contiguous run scaled by one complex constant, rather than an elementwise multiply. */
  void (*complex_scale_f64)(double *restrict out_re, double *restrict out_im,
                            const double *restrict in_re, const double *restrict in_im,
                            double scale_re, double scale_im, size_t n);

  void (*complex_sum_out_pairs_f64)(double *restrict out_re, double *restrict out_im,
                                    const double *restrict in_re, const double *restrict in_im,
                                    size_t pairs);
} qsop_simd_vtable_t;

/* Kernels are selected at *run time*, not compile time: a binary may carry several and picks the
 * best one this CPU actually supports. QSOP_SIMD_KERNEL_AUTO always resolves (scalar is the
 * floor); an explicitly named kernel resolves to NULL when it is not compiled in or the CPU
 * lacks it, so callers refuse rather than execute an illegal instruction. */
const qsop_simd_vtable_t *qsop_simd_resolve(qsop_simd_kernel_t requested);
const char *qsop_simd_kernel_name(const qsop_simd_vtable_t *vt);

/* The best kernel this CPU can run, among those compiled in. */
qsop_simd_kernel_t qsop_simd_runtime_kernel(void);
/* Comma-separated list of the kernels compiled into this binary, best first (e.g.
 * "avx512,avx2,scalar"). Independent of what the current CPU supports. */
const char *qsop_simd_compiled_arch(void);

const qsop_simd_vtable_t *qsop_simd_scalar_vtable(void);
const qsop_simd_vtable_t *qsop_simd_neon_vtable(void);
const qsop_simd_vtable_t *qsop_simd_avx2_vtable(void);
const qsop_simd_vtable_t *qsop_simd_avx512_vtable(void);

#endif
