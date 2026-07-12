#include "dlx4sop/residue.h"
#include "qsop_internal.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool qsop_counts_alloc(uint32_t r, uint64_t **out, qsop_error_t *error) {
  if (out == NULL) {
    qsop_set_error(error, "internal error: null residue-count allocation output");
    return false;
  }
  *out = NULL;

  if (r == 0) {
    qsop_set_error(error, "cannot allocate residue vector for modulus 0");
    return false;
  }

  uint64_t *counts = calloc(r, sizeof(*counts));
  if (counts == NULL) {
    qsop_set_error(error, "out of memory while allocating residue counts");
    return false;
  }

  *out = counts;
  return true;
}

void qsop_counts_clear(uint32_t r, uint64_t *counts) {
  if (counts == NULL) {
    return;
  }
  memset(counts, 0, (size_t)r * sizeof(*counts));
}

bool qsop_count_add(uint64_t *dst, uint64_t value, qsop_error_t *error) {
  if (dst == NULL) {
    qsop_set_error(error, "internal error: null residue-count add argument");
    return false;
  }
  if (UINT64_MAX - *dst < value) {
    qsop_set_error(error, "residue count exceeds uint64 capacity; use a CRT-backed solver path");
    return false;
  }
  *dst += value;
  return true;
}

bool qsop_count_mul(uint64_t left, uint64_t right, uint64_t *out, qsop_error_t *error) {
  if (out == NULL) {
    qsop_set_error(error, "internal error: null residue-count multiply output");
    return false;
  }
  if (left != 0 && right > UINT64_MAX / left) {
    qsop_set_error(error,
              "residue count product %" PRIu64 " * %" PRIu64
              " exceeds uint64 capacity; use a CRT-backed solver path",
              left, right);
    return false;
  }
  *out = left * right;
  return true;
}

uint64_t qsop_mod_add_u64(uint64_t a, uint64_t b, uint64_t mod) {
  return a >= mod - b ? a - (mod - b) : a + b;
}

uint64_t qsop_mod_mul_u64(uint64_t a, uint64_t b, uint64_t mod) {
  __extension__ typedef unsigned __int128 uint128_t;
  return (uint64_t)(((uint128_t)a * b) % mod);
}

uint64_t qsop_mod_pow_u64(uint64_t base, uint64_t exp, uint64_t mod) {
  uint64_t result = 1;
  uint64_t value = base % mod;
  while (exp != 0) {
    if ((exp & 1U) != 0) {
      result = qsop_mod_mul_u64(result, value, mod);
    }
    exp >>= 1U;
    if (exp != 0) {
      value = qsop_mod_mul_u64(value, value, mod);
    }
  }
  return result;
}

static bool miller_rabin_witness(uint64_t n, uint64_t base, uint64_t d, uint32_t s) {
  if (base % n == 0) {
    return false;
  }
  uint64_t x = qsop_mod_pow_u64(base, d, n);
  if (x == 1 || x == n - 1U) {
    return false;
  }
  for (uint32_t r = 1; r < s; r++) {
    x = qsop_mod_mul_u64(x, x, n);
    if (x == n - 1U) {
      return false;
    }
  }
  return true;
}

bool qsop_mod_is_prime_u64(uint64_t n) {
  static const uint32_t small_primes[] = {2,  3,  5,  7,  11, 13,
                                          17, 19, 23, 29, 31, 37};
  if (n < 2) {
    return false;
  }
  for (size_t i = 0; i < sizeof(small_primes) / sizeof(small_primes[0]); i++) {
    const uint32_t p = small_primes[i];
    if (n == p) {
      return true;
    }
    if (n % p == 0) {
      return false;
    }
  }

  uint64_t d = n - 1U;
  uint32_t s = 0;
  while ((d & 1U) == 0) {
    d >>= 1U;
    s++;
  }

  static const uint64_t bases[] = {2,      325,     9375,      28178,
                                   450775, 9780504, 1795265022};
  for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
    if (miller_rabin_witness(n, bases[i], d, s)) {
      return false;
    }
  }
  return true;
}

