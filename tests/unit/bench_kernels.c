#define _POSIX_C_SOURCE 200809L

#include "dlx4sop/simd.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile uint64_t bench_sink_u64;
static volatile double bench_sink_f64;

static uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13U;
  x ^= x >> 7U;
  x ^= x << 17U;
  *state = x;
  return x;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static bool parse_size_arg(const char *flag, const char *text, size_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    fprintf(stderr, "error: %s requires a non-negative integer\n", flag);
    return false;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    fprintf(stderr, "error: %s requires a non-negative integer\n", flag);
    return false;
  }
  *out = (size_t)value;
  return true;
}

static bool parse_simd_kernel(const char *text, qsop_simd_kernel_t *out) {
  if (strcmp(text, "auto") == 0) {
    *out = QSOP_SIMD_KERNEL_AUTO;
    return true;
  }
  if (strcmp(text, "scalar") == 0) {
    *out = QSOP_SIMD_KERNEL_SCALAR;
    return true;
  }
  if (strcmp(text, "neon") == 0) {
    *out = QSOP_SIMD_KERNEL_NEON;
    return true;
  }
  if (strcmp(text, "avx512") == 0) {
    *out = QSOP_SIMD_KERNEL_AVX512;
    return true;
  }
  fprintf(stderr, "error: --simd requires auto|scalar|neon|avx512\n");
  return false;
}

static void usage(FILE *file, const char *argv0) {
  fprintf(file,
          "usage: %s [--simd auto|scalar|neon|avx512] [--words N] [--items N] "
          "[--iterations N] [--quick]\n",
          argv0);
}

static void fill_u64(uint64_t *values, size_t n, uint64_t seed) {
  uint64_t state = seed;
  for (size_t i = 0; i < n; i++) {
    values[i] = xorshift64(&state);
  }
}

static void fill_f64(double *values, size_t n, uint64_t seed) {
  uint64_t state = seed;
  for (size_t i = 0; i < n; i++) {
    values[i] = (double)(xorshift64(&state) & UINT64_C(0xfffff)) / 1048576.0;
  }
}

static void emit_u64_record(const char *kernel, const char *simd, size_t n,
                            size_t iterations, uint64_t ns, uint64_t checksum) {
  printf("{\"schema\":\"dlx4sop_kernel_bench_v1\",\"kernel\":\"%s\","
         "\"simd\":\"%s\",\"n\":%zu,\"iterations\":%zu,\"ns\":%" PRIu64
         ",\"checksum\":%" PRIu64 "}\n",
         kernel, simd, n, iterations, ns, checksum);
}

static void emit_f64_record(const char *kernel, const char *simd, size_t n,
                            size_t iterations, uint64_t ns, double checksum) {
  printf("{\"schema\":\"dlx4sop_kernel_bench_v1\",\"kernel\":\"%s\","
         "\"simd\":\"%s\",\"n\":%zu,\"iterations\":%zu,\"ns\":%" PRIu64
         ",\"checksum\":%.17g}\n",
         kernel, simd, n, iterations, ns, checksum);
}

static void bench_popcount_and(const qsop_simd_vtable_t *vt, const char *simd,
                               const uint64_t *a, const uint64_t *b,
                               size_t words, size_t iterations) {
  uint64_t checksum = 0;
  const uint64_t start = now_ns();
  for (size_t i = 0; i < iterations; i++) {
    checksum += vt->popcount_and_u64(a, b, words);
  }
  const uint64_t elapsed = now_ns() - start;
  bench_sink_u64 = checksum;
  emit_u64_record("bitset.popcount_and", simd, words, iterations, elapsed, checksum);
}

static void bench_row_xor(const qsop_simd_vtable_t *vt, const char *simd,
                          uint64_t *dst, const uint64_t *src, size_t words,
                          size_t iterations) {
  uint64_t checksum = 0;
  const uint64_t start = now_ns();
  for (size_t i = 0; i < iterations; i++) {
    vt->xor_u64(dst, src, words);
    checksum ^= dst[i % words];
  }
  const uint64_t elapsed = now_ns() - start;
  bench_sink_u64 = checksum;
  emit_u64_record("bitset.xor_row", simd, words, iterations, elapsed, checksum);
}

static void bench_complex_mul(const qsop_simd_vtable_t *vt, const char *simd,
                              double *out_re, double *out_im,
                              const double *left_re, const double *left_im,
                              const double *right_re, const double *right_im,
                              size_t n, size_t iterations) {
  double checksum = 0.0;
  const uint64_t start = now_ns();
  for (size_t i = 0; i < iterations; i++) {
    vt->complex_mul_assign_f64(out_re, out_im, left_re, left_im, right_re, right_im, n);
    checksum += out_re[i % n] + out_im[(i * 17U) % n];
  }
  const uint64_t elapsed = now_ns() - start;
  bench_sink_f64 = checksum;
  emit_f64_record("complex.mul_assign_f64", simd, n, iterations, elapsed, checksum);
}

