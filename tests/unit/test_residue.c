#include "dlx4sop/residue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int expect_counts(const char *name, uint32_t r, const uint64_t *actual,
                         const uint64_t *expected) {
  for (uint32_t i = 0; i < r; i++) {
    if (actual[i] != expected[i]) {
      fprintf(stderr, "%s: residue %" PRIu32 " expected %" PRIu64 " got %" PRIu64 "\n", name,
              i, expected[i], actual[i]);
      return 1;
    }
  }
  return 0;
}

static int test_shift_add(void) {
  const uint32_t r = 4;
  uint64_t src[] = {1, 2, 0, 3};
  uint64_t dst[] = {0, 0, 0, 0};
  uint64_t expected[] = {3, 1, 2, 0};

  qsop_counts_shift_add(r, dst, src, 1);
  return expect_counts("shift_add", r, dst, expected);
}

static int test_clear(void) {
  const uint32_t r = 4;
  uint64_t counts[] = {1, 2, 3, 4};
  uint64_t expected[] = {0, 0, 0, 0};

  qsop_counts_clear(r, counts);
  return expect_counts("clear", r, counts, expected);
}

static int test_convolve_support(void) {
  const uint32_t r = 4;
  uint64_t left[] = {1, 1, 0, 0};
  uint64_t right[] = {1, 0, 1, 0};
  uint64_t dst[] = {99, 99, 99, 99};
  uint64_t expected[] = {1, 1, 1, 1};
  qsop_error_t error = {0};

  if (!qsop_counts_convolve(r, dst, left, right, &error)) {
    fprintf(stderr, "convolve_support failed: %s\n", error.message);
    return 1;
  }
  return expect_counts("convolve_support", r, dst, expected);
}

static int test_convolve_counts(void) {
  const uint32_t r = 4;
  uint64_t left[] = {0, 0, 0, 2};
  uint64_t right[] = {0, 0, 0, 5};
  uint64_t dst[] = {0, 0, 0, 0};
  uint64_t expected[] = {0, 0, 10, 0};
  qsop_error_t error = {0};

  if (!qsop_counts_convolve(r, dst, left, right, &error)) {
    fprintf(stderr, "convolve_counts failed: %s\n", error.message);
    return 1;
  }
  return expect_counts("convolve_counts", r, dst, expected);
}

int main(void) {
  if (test_shift_add() != 0) {
    return 1;
  }
  if (test_clear() != 0) {
    return 1;
  }
  if (test_convolve_support() != 0) {
    return 1;
  }
  if (test_convolve_counts() != 0) {
    return 1;
  }
  return 0;
}