typedef struct big_uint {
  uint32_t *limbs;
  size_t len;
  size_t cap;
} big_uint_t;

static void big_uint_free(big_uint_t *value) {
  if (value == NULL) {
    return;
  }
  free(value->limbs);
  *value = (big_uint_t){0};
}

static bool big_uint_reserve(big_uint_t *value, size_t needed, qsop_error_t *error) {
  if (needed <= value->cap) {
    return true;
  }
  size_t new_cap = value->cap == 0 ? 4U : value->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      qsop_set_error(error, "CRT integer is too large");
      return false;
    }
    new_cap *= 2U;
  }
  uint32_t *limbs = realloc(value->limbs, new_cap * sizeof(*limbs));
  if (limbs == NULL) {
    qsop_set_error(error, "out of memory while growing CRT integer");
    return false;
  }
  for (size_t i = value->cap; i < new_cap; i++) {
    limbs[i] = 0;
  }
  value->limbs = limbs;
  value->cap = new_cap;
  return true;
}

static void big_uint_normalize(big_uint_t *value) {
  while (value->len != 0 && value->limbs[value->len - 1U] == 0) {
    value->len--;
  }
}

static bool big_uint_set_u64(big_uint_t *value, uint64_t input, qsop_error_t *error) {
  static const uint64_t base = UINT64_C(1000000000);
  value->len = 0;
  if (!big_uint_reserve(value, 3, error)) {
    return false;
  }
  while (input != 0) {
    value->limbs[value->len++] = (uint32_t)(input % base);
    input /= base;
  }
  return true;
}

static uint64_t big_uint_mod_u64(const big_uint_t *value, uint64_t modulus) {
  static const uint64_t base = UINT64_C(1000000000);
  __extension__ typedef unsigned __int128 uint128_t;
  uint64_t residue = 0;
  for (size_t i = value->len; i > 0; i--) {
    residue = (uint64_t)(((uint128_t)residue * base + value->limbs[i - 1U]) % modulus);
  }
  return residue;
}

static bool big_uint_mul_u64(big_uint_t *value, uint64_t multiplier, qsop_error_t *error) {
  static const uint64_t base = UINT64_C(1000000000);
  __extension__ typedef unsigned __int128 uint128_t;
  if (value->len == 0 || multiplier == 1) {
    return true;
  }
  if (multiplier == 0) {
    value->len = 0;
    return true;
  }
  if (!big_uint_reserve(value, value->len + 3U, error)) {
    return false;
  }
  uint128_t carry = 0;
  for (size_t i = 0; i < value->len; i++) {
    const uint128_t product = (uint128_t)value->limbs[i] * multiplier + carry;
    value->limbs[i] = (uint32_t)(product % base);
    carry = product / base;
  }
  while (carry != 0) {
    if (!big_uint_reserve(value, value->len + 1U, error)) {
      return false;
    }
    value->limbs[value->len++] = (uint32_t)(carry % base);
    carry /= base;
  }
  big_uint_normalize(value);
  return true;
}

static bool big_uint_add_mul_u64(big_uint_t *value, const big_uint_t *addend,
                                 uint64_t multiplier, qsop_error_t *error) {
  static const uint64_t base = UINT64_C(1000000000);
  __extension__ typedef unsigned __int128 uint128_t;
  if (addend->len == 0 || multiplier == 0) {
    return true;
  }
  if (!big_uint_reserve(value, addend->len + 3U, error)) {
    return false;
  }
  if (value->len < addend->len) {
    for (size_t i = value->len; i < addend->len; i++) {
      value->limbs[i] = 0;
    }
    value->len = addend->len;
  }
  uint128_t carry = 0;
  size_t i = 0;
  for (; i < addend->len; i++) {
    const uint128_t sum =
        (uint128_t)addend->limbs[i] * multiplier + value->limbs[i] + carry;
    value->limbs[i] = (uint32_t)(sum % base);
    carry = sum / base;
  }
  while (carry != 0) {
    if (!big_uint_reserve(value, i + 1U, error)) {
      return false;
    }
    if (i >= value->len) {
      value->limbs[value->len++] = 0;
    }
    const uint128_t sum = (uint128_t)value->limbs[i] + carry;
    value->limbs[i] = (uint32_t)(sum % base);
    carry = sum / base;
    i++;
  }
  big_uint_normalize(value);
  return true;
}

