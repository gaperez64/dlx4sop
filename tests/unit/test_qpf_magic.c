#include "dlx4sop/qpf.h"
#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_stats.h"
#include "dlx4sop/residue.h"
#include "../../src/solve/qpf_magic.h"

#include <inttypes.h>
#include <stdio.h>

static uint64_t direct_value(const qsop_instance_t *qsop, uint32_t mode, uint64_t assignment,
                             uint64_t root, uint32_t order, uint64_t prime) {
  uint64_t exponent = (mode * qsop->constant) % qsop->r;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (((assignment >> v) & 1U) != 0) {
      exponent = (exponent + mode * qsop->unary[v]) % qsop->r;
    }
  }
  if ((mode & 1U) != 0) {
    for (uint32_t edge = 0; edge < qsop->nedges; edge++) {
      if (((assignment >> qsop->edge_u[edge]) & 1U) != 0 &&
          ((assignment >> qsop->edge_v[edge]) & 1U) != 0) {
        exponent = (exponent + qsop->r / 2U) % qsop->r;
      }
    }
  }
  return qsop_mod_pow_u64(root, exponent * (order / qsop->r), prime);
}

static int one_mode(const qsop_instance_t *qsop, uint32_t mode) {
  const uint64_t prime = 97U;
  const uint32_t order = 24U;
  const uint64_t root = qsop_mod_pow_u64(5U, (prime - 1U) / order, prime);
  qsop_qpf_scalar_t unit = {0};
  qsop_qpf_scalar_modular(&unit, order, prime, root, 1U);
  qsop_qpf_term_list_t terms = {0};
  qsop_error_t error = {0};
  if (!qsop_qpf_magic_decompose(qsop, mode, 1024U, &unit, &terms, &error)) {
    fprintf(stderr, "decompose: %s\n", error.message);
    return 1;
  }
  for (uint64_t assignment = 0; assignment < (UINT64_C(1) << qsop->nvars); assignment++) {
    qsop_qpf_scalar_t sum = unit;
    sum.value = 0;
    for (uint64_t term = 0; term < terms.len; term++) {
      qsop_qpf_scalar_t value = {0};
      if (!qsop_qpf_eval(&terms.terms[term], &assignment, &value, &error) ||
          !qsop_qpf_scalar_add(&sum, &value, &error)) {
        fprintf(stderr, "term eval: %s\n", error.message);
        qsop_qpf_term_list_free(&terms);
        return 1;
      }
    }
    const uint64_t expected = direct_value(qsop, mode, assignment, root, order, prime);
    if (sum.value != expected) {
      fprintf(stderr,
              "mode=%" PRIu32 " assignment=%" PRIu64 ": expected %" PRIu64 ", got %" PRIu64
              "\n",
              mode, assignment, expected, sum.value);
      qsop_qpf_term_list_free(&terms);
      return 1;
    }
  }
  qsop_qpf_term_list_free(&terms);
  return 0;
}

int main(void) {
  uint64_t unary[] = {1, 2, 5, 0};
  uint32_t edge_u[] = {0, 1, 0};
  uint32_t edge_v[] = {1, 2, 3};
  qsop_instance_t qsop = {
      .r = 6,
      .nvars = 4,
      .constant = 5,
      .unary = unary,
      .nedges = 3,
      .edge_u = edge_u,
      .edge_v = edge_v,
  };
  for (uint32_t mode = 0; mode < qsop.r; mode++) {
    if (one_mode(&qsop, mode) != 0) {
      return 1;
    }
  }

  uint64_t unary16[] = {1};
  qsop_instance_t mod16 = {.r = 16, .nvars = 1, .unary = unary16};
  if (qsop_magic_vertex_count(&mod16, 2U) != 1U ||
      qsop_magic_vertex_count(&mod16, 4U) != 0U) {
    fputs("general magic predicate failed for r=16\n", stderr);
    return 1;
  }

  uint64_t unary8[] = {1, 3, 5, 7, 1, 3};
  qsop_instance_t t6 = {.r = 8, .nvars = 6, .unary = unary8};
  const uint64_t prime = 97U;
  const uint32_t order = 8U;
  const uint64_t root = qsop_mod_pow_u64(5U, (prime - 1U) / order, prime);
  qsop_qpf_scalar_t unit = {0};
  qsop_qpf_scalar_modular(&unit, order, prime, root, 1U);
  qsop_qpf_term_list_t block = {0};
  qsop_error_t error = {0};
  if (qsop_qpf_stabilizer_term_bound(&t6, 1U) != 6U ||
      !qsop_qpf_magic_decompose(&t6, 1U, 6U, &unit, &block, &error) || block.len != 6U) {
    fprintf(stderr, "verified T6 block construction failed: %s\n", error.message);
    return 1;
  }
  for (uint64_t assignment = 0; assignment < 64U; assignment++) {
    qsop_qpf_scalar_t actual = unit;
    actual.value = 0U;
    for (uint64_t term = 0; term < block.len; term++) {
      qsop_qpf_scalar_t value = {0};
      if (!qsop_qpf_eval(&block.terms[term], &assignment, &value, &error) ||
          !qsop_qpf_scalar_add(&actual, &value, &error)) {
        fprintf(stderr, "T6 verifier: %s\n", error.message);
        return 1;
      }
    }
    const uint64_t expected = direct_value(&t6, 1U, assignment, root, order, prime);
    if (actual.value != expected) {
      fprintf(stderr, "T6 verifier assignment=%" PRIu64 ": expected %" PRIu64 ", got %" PRIu64
                      "\n",
              assignment, expected, actual.value);
      return 1;
    }
  }
  qsop_qpf_term_list_free(&block);
  puts("qpf magic tests passed");
  return 0;
}
