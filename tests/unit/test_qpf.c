#include "dlx4sop/qpf.h"
#include "dlx4sop/residue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t random_state = UINT64_C(0x7b52c941d06a37ef);

static uint64_t random_u64(void) {
  random_state ^= random_state << 13U;
  random_state ^= random_state >> 7U;
  random_state ^= random_state << 17U;
  return random_state;
}

static int one_case(uint32_t m, uint32_t constraints) {
  const uint64_t prime = 97U;
  const uint32_t order = 8U;
  const uint64_t primitive = 5U;
  const uint64_t root = qsop_mod_pow_u64(primitive, (prime - 1U) / order, prime);
  qsop_qpf_scalar_t eps = {0};
  qsop_qpf_scalar_modular(&eps, order, prime, root, 1U + random_u64() % (prime - 1U));
  qsop_qpf_t qpf = {0};
  qsop_error_t error = {0};
  if (!qsop_qpf_init(&qpf, m, &eps, &error)) {
    fprintf(stderr, "qpf init: %s\n", error.message);
    return 1;
  }
  for (uint32_t i = 0; i < m; i++) {
    qsop_qpf_set_linear(&qpf, i, (uint8_t)(random_u64() & 3U));
    for (uint32_t j = i + 1U; j < m; j++) {
      if ((random_u64() & 1U) != 0) {
        qsop_qpf_toggle_quadratic(&qpf, i, j);
      }
    }
  }
  const size_t words = ((size_t)m + 63U) / 64U;
  uint64_t row[1] = {0};
  for (uint32_t c = 0; c < constraints; c++) {
    row[0] = m == 64U ? random_u64() : random_u64() & ((UINT64_C(1) << m) - 1U);
    if (!qsop_qpf_add_constraint(&qpf, row, random_u64() & 1U, &error)) {
      fprintf(stderr, "constraint: %s\n", error.message);
      qsop_qpf_free(&qpf);
      return 1;
    }
  }

  qsop_qpf_scalar_t expected = eps;
  expected.value = 0;
  const uint64_t assignments = UINT64_C(1) << m;
  for (uint64_t bits = 0; bits < assignments; bits++) {
    qsop_qpf_scalar_t value = {0};
    if (!qsop_qpf_eval(&qpf, words == 0 ? NULL : &bits, &value, &error) ||
        !qsop_qpf_scalar_add(&expected, &value, &error)) {
      fprintf(stderr, "brute force: %s\n", error.message);
      qsop_qpf_free(&qpf);
      return 1;
    }
  }
  qsop_qpf_scalar_t actual = {0};
  if (!qsop_qpf_total_sum(&qpf, &actual, &error)) {
    fprintf(stderr, "total sum: %s\n", error.message);
    qsop_qpf_free(&qpf);
    return 1;
  }
  qsop_qpf_free(&qpf);
  if (actual.value != expected.value) {
    fprintf(stderr,
            "m=%" PRIu32 " constraints=%" PRIu32 ": expected %" PRIu64 ", got %" PRIu64
            "\n",
            m, constraints, expected.value, actual.value);
    return 1;
  }
  return 0;
}

int main(void) {
  for (uint32_t m = 0; m <= 12U; m++) {
    const uint32_t cases = m < 9U ? 40U : 8U;
    for (uint32_t trial = 0; trial < cases; trial++) {
      if (one_case(m, m == 0 ? 0U : (uint32_t)(random_u64() % (m + 3U))) != 0) {
        return 1;
      }
    }
  }
  puts("qpf unit tests passed");
  return 0;
}