static char *big_uint_to_string(const big_uint_t *value, qsop_error_t *error) {
  if (value->len == 0) {
    char *zero = malloc(2);
    if (zero == NULL) {
      qsop_set_error(error, "out of memory while formatting CRT count");
      return NULL;
    }
    zero[0] = '0';
    zero[1] = '\0';
    return zero;
  }
  const size_t cap = value->len * 9U + 2U;
  char *text = malloc(cap);
  if (text == NULL) {
    qsop_set_error(error, "out of memory while formatting CRT count");
    return NULL;
  }
  size_t offset = (size_t)snprintf(text, cap, "%" PRIu32, value->limbs[value->len - 1U]);
  for (size_t i = value->len - 1U; i > 0; i--) {
    offset += (size_t)snprintf(text + offset, cap - offset, "%09" PRIu32, value->limbs[i - 1U]);
  }
  return text;
}

bool qsop_crt_reconstruct_decimal(const uint64_t *residues, const uint64_t *primes,
                                  size_t nprimes, char **out, qsop_error_t *error) {
  if (residues == NULL || primes == NULL || out == NULL) {
    qsop_set_error(error, "internal error: invalid CRT reconstruction argument");
    return false;
  }
  *out = NULL;

  big_uint_t value = {0};
  big_uint_t product = {0};
  if (nprimes == 0 || !big_uint_set_u64(&value, residues[0], error) ||
      !big_uint_set_u64(&product, primes[0], error)) {
    big_uint_free(&value);
    big_uint_free(&product);
    return false;
  }

  for (size_t i = 1; i < nprimes; i++) {
    const uint64_t prime = primes[i];
    const uint64_t value_mod = big_uint_mod_u64(&value, prime);
    const uint64_t product_mod = big_uint_mod_u64(&product, prime);
    const uint64_t delta =
        residues[i] >= value_mod ? residues[i] - value_mod : prime - (value_mod - residues[i]);
    const uint64_t coeff =
        qsop_mod_mul_u64(delta, qsop_mod_pow_u64(product_mod, prime - 2U, prime), prime);
    if (!big_uint_add_mul_u64(&value, &product, coeff, error) ||
        !big_uint_mul_u64(&product, prime, error)) {
      big_uint_free(&value);
      big_uint_free(&product);
      return false;
    }
  }

  *out = big_uint_to_string(&value, error);
  big_uint_free(&value);
  big_uint_free(&product);
  return *out != NULL;
}

bool qsop_crt_find_primes_for_nvars(uint32_t nvars, uint64_t **out_primes, size_t *out_len,
                                    qsop_error_t *error) {
  if (out_primes == NULL || out_len == NULL) {
    qsop_set_error(error, "internal error: null CRT prime output");
    return false;
  }
  *out_primes = NULL;
  *out_len = 0;

  const size_t needed = (size_t)nvars / 63U + 1U;
  uint64_t *primes = calloc(needed, sizeof(*primes));
  if (primes == NULL) {
    qsop_set_error(error, "out of memory while allocating CRT primes");
    return false;
  }

  uint64_t candidate = UINT64_MAX;
  size_t found = 0;
  for (uint64_t attempts = 0; found < needed && attempts < UINT64_C(10000000);
       attempts++, candidate -= 2U) {
    if (qsop_mod_is_prime_u64(candidate)) {
      primes[found++] = candidate;
    }
  }
  if (found != needed) {
    free(primes);
    qsop_set_error(error, "CRT mode could not find enough 64-bit primes");
    return false;
  }
  *out_primes = primes;
  *out_len = needed;
  return true;
}

static uint32_t factor_u32(uint32_t value, uint32_t *factors, uint32_t cap) {
  uint32_t len = 0;
  uint32_t remaining = value;
  for (uint32_t p = 2; p <= remaining / p; p++) {
    if (remaining % p != 0) {
      continue;
    }
    if (len < cap) {
      factors[len++] = p;
    }
    while (remaining % p == 0) {
      remaining /= p;
    }
  }
  if (remaining > 1 && len < cap) {
    factors[len++] = remaining;
  }
  return len;
}