static void bench_complex_sum_out(const qsop_simd_vtable_t *vt, const char *simd,
                                  double *out_re, double *out_im,
                                  const double *in_re, const double *in_im,
                                  size_t pairs, size_t iterations) {
  double checksum = 0.0;
  const uint64_t start = now_ns();
  for (size_t i = 0; i < iterations; i++) {
    vt->complex_sum_out_pairs_f64(out_re, out_im, in_re, in_im, pairs);
    checksum += out_re[i % pairs] + out_im[(i * 17U) % pairs];
  }
  const uint64_t elapsed = now_ns() - start;
  bench_sink_f64 = checksum;
  emit_f64_record("complex.sum_out_pairs_f64", simd, pairs, iterations, elapsed, checksum);
}

int main(int argc, char **argv) {
  qsop_simd_kernel_t requested = QSOP_SIMD_KERNEL_AUTO;
  size_t words = 4096;
  size_t items = 65536;
  size_t iterations = 1000;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout, argv[0]);
      return 0;
    }
    if (strcmp(argv[i], "--simd") == 0) {
      if (++i >= argc || !parse_simd_kernel(argv[i], &requested)) {
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--words") == 0) {
      if (++i >= argc || !parse_size_arg("--words", argv[i], &words)) {
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--items") == 0) {
      if (++i >= argc || !parse_size_arg("--items", argv[i], &items)) {
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--iterations") == 0) {
      if (++i >= argc || !parse_size_arg("--iterations", argv[i], &iterations)) {
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--quick") == 0) {
      words = 256;
      items = 4096;
      iterations = 20;
      continue;
    }
    fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
    usage(stderr, argv[0]);
    return 2;
  }

  if (words == 0 || items == 0 || iterations == 0) {
    fprintf(stderr, "error: --words, --items, and --iterations must be positive\n");
    return 2;
  }
  if (items > (SIZE_MAX / 2U)) {
    fprintf(stderr, "error: --items is too large\n");
    return 2;
  }

  const qsop_simd_vtable_t *vt = qsop_simd_resolve(requested);
  if (vt == NULL) {
    fprintf(stderr, "error: requested SIMD kernel is not available\n");
    return 1;
  }
  const char *simd = qsop_simd_kernel_name(vt);

  uint64_t *a = malloc(words * sizeof(*a));
  uint64_t *b = malloc(words * sizeof(*b));
  uint64_t *dst = malloc(words * sizeof(*dst));
  double *left_re = malloc(items * sizeof(*left_re));
  double *left_im = malloc(items * sizeof(*left_im));
  double *right_re = malloc(items * sizeof(*right_re));
  double *right_im = malloc(items * sizeof(*right_im));
  double *out_re = malloc(items * sizeof(*out_re));
  double *out_im = malloc(items * sizeof(*out_im));
  double *sum_in_re = malloc(2U * items * sizeof(*sum_in_re));
  double *sum_in_im = malloc(2U * items * sizeof(*sum_in_im));
  if (a == NULL || b == NULL || dst == NULL || left_re == NULL || left_im == NULL ||
      right_re == NULL || right_im == NULL || out_re == NULL || out_im == NULL ||
      sum_in_re == NULL || sum_in_im == NULL) {
    fprintf(stderr, "error: out of memory while allocating benchmark buffers\n");
    free(a);
    free(b);
    free(dst);
    free(left_re);
    free(left_im);
    free(right_re);
    free(right_im);
    free(out_re);
    free(out_im);
    free(sum_in_re);
    free(sum_in_im);
    return 1;
  }

  fill_u64(a, words, UINT64_C(0x123456789abcdef0));
  fill_u64(b, words, UINT64_C(0xfedcba9876543210));
  memcpy(dst, a, words * sizeof(*dst));
  fill_f64(left_re, items, UINT64_C(0x1111111111111111));
  fill_f64(left_im, items, UINT64_C(0x2222222222222222));
  fill_f64(right_re, items, UINT64_C(0x3333333333333333));
  fill_f64(right_im, items, UINT64_C(0x4444444444444444));
  fill_f64(sum_in_re, 2U * items, UINT64_C(0x5555555555555555));
  fill_f64(sum_in_im, 2U * items, UINT64_C(0x6666666666666666));

  bench_popcount_and(vt, simd, a, b, words, iterations);
  bench_row_xor(vt, simd, dst, b, words, iterations);
  bench_complex_mul(vt, simd, out_re, out_im, left_re, left_im, right_re, right_im,
                    items, iterations);
  bench_complex_sum_out(vt, simd, out_re, out_im, sum_in_re, sum_in_im, items,
                        iterations);

  free(a);
  free(b);
  free(dst);
  free(left_re);
  free(left_im);
  free(right_re);
  free(right_im);
  free(out_re);
  free(out_im);
  free(sum_in_re);
  free(sum_in_im);
  return 0;
}
