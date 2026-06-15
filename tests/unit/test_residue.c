#include "dlx4sop/qsop.h"
#include "dlx4sop/residue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int test_shift_add_checked_invalid(void) {
  qsop_error_t error = {0};
  if (qsop_counts_shift_add_checked(0, NULL, NULL, 0, &error)) {
    fprintf(stderr, "shift_add_checked accepted invalid arguments\n");
    return 1;
  }
  if (strstr(error.message, "invalid residue shift-add") == NULL) {
    fprintf(stderr, "shift_add_checked returned unexpected error: %s\n", error.message);
    return 1;
  }
  return 0;
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

static int test_crt_reconstruct_decimal(void) {
  const uint64_t primes[] = {101, 103, 107};
  const uint64_t value = UINT64_C(123456);
  uint64_t residues[] = {value % primes[0], value % primes[1], value % primes[2]};
  qsop_error_t error = {0};
  char *text = NULL;

  if (!qsop_crt_reconstruct_decimal(residues, primes, 3, &text, &error)) {
    fprintf(stderr, "crt_reconstruct_decimal failed: %s\n", error.message);
    return 1;
  }
  if (strcmp(text, "123456") != 0) {
    fprintf(stderr, "crt_reconstruct_decimal expected 123456 got %s\n", text);
    free(text);
    return 1;
  }
  free(text);
  return 0;
}

static int test_crt_prime_count(void) {
  qsop_error_t error = {0};
  uint64_t *primes = NULL;
  size_t nprimes = 0;

  if (!qsop_crt_find_primes_for_nvars(64, &primes, &nprimes, &error)) {
    fprintf(stderr, "crt_find_primes failed: %s\n", error.message);
    return 1;
  }
  if (nprimes != 2 || !qsop_mod_is_prime_u64(primes[0]) || !qsop_mod_is_prime_u64(primes[1])) {
    fprintf(stderr, "crt_find_primes returned invalid prime set\n");
    free(primes);
    return 1;
  }
  free(primes);
  return 0;
}

static int test_fourier_helpers(void) {
  const uint32_t r = 8;
  qsop_error_t error = {0};
  uint64_t prime = 0;
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;

  if (!qsop_fourier_find_ntt_prime(r, 4, &prime, &error)) {
    fprintf(stderr, "fourier_find_ntt_prime failed: %s\n", error.message);
    return 1;
  }
  if (!qsop_mod_is_prime_u64(prime) || (prime - 1U) % r != 0 || prime <= (UINT64_C(1) << 4U)) {
    fprintf(stderr, "fourier_find_ntt_prime returned invalid prime %" PRIu64 "\n", prime);
    return 1;
  }
  if (!qsop_fourier_find_order_root(prime, r, &root, &error)) {
    fprintf(stderr, "fourier_find_order_root failed: %s\n", error.message);
    return 1;
  }
  if (qsop_mod_pow_u64(root, r, prime) != 1 ||
      qsop_mod_pow_u64(root, r / 2U, prime) == 1) {
    fprintf(stderr, "fourier root does not have exact order %" PRIu32 "\n", r);
    return 1;
  }
  if (!qsop_fourier_make_root_powers(r, root, prime, &powers, &error)) {
    fprintf(stderr, "fourier_make_root_powers failed: %s\n", error.message);
    return 1;
  }
  inv_root = qsop_mod_pow_u64(root, prime - 2U, prime);
  if (!qsop_fourier_make_root_powers(r, inv_root, prime, &inv_powers, &error)) {
    fprintf(stderr, "fourier_make_root_powers inverse failed: %s\n", error.message);
    free(powers);
    return 1;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    for (uint32_t residue = 0; residue < r; residue++) {
      const uint64_t expected = qsop_mod_pow_u64(root, ((uint64_t)mode * residue) % r, prime);
      if (powers[(size_t)mode * r + residue] != expected) {
        fprintf(stderr, "fourier power mismatch at mode %" PRIu32 " residue %" PRIu32 "\n",
                mode, residue);
        free(powers);
        free(inv_powers);
        return 1;
      }
    }
  }
  uint64_t modes[8] = {0};
  for (uint32_t mode = 0; mode < r; mode++) {
    modes[mode] = qsop_mod_add_u64(2, powers[(size_t)mode * r + 2], prime);
  }
  uint64_t counts[8] = {0};
  if (!qsop_fourier_inverse_counts(r, modes, 3, powers, inv_powers, prime, counts, &error)) {
    fprintf(stderr, "fourier_inverse_counts failed: %s\n", error.message);
    free(powers);
    free(inv_powers);
    return 1;
  }
  for (uint32_t residue = 0; residue < r; residue++) {
    const uint64_t expected = residue == 3 ? 2 : (residue == 5 ? 1 : 0);
    if (counts[residue] != expected) {
      fprintf(stderr, "fourier inverse mismatch at residue %" PRIu32 ": got %" PRIu64 "\n",
              residue, counts[residue]);
      free(powers);
      free(inv_powers);
      return 1;
    }
  }
  free(powers);
  free(inv_powers);
  return 0;
}

static int test_qsop_write_errors(void) {
  qsop_error_t error = {0};
  if (qsop_write_file(NULL, NULL, &error)) {
    fprintf(stderr, "qsop_write_file accepted null arguments\n");
    return 1;
  }
  if (strstr(error.message, "null write argument") == NULL) {
    fprintf(stderr, "qsop_write_file returned unexpected null error: %s\n", error.message);
    return 1;
  }

  uint32_t unary[] = {1, 0};
  uint32_t edge_u[] = {0};
  uint32_t edge_v[] = {1};
  uint32_t edge_q[] = {4};
  qsop_instance_t qsop = {
      .r = 8,
      .nvars = 2,
      .norm_h = 0,
      .constant = 3,
      .mode = QSOP_MODE_SIGN,
      .unary = unary,
      .nedges = 1,
      .edge_u = edge_u,
      .edge_v = edge_v,
      .edge_q = edge_q,
  };

  FILE *full = fopen("/dev/full", "w");
  if (full == NULL) {
    return 0;
  }
  setvbuf(full, NULL, _IONBF, 0);
  error = (qsop_error_t){0};
  const bool ok = qsop_write_file(full, &qsop, &error);
  fclose(full);
  if (ok) {
    fprintf(stderr, "qsop_write_file unexpectedly wrote to /dev/full\n");
    return 1;
  }
  if (strstr(error.message, "write failed") == NULL) {
    fprintf(stderr, "qsop_write_file returned unexpected write error: %s\n", error.message);
    return 1;
  }
  return 0;
}

int main(void) {
  if (test_shift_add() != 0) {
    return 1;
  }
  if (test_clear() != 0) {
    return 1;
  }
  if (test_shift_add_checked_invalid() != 0) {
    return 1;
  }
  if (test_convolve_support() != 0) {
    return 1;
  }
  if (test_convolve_counts() != 0) {
    return 1;
  }
  if (test_crt_reconstruct_decimal() != 0) {
    return 1;
  }
  if (test_crt_prime_count() != 0) {
    return 1;
  }
  if (test_fourier_helpers() != 0) {
    return 1;
  }
  if (test_qsop_write_errors() != 0) {
    return 1;
  }
  return 0;
}