bool qsop_fourier_find_ntt_prime(uint32_t r, uint32_t nvars, uint64_t *prime,
                                 qsop_error_t *error) {
  if (prime == NULL) {
    qsop_set_error(error, "internal error: null Fourier prime output");
    return false;
  }
  *prime = 0;
  if (r == 0 || nvars >= 64U) {
    qsop_set_error(error, "Fourier NTT prime search requires nonzero modulus and fewer than 64 variables");
    return false;
  }
  const uint64_t count_bound = UINT64_C(1) << nvars;
  uint64_t k = (UINT64_MAX - 1U) / r;
  for (uint64_t attempts = 0; attempts < UINT64_C(2000000) && k > 0; attempts++, k--) {
    const uint64_t candidate = k * (uint64_t)r + 1U;
    if (candidate <= count_bound) {
      break;
    }
    if (qsop_mod_is_prime_u64(candidate)) {
      *prime = candidate;
      return true;
    }
  }
  qsop_set_error(error, "Fourier mode could not find a 64-bit NTT prime for modulus %" PRIu32, r);
  return false;
}

bool qsop_fourier_find_ntt_primes_for_nvars(uint32_t r, uint32_t nvars, uint64_t **out_primes,
                                            size_t *out_len, qsop_error_t *error) {
  if (out_primes == NULL || out_len == NULL) {
    qsop_set_error(error, "internal error: null Fourier CRT prime output");
    return false;
  }
  *out_primes = NULL;
  *out_len = 0;
  if (r == 0) {
    qsop_set_error(error, "Fourier CRT prime search requires nonzero modulus");
    return false;
  }
  if (nvars < 64U) {
    uint64_t *primes = calloc(1, sizeof(*primes));
    if (primes == NULL) {
      qsop_set_error(error, "out of memory while allocating Fourier CRT primes");
      return false;
    }
    if (!qsop_fourier_find_ntt_prime(r, nvars, &primes[0], error)) {
      free(primes);
      return false;
    }
    *out_primes = primes;
    *out_len = 1;
    return true;
  }

  const size_t needed = (size_t)nvars / 63U + 1U;
  uint64_t *primes = calloc(needed, sizeof(*primes));
  if (primes == NULL) {
    qsop_set_error(error, "out of memory while allocating Fourier CRT primes");
    return false;
  }

  uint64_t k = (UINT64_MAX - 1U) / r;
  size_t found = 0;
  for (uint64_t attempts = 0; found < needed && attempts < UINT64_C(10000000) && k > 0;
       attempts++, k--) {
    const uint64_t candidate = k * (uint64_t)r + 1U;
    if (qsop_mod_is_prime_u64(candidate)) {
      primes[found++] = candidate;
    }
  }
  if (found != needed) {
    free(primes);
    qsop_set_error(error, "Fourier CRT mode could not find enough 64-bit NTT primes");
    return false;
  }
  *out_primes = primes;
  *out_len = needed;
  return true;
}

bool qsop_fourier_find_order_root(uint64_t prime, uint32_t r, uint64_t *root,
                                  qsop_error_t *error) {
  if (root == NULL) {
    qsop_set_error(error, "internal error: null Fourier root output");
    return false;
  }
  *root = 0;
  if (r == 0 || prime <= 2U || (prime - 1U) % r != 0) {
    qsop_set_error(error, "Fourier root search requires prime congruent to 1 modulo %" PRIu32, r);
    return false;
  }

  uint32_t factors[32] = {0};
  const uint32_t nfactors = factor_u32(r, factors, 32);
  for (uint64_t g = 2; g < UINT64_C(1000000); g++) {
    const uint64_t candidate = qsop_mod_pow_u64(g, (prime - 1U) / r, prime);
    if (candidate == 1) {
      continue;
    }
    bool exact = true;
    for (uint32_t i = 0; i < nfactors; i++) {
      if (qsop_mod_pow_u64(candidate, r / factors[i], prime) == 1) {
        exact = false;
        break;
      }
    }
    if (exact) {
      *root = candidate;
      return true;
    }
  }
  qsop_set_error(error, "Fourier mode could not find an order-%" PRIu32 " root", r);
  return false;
}

