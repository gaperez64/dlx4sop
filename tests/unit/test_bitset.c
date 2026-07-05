#include "dlx4sop/bitset.h"
#include "dlx4sop/simd.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int expect_u32(const char *name, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %" PRIu32 " got %" PRIu32 "\n", name, expected, actual);
    return 1;
  }
  return 0;
}

static int expect_words(const char *name, const uint64_t *actual, const uint64_t *expected,
                        size_t words) {
  for (size_t w = 0; w < words; w++) {
    if (actual[w] != expected[w]) {
      fprintf(stderr, "%s[%" PRIu64 "]: expected 0x%016" PRIx64 " got 0x%016" PRIx64 "\n",
              name, (uint64_t)w, expected[w], actual[w]);
      return 1;
    }
  }
  return 0;
}

static int test_popcount_wrappers(const qsop_simd_vtable_t *simd, const char *name) {
  const uint64_t left[] = {
      UINT64_C(0xffff0000ffff0000),
      UINT64_C(0x0123456789abcdef),
      UINT64_C(0xf0f0f0f00f0f0f0f),
      UINT64_C(0xaaaaaaaa55555555),
      UINT64_C(0x8000000000000001),
  };
  const uint64_t right[] = {
      UINT64_C(0x0f0f0f0f0f0f0f0f),
      UINT64_C(0xfedcba9876543210),
      UINT64_C(0x00ff00ff00ff00ff),
      UINT64_C(0xffff0000ffff0000),
      UINT64_C(0x7ffffffffffffffe),
  };
  const size_t words = sizeof(left) / sizeof(left[0]);
  char label[96];

  snprintf(label, sizeof(label), "%s intersection", name);
  if (expect_u32(label, qsop_bitset_popcount_intersection_simd(left, right, words, simd),
                 qsop_bitset_popcount_intersection(left, right, words)) != 0) {
    return 1;
  }

  uint32_t expected_andnot = 0;
  for (size_t w = 0; w < words; w++) {
    expected_andnot += qsop_popcount_u64(left[w] & ~right[w]);
  }
  snprintf(label, sizeof(label), "%s andnot", name);
  return expect_u32(label, qsop_bitset_popcount_andnot_simd(left, right, words, simd),
                    expected_andnot);
}

static int test_row_wrappers(const qsop_simd_vtable_t *simd, const char *name) {
  const uint64_t src[] = {
      UINT64_C(0x1111111111111111),
      UINT64_C(0x2222222222222222),
      UINT64_C(0x4444444444444444),
      UINT64_C(0x8888888888888888),
      UINT64_C(0xffff0000ffff0000),
  };
  const size_t words = sizeof(src) / sizeof(src[0]);
  uint64_t row[5];
  uint64_t expected[5];
  char label[96];

  for (size_t w = 0; w < words; w++) {
    row[w] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    expected[w] = row[w] ^ src[w];
  }
  qsop_bitset_xor_simd(row, src, words, simd);
  snprintf(label, sizeof(label), "%s xor", name);
  if (expect_words(label, row, expected, words) != 0) {
    return 1;
  }

  for (size_t w = 0; w < words; w++) {
    row[w] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    expected[w] = row[w] | src[w];
  }
  qsop_bitset_or_simd(row, src, words, simd);
  snprintf(label, sizeof(label), "%s or", name);
  if (expect_words(label, row, expected, words) != 0) {
    return 1;
  }

  for (size_t w = 0; w < words; w++) {
    row[w] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    expected[w] = row[w] & src[w];
  }
  qsop_bitset_and_simd(row, src, words, simd);
  snprintf(label, sizeof(label), "%s and", name);
  if (expect_words(label, row, expected, words) != 0) {
    return 1;
  }

  for (size_t w = 0; w < words; w++) {
    row[w] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    expected[w] = row[w] & ~src[w];
  }
  qsop_bitset_and_not_simd(row, src, words, simd);
  snprintf(label, sizeof(label), "%s and_not", name);
  return expect_words(label, row, expected, words);
}

static int test_rank_wrapper(const qsop_simd_vtable_t *simd, const char *name) {
  enum { nrows = 5, ncols = 130 };
  const size_t words = qsop_bitset_words(ncols);
  uint64_t scalar[nrows * 3] = {0};
  uint64_t vector[nrows * 3] = {0};

  qsop_bitset_set(qsop_bitset_row(scalar, words, 0), 0);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 0), 64);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 1), 1);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 1), 65);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 2), 2);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 2), 129);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 3), 0);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 3), 1);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 3), 64);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 3), 65);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 4), 2);
  qsop_bitset_set(qsop_bitset_row(scalar, words, 4), 129);
  memcpy(vector, scalar, sizeof(scalar));

  const uint32_t scalar_rank = qsop_gf2_rank_bitsets(scalar, nrows, ncols, words);
  const uint32_t vector_rank = qsop_gf2_rank_bitsets_simd(vector, nrows, ncols, words, simd);
  char label[96];
  snprintf(label, sizeof(label), "%s rank", name);
  return expect_u32(label, vector_rank, scalar_rank);
}

static int run_for_kernel(const qsop_simd_vtable_t *simd, const char *name) {
  int failures = 0;
  failures += test_popcount_wrappers(simd, name);
  failures += test_row_wrappers(simd, name);
  failures += test_rank_wrapper(simd, name);
  return failures;
}

int main(void) {
  int failures = 0;
  failures += run_for_kernel(qsop_simd_scalar_vtable(), "scalar");

  const qsop_simd_vtable_t *auto_simd = qsop_simd_resolve(QSOP_SIMD_KERNEL_AUTO);
  if (auto_simd != qsop_simd_scalar_vtable()) {
    failures += run_for_kernel(auto_simd, qsop_simd_kernel_name(auto_simd));
  }

  if (failures != 0) {
    fprintf(stderr, "%d bitset SIMD test(s) FAILED\n", failures);
    return 1;
  }
  fprintf(stderr, "bitset SIMD tests passed\n");
  return 0;
}
