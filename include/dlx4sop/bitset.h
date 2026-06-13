#ifndef DLX4SOP_BITSET_H
#define DLX4SOP_BITSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t qsop_bitset_words(uint32_t nbits);
uint64_t *qsop_bitset_row(uint64_t *matrix, size_t words, uint32_t row);
const uint64_t *qsop_bitset_const_row(const uint64_t *matrix, size_t words, uint32_t row);

uint32_t qsop_popcount_u64(uint64_t value);
void qsop_bitset_set(uint64_t *bits, uint32_t bit);
void qsop_bitset_clear(uint64_t *bits, uint32_t bit);
bool qsop_bitset_get(const uint64_t *bits, uint32_t bit);
void qsop_bitset_zero(uint64_t *bits, size_t words);
void qsop_bitset_copy(uint64_t *dst, const uint64_t *src, size_t words);
void qsop_bitset_or(uint64_t *dst, const uint64_t *src, size_t words);
void qsop_bitset_and(uint64_t *dst, const uint64_t *src, size_t words);
void qsop_bitset_and_not(uint64_t *dst, const uint64_t *src, size_t words);
void qsop_bitset_xor(uint64_t *dst, const uint64_t *src, size_t words);
bool qsop_bitset_equal(const uint64_t *left, const uint64_t *right, size_t words);
bool qsop_bitset_empty(const uint64_t *bits, size_t words);
uint32_t qsop_bitset_popcount(const uint64_t *bits, size_t words);
uint32_t qsop_bitset_popcount_intersection(const uint64_t *left, const uint64_t *right,
                                           size_t words);
uint64_t qsop_bitset_fingerprint(const uint64_t *bits, size_t words);
uint32_t qsop_gf2_rank_bitsets(uint64_t *rows, uint32_t nrows, uint32_t ncols, size_t words);

#endif