bool qsop_fourier_make_root_powers(uint32_t r, uint64_t root, uint64_t prime,
                                   uint64_t **out, qsop_error_t *error) {
  if (out == NULL) {
    qsop_set_error(error, "internal error: null Fourier powers output");
    return false;
  }
  *out = NULL;
  if ((size_t)r > SIZE_MAX / (r == 0 ? 1U : (size_t)r) / sizeof(uint64_t)) {
    qsop_set_error(error, "Fourier modulus is too large for dense mode tables");
    return false;
  }
  uint64_t *powers = calloc((size_t)r * r, sizeof(*powers));
  if (powers == NULL) {
    qsop_set_error(error, "out of memory while allocating Fourier powers");
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    for (uint32_t residue = 0; residue < r; residue++) {
      powers[(size_t)mode * r + residue] =
          qsop_mod_pow_u64(root, ((uint64_t)mode * residue) % r, prime);
    }
  }
  *out = powers;
  return true;
}

bool qsop_fourier_inverse_counts(uint32_t r, const uint64_t *modes, uint32_t shift,
                                 const uint64_t *powers, const uint64_t *inv_powers,
                                 uint64_t prime, uint64_t *counts, qsop_error_t *error) {
  if (r == 0 || modes == NULL || powers == NULL || inv_powers == NULL || counts == NULL) {
    qsop_set_error(error, "internal error: null Fourier inverse argument");
    return false;
  }
  const uint64_t inv_r = qsop_mod_pow_u64(r, prime - 2U, prime);
  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint64_t sum = 0;
    for (uint32_t mode = 0; mode < r; mode++) {
      uint64_t value = qsop_mod_mul_u64(modes[mode], powers[(size_t)mode * r + delta], prime);
      value = qsop_mod_mul_u64(value, inv_powers[(size_t)mode * r + residue], prime);
      sum = qsop_mod_add_u64(sum, value, prime);
    }
    counts[residue] = qsop_mod_mul_u64(sum, inv_r, prime);
  }
  return true;
}

void qsop_counts_shift_add(uint32_t r, uint64_t *dst, const uint64_t *src, uint32_t shift) {
  if (r == 0 || dst == NULL || src == NULL) {
    return;
  }

  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t target = residue + delta;
    if (target >= r) {
      target -= r;
    }
    dst[target] += src[residue];
  }
}

bool qsop_counts_shift_add_checked(uint32_t r, uint64_t *dst, const uint64_t *src,
                                   uint32_t shift, qsop_error_t *error) {
  if (r == 0 || dst == NULL || src == NULL) {
    qsop_set_error(error, "internal error: invalid residue shift-add argument");
    return false;
  }

  const uint32_t delta = shift % r;
  for (uint32_t residue = 0; residue < r; residue++) {
    uint32_t target = residue + delta;
    if (target >= r) {
      target -= r;
    }
    if (!qsop_count_add(&dst[target], src[residue], error)) {
      return false;
    }
  }
  return true;
}

bool qsop_counts_convolve(uint32_t r, uint64_t *dst, const uint64_t *left,
                          const uint64_t *right, qsop_error_t *error) {
  if (r == 0 || dst == NULL || left == NULL || right == NULL) {
    qsop_set_error(error, "internal error: invalid residue convolution argument");
    return false;
  }

  qsop_counts_clear(r, dst);
  for (uint32_t a = 0; a < r; a++) {
    if (left[a] == 0) {
      continue;
    }
    for (uint32_t b = 0; b < r; b++) {
      if (right[b] == 0) {
        continue;
      }
      uint32_t target = a + b;
      if (target >= r) {
        target -= r;
      }
      uint64_t product = 0;
      if (!qsop_count_mul(left[a], right[b], &product, error) ||
          !qsop_count_add(&dst[target], product, error)) {
        return false;
      }
    }
  }
  return true;
}
