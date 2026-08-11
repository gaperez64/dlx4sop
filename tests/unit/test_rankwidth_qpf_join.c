#include "dlx4sop/bitset.h"
#include "../../src/solve/rankwidth_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static uint64_t random_state = UINT64_C(0x83a42d150b6ce917);

static uint64_t random_u64(void) {
  random_state ^= random_state << 13U;
  random_state ^= random_state >> 7U;
  random_state ^= random_state << 17U;
  return random_state;
}

static void signature_of(const qsop_instance_t *qsop, const uint64_t *adj,
                         const uint64_t *node_set, const uint64_t *assignment, size_t words,
                         uint64_t *signature) {
  qsop_bitset_zero(signature, words);
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!qsop_bitset_get(assignment, v)) {
      continue;
    }
    qsop_bitset_xor(signature, qsop_bitset_const_row(adj, words, v), words);
  }
  qsop_bitset_and_not(signature, node_set, words);
}

static uint32_t cross_parity(const qsop_instance_t *qsop, const uint64_t *adj,
                             const uint64_t *left, const uint64_t *right, size_t words) {
  uint32_t parity = 0;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (qsop_bitset_get(left, v)) {
      parity ^= qsop_bitset_popcount_intersection(
                    qsop_bitset_const_row(adj, words, v), right, words) &
                1U;
    }
  }
  return parity;
}

