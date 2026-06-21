#include "dlx4sop/bitset.h"

#include <string.h>

size_t qsop_bitset_words(uint32_t nbits) {
  return ((size_t)nbits + 63U) / 64U;
}

uint64_t *qsop_bitset_row(uint64_t *matrix, size_t words, uint32_t row) {
  return matrix + (size_t)row * words;
}

const uint64_t *qsop_bitset_const_row(const uint64_t *matrix, size_t words, uint32_t row) {
  return matrix + (size_t)row * words;
}

uint32_t qsop_popcount_u64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_popcountll(value);
#else
  uint32_t count = 0;
  while (value != 0) {
    value &= value - 1U;
    count++;
  }
  return count;
#endif
}

void qsop_bitset_set(uint64_t *bits, uint32_t bit) {
  bits[bit / 64U] |= UINT64_C(1) << (bit % 64U);
}

void qsop_bitset_clear(uint64_t *bits, uint32_t bit) {
  bits[bit / 64U] &= ~(UINT64_C(1) << (bit % 64U));
}

bool qsop_bitset_get(const uint64_t *bits, uint32_t bit) {
  return (bits[bit / 64U] & (UINT64_C(1) << (bit % 64U))) != 0;
}

void qsop_bitset_zero(uint64_t *bits, size_t words) {
  memset(bits, 0, words * sizeof(*bits));
}

void qsop_bitset_copy(uint64_t *dst, const uint64_t *src, size_t words) {
  memcpy(dst, src, words * sizeof(*dst));
}

void qsop_bitset_or(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] |= src[w];
  }
}

void qsop_bitset_and(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] &= src[w];
  }
}

void qsop_bitset_and_not(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] &= ~src[w];
  }
}

void qsop_bitset_xor(uint64_t *dst, const uint64_t *src, size_t words) {
  for (size_t w = 0; w < words; w++) {
    dst[w] ^= src[w];
  }
}

bool qsop_bitset_equal(const uint64_t *left, const uint64_t *right, size_t words) {
  return memcmp(left, right, words * sizeof(*left)) == 0;
}

bool qsop_bitset_empty(const uint64_t *bits, size_t words) {
  for (size_t w = 0; w < words; w++) {
    if (bits[w] != 0) {
      return false;
    }
  }
  return true;
}

uint32_t qsop_bitset_popcount(const uint64_t *bits, size_t words) {
  uint32_t count = 0;
  for (size_t w = 0; w < words; w++) {
    count += qsop_popcount_u64(bits[w]);
  }
  return count;
}

uint32_t qsop_bitset_popcount_intersection(const uint64_t *left, const uint64_t *right,
                                           size_t words) {
  uint32_t count = 0;
  for (size_t w = 0; w < words; w++) {
    count += qsop_popcount_u64(left[w] & right[w]);
  }
  return count;
}

uint64_t qsop_bitset_fingerprint(const uint64_t *bits, size_t words) {
  uint64_t fingerprint = UINT64_C(1469598103934665603);
  for (size_t w = 0; w < words; w++) {
    fingerprint ^= bits[w];
    fingerprint *= UINT64_C(1099511628211);
  }
  return fingerprint;
}

uint32_t qsop_gf2_rank_bitsets(uint64_t *rows, uint32_t nrows, uint32_t ncols, size_t words) {
  uint32_t rank = 0;
  for (uint32_t col = 0; col < ncols && rank < nrows; col++) {
    /* The column bit is fixed across all rows this iteration: hoist its word + mask out of
       the per-row loops and index rows directly (rows + r*words) instead of recomputing. */
    const size_t col_word = (size_t)(col / 64U);
    const uint64_t col_mask = UINT64_C(1) << (col % 64U);
    uint32_t pivot = rank;
    while (pivot < nrows && (rows[(size_t)pivot * words + col_word] & col_mask) == 0) {
      pivot++;
    }
    if (pivot == nrows) {
      continue;
    }
    uint64_t *rank_row = rows + (size_t)rank * words;
    if (pivot != rank) {
      uint64_t *pivot_row = rows + (size_t)pivot * words;
      for (size_t w = 0; w < words; w++) {
        const uint64_t tmp = rank_row[w];
        rank_row[w] = pivot_row[w];
        pivot_row[w] = tmp;
      }
    }
    for (uint32_t row = 0; row < nrows; row++) {
      uint64_t *target = rows + (size_t)row * words;
      if (row == rank || (target[col_word] & col_mask) == 0) {
        continue;
      }
      qsop_bitset_xor(target, rank_row, words);
    }
    rank++;
  }
  return rank;
}
