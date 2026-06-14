#ifndef DLX4SOP_RESIDUE_H
#define DLX4SOP_RESIDUE_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool qsop_counts_alloc(uint32_t r, uint64_t **out, qsop_error_t *error);

void qsop_counts_clear(uint32_t r, uint64_t *counts);

bool qsop_count_add(uint64_t *dst, uint64_t value, qsop_error_t *error);
bool qsop_count_mul(uint64_t left, uint64_t right, uint64_t *out, qsop_error_t *error);

void qsop_counts_shift_add(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift);
bool qsop_counts_shift_add_checked(uint32_t r, uint64_t *dst, const uint64_t *src,
                                   uint32_t shift, qsop_error_t *error);

bool qsop_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                          const uint64_t *right, qsop_error_t *error);

uint64_t qsop_mod_add_u64(uint64_t a, uint64_t b, uint64_t mod);
uint64_t qsop_mod_mul_u64(uint64_t a, uint64_t b, uint64_t mod);
uint64_t qsop_mod_pow_u64(uint64_t base, uint64_t exp, uint64_t mod);
bool qsop_mod_is_prime_u64(uint64_t n);

bool qsop_crt_find_primes_for_nvars(uint32_t nvars, uint64_t **out_primes, size_t *out_len,
                                    qsop_error_t *error);
bool qsop_crt_reconstruct_decimal(const uint64_t *residues, const uint64_t *primes,
                                  size_t nprimes, char **out, qsop_error_t *error);

#endif