int main(void) {
  qsop_instance_t qsop = {.r = 8U, .nvars = 12U, .norm_h = 0U, .constant = 0U};
  qsop.unary = calloc(qsop.nvars, sizeof(*qsop.unary));
  const uint32_t max_edges = qsop.nvars * (qsop.nvars - 1U) / 2U;
  qsop.edge_u = calloc(max_edges, sizeof(*qsop.edge_u));
  qsop.edge_v = calloc(max_edges, sizeof(*qsop.edge_v));
  if (qsop.unary == NULL || qsop.edge_u == NULL || qsop.edge_v == NULL) {
    return 1;
  }
  for (uint32_t u = 0; u < qsop.nvars; u++) {
    for (uint32_t v = u + 1U; v < qsop.nvars; v++) {
      if ((random_u64() & 3U) != 0U) {
        qsop.edge_u[qsop.nedges] = u;
        qsop.edge_v[qsop.nedges++] = v;
      }
    }
  }
  qsop_error_t error = {0};
  qsop_rankwidth_decomposition_t *decomposition = NULL;
  if (!qsop_rankwidth_decomposition_generate(&qsop, QSOP_RANKWIDTH_GENERATOR_BALANCED,
                                              &decomposition, &error)) {
    fprintf(stderr, "decomposition: %s\n", error.message);
    return 1;
  }
  uint64_t *adj = rw_adjacency_bitsets(&qsop, decomposition->words, &error);
  const size_t words = decomposition->words;
  uint64_t *assignment = calloc(words, sizeof(*assignment));
  uint64_t *signature = calloc(words, sizeof(*signature));
  uint64_t *recovered = calloc(words, sizeof(*recovered));
  uint64_t coord[1] = {0};
  if (adj == NULL || assignment == NULL || signature == NULL || recovered == NULL) {
    return 1;
  }

  for (uint32_t node = 0; node < decomposition->nnodes; node++) {
    rw_linear_section_t section = {0};
    if (!rw_linear_section_build(&qsop, decomposition, node, adj, &section, &error)) {
      fprintf(stderr, "section: %s\n", error.message);
      return 1;
    }
    for (uint32_t trial = 0; trial < 200U; trial++) {
      qsop_bitset_zero(assignment, words);
      for (uint32_t v = 0; v < qsop.nvars; v++) {
        if (qsop_bitset_get(node_vars_const(decomposition, node), v) &&
            (random_u64() & 1U) != 0U) {
          qsop_bitset_set(assignment, v);
        }
      }
      signature_of(&qsop, adj, node_vars_const(decomposition, node), assignment, words,
                   signature);
      if (!rw_linear_section_coord(&section, signature, coord, &error)) {
        fprintf(stderr, "coord: %s\n", error.message);
        return 1;
      }
      rw_linear_section_apply(&section, coord, recovered);
      signature_of(&qsop, adj, node_vars_const(decomposition, node), recovered, words,
                   assignment);
      if (!qsop_bitset_equal(signature, assignment, words)) {
        fprintf(stderr, "linear section is not a right inverse at node %u\n", node);
        return 1;
      }
    }

    if (decomposition->nodes[node].kind == RW_NODE_JOIN) {
      const rw_node_t *join = &decomposition->nodes[node];
      rw_linear_section_t left = {0};
      rw_linear_section_t right = {0};
      uint64_t *matrix = NULL;
      size_t matrix_words = 0;
      if (!rw_linear_section_build(&qsop, decomposition, join->left, adj, &left, &error) ||
          !rw_linear_section_build(&qsop, decomposition, join->right, adj, &right, &error) ||
          !rw_qpf_crossing_matrix(&qsop, &left, &right, adj, &matrix, &matrix_words, &error)) {
        fprintf(stderr, "crossing matrix: %s\n", error.message);
        return 1;
      }
      uint64_t left_coord[1] = {0};
      uint64_t right_coord[1] = {0};
      uint64_t *left_assignment = calloc(words, sizeof(*left_assignment));
      uint64_t *right_assignment = calloc(words, sizeof(*right_assignment));
      for (uint32_t trial = 0; trial < 200U; trial++) {
        left_coord[0] = left.dim == 64U ? random_u64()
                                            : random_u64() & ((UINT64_C(1) << left.dim) - 1U);
        right_coord[0] = right.dim == 64U ? random_u64()
                                               : random_u64() & ((UINT64_C(1) << right.dim) - 1U);
        rw_linear_section_apply(&left, left_coord, left_assignment);
        rw_linear_section_apply(&right, right_coord, right_assignment);
        uint32_t from_matrix = 0;
        for (uint32_t i = 0; i < left.dim; i++) {
          if (qsop_bitset_get(left_coord, i)) {
            from_matrix ^= qsop_bitset_popcount_intersection(
                               matrix + (size_t)i * (matrix_words == 0 ? 1U : matrix_words),
                               right_coord, matrix_words) &
                           1U;
          }
        }
        if (from_matrix != cross_parity(&qsop, adj, left_assignment, right_assignment, words)) {
          fprintf(stderr, "crossing matrix mismatch at node %u\n", node);
          return 1;
        }
      }
      free(left_assignment);
      free(right_assignment);
      free(matrix);
      rw_linear_section_free(&left);
      rw_linear_section_free(&right);
    }
    rw_linear_section_free(&section);
  }

  for (uint32_t mode = 0; mode < 8U; mode++) {
    qsop_amplitude_t expected = {0};
    qsop_amplitude_t actual = {0};
    qsop_solve_stats_t hybrid_stats = {0};
    bool handled = false;
    if (!qsop_solve_rankwidth_single_mode(&qsop, decomposition, qsop.nvars, mode, &expected,
                                          NULL, NULL, &error) ||
        !rw_qpf_hybrid_single_mode(&qsop, decomposition, adj, mode, 4096U, &handled, &actual,
                                   &hybrid_stats, &error) ||
        !handled) {
      fprintf(stderr, "hybrid solve mode %u: %s\n", mode, error.message);
      return 1;
    }
    const long double expected_re = ldexpl(expected.re, expected.scale_exp2);
    const long double expected_im = ldexpl(expected.im, expected.scale_exp2);
    const long double actual_re = ldexpl(actual.re, actual.scale_exp2);
    const long double actual_im = ldexpl(actual.im, actual.scale_exp2);
    if (fabsl(expected_re - actual_re) > 1e-12L || fabsl(expected_im - actual_im) > 1e-12L) {
      fprintf(stderr, "hybrid amplitude mismatch in mode %u: (%Lg,%Lg) != (%Lg,%Lg)\n", mode,
              expected_re, expected_im, actual_re, actual_im);
      return 1;
    }
    if (hybrid_stats.qpf_collapses == 0U) {
      fprintf(stderr, "hybrid mode %u did not exercise point collapse\n", mode);
      return 1;
    }
    qsop_amplitude_t rescued = {0};
    qsop_solve_stats_t rescued_stats = {0};
    if (!qsop_solve_rankwidth_single_mode_options(
            &qsop, decomposition, qsop.nvars - 1U, mode,
            &(qsop_rankwidth_single_mode_options_t){.qpf_max_terms = 4096U}, &rescued,
            &rescued_stats, NULL, &error)) {
      fprintf(stderr, "rankwidth QPF refusal rescue mode %u: %s\n", mode, error.message);
      return 1;
    }
    const long double rescued_re = ldexpl(rescued.re, rescued.scale_exp2);
    const long double rescued_im = ldexpl(rescued.im, rescued.scale_exp2);
    if (fabsl(expected_re - rescued_re) > 1e-12L ||
        fabsl(expected_im - rescued_im) > 1e-12L || rescued_stats.qpf_collapses == 0U) {
      fprintf(stderr, "rankwidth QPF refusal rescue mismatch in mode %u\n", mode);
      return 1;
    }
  }

  qsop_result_t *point_counts = NULL;
  qsop_result_t *rescued_counts = NULL;
  qsop_solve_stats_t count_stats = {0};
  if (!qsop_solve_rankwidth_options_mode_trace_stats(
          &qsop, decomposition, qsop.nvars, QSOP_RANKWIDTH_SOLVE_COUNT_TABLE, NULL,
          &point_counts, NULL, NULL, &error) ||
      !qsop_solve_rankwidth_options_mode_trace_stats(
          &qsop, decomposition, qsop.nvars - 1U, QSOP_RANKWIDTH_SOLVE_COUNT_TABLE,
          &(qsop_rankwidth_solve_options_t){.qpf_max_terms = 4096U}, &rescued_counts,
          &count_stats, NULL, &error)) {
    fprintf(stderr, "rankwidth QPF exact-count rescue: %s\n", error.message);
    return 1;
  }
  for (uint32_t residue = 0; residue < 8U; residue++) {
    if (point_counts->counts[residue] != rescued_counts->counts[residue]) {
      fprintf(stderr, "rankwidth QPF exact-count mismatch in residue %u\n", residue);
      return 1;
    }
  }
  if (count_stats.qpf_decompositions == 0U) {
    fputs("rankwidth QPF exact-count rescue did not report QPF work\n", stderr);
    return 1;
  }
  qsop_result_free(point_counts);
  qsop_result_free(rescued_counts);

  free(assignment);
  free(signature);
  free(recovered);
  free(adj);
  qsop_rankwidth_decomposition_free(decomposition);
  free(qsop.unary);
  free(qsop.edge_u);
  free(qsop.edge_v);
  puts("rankwidth QPF section/join tests passed");
  return 0;
}
