#include "dlx4sop/bitset.h"
#include "dlx4sop/qpf.h"
#include "rankwidth_internal.h"
#include "qpf_magic.h"
#include "../core/qsop_internal.h"

#include <stdlib.h>
#include <string.h>

#define RW_QPF_DEFAULT_MAX_TERMS UINT64_C(4096)

typedef struct rw_qpf_list_term {
  qsop_qpf_t qpf;
  uint32_t *variables;
  uint64_t *fixed_assignment;
} rw_qpf_list_term_t;

typedef struct rw_qpf_list {
  rw_qpf_list_term_t *terms;
  uint64_t len;
  uint32_t nvars;
  size_t words;
} rw_qpf_list_t;

static bool qpf_bit_get(const uint64_t *bits, uint32_t bit) {
  return bits != NULL && ((bits[bit / 64U] >> (bit % 64U)) & 1U) != 0U;
}

static void rw_qpf_list_free(rw_qpf_list_t *list) {
  if (list == NULL) {
    return;
  }
  for (uint64_t i = 0; i < list->len; i++) {
    qsop_qpf_free(&list->terms[i].qpf);
    free(list->terms[i].variables);
    free(list->terms[i].fixed_assignment);
  }
  free(list->terms);
  *list = (rw_qpf_list_t){0};
}

static void scalar_set_zero(qsop_qpf_scalar_t *scalar) {
  if (scalar->backend == QSOP_QPF_SCALAR_MODULAR) {
    scalar->value = 0U;
  } else if (scalar->backend == QSOP_QPF_SCALAR_COMPLEX) {
    scalar->re = 0.0L;
    scalar->im = 0.0L;
    scalar->numeric_error_bound = 0.0L;
    scalar->scale_exp2 = 0;
  } else {
    memset(scalar->coefficients, 0, sizeof(scalar->coefficients));
    scalar->denominator_exp2 = 0U;
  }
}

static uint32_t qpf_first_set(const uint64_t *bits, size_t words, uint32_t nbits) {
  for (size_t word = 0; word < words; word++) {
    uint64_t value = bits[word];
    if (value == 0U) {
      continue;
    }
    const uint32_t bit = (uint32_t)(word * 64U) + rw_ctz_u64(value);
    return bit < nbits ? bit : UINT32_MAX;
  }
  return UINT32_MAX;
}

void rw_linear_section_free(rw_linear_section_t *section) {
  if (section == NULL) {
    return;
  }
  free(section->pivots);
  free(section->pivot_to_index);
  free(section->basis);
  free(section->preimages);
  *section = (rw_linear_section_t){0};
}

bool rw_linear_section_build(const qsop_instance_t *qsop,
                             const qsop_rankwidth_decomposition_t *decomposition,
                             uint32_t node, const uint64_t *adj, rw_linear_section_t *out,
                             qsop_error_t *error) {
  if (qsop == NULL || decomposition == NULL || out == NULL || node >= decomposition->nnodes ||
      (qsop->nedges != 0U && adj == NULL)) {
    qsop_set_error(error, "invalid rankwidth linear-section argument");
    return false;
  }
  *out = (rw_linear_section_t){.nbits = qsop->nvars, .words = decomposition->words};
  const size_t words = decomposition->words;
  const size_t rows_size = (size_t)(qsop->nvars == 0 ? 1U : qsop->nvars) *
                           (words == 0 ? 1U : words);
  out->pivots = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*out->pivots));
  out->pivot_to_index = malloc((size_t)(qsop->nvars == 0 ? 1U : qsop->nvars) *
                               sizeof(*out->pivot_to_index));
  out->basis = calloc(rows_size, sizeof(*out->basis));
  out->preimages = calloc(rows_size, sizeof(*out->preimages));
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  uint64_t *row = calloc(words == 0 ? 1U : words, sizeof(*row));
  uint64_t *preimage = calloc(words == 0 ? 1U : words, sizeof(*preimage));
  if (out->pivots == NULL || out->pivot_to_index == NULL || out->basis == NULL ||
      out->preimages == NULL || outside == NULL || row == NULL || preimage == NULL) {
    free(outside);
    free(row);
    free(preimage);
    rw_linear_section_free(out);
    qsop_set_error(error, "out of memory while building a rankwidth linear section");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    out->pivot_to_index[v] = -1;
  }
  rw_fill_all_vars(outside, qsop->nvars, words);
  qsop_bitset_and_not(outside, node_vars_const(decomposition, node), words);

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!qsop_bitset_get(node_vars_const(decomposition, node), v)) {
      continue;
    }
    qsop_bitset_zero(row, words);
    if (adj != NULL) {
      qsop_bitset_copy(row, qsop_bitset_const_row(adj, words, v), words);
      qsop_bitset_and(row, outside, words);
    }
    qsop_bitset_zero(preimage, words);
    qsop_bitset_set(preimage, v);
    for (;;) {
      const uint32_t pivot = qpf_first_set(row, words, qsop->nvars);
      if (pivot == UINT32_MAX) {
        break;
      }
      const int32_t existing = out->pivot_to_index[pivot];
      if (existing < 0) {
        const uint32_t index = out->dim++;
        out->pivots[index] = pivot;
        out->pivot_to_index[pivot] = (int32_t)index;
        qsop_bitset_copy(out->basis + (size_t)index * words, row, words);
        qsop_bitset_copy(out->preimages + (size_t)index * words, preimage, words);
        break;
      }
      qsop_bitset_xor(row, out->basis + (size_t)existing * words, words);
      qsop_bitset_xor(preimage, out->preimages + (size_t)existing * words, words);
    }
  }
  free(outside);
  free(row);
  free(preimage);
  return true;
}

bool rw_linear_section_coord(const rw_linear_section_t *section, const uint64_t *signature,
                             uint64_t *coord, qsop_error_t *error) {
  if (section == NULL || coord == NULL || (section->words != 0U && signature == NULL)) {
    qsop_set_error(error, "invalid rankwidth section-coordinate argument");
    return false;
  }
  const size_t coord_words = ((size_t)section->dim + 63U) / 64U;
  qsop_bitset_zero(coord, coord_words);
  uint64_t *row = calloc(section->words == 0 ? 1U : section->words, sizeof(*row));
  if (row == NULL) {
    qsop_set_error(error, "out of memory while coordinatizing a rankwidth signature");
    return false;
  }
  qsop_bitset_copy(row, signature, section->words);
  bool ok = true;
  for (;;) {
    const uint32_t pivot = qpf_first_set(row, section->words, section->nbits);
    if (pivot == UINT32_MAX) {
      break;
    }
    const int32_t index = section->pivot_to_index[pivot];
    if (index < 0) {
      qsop_set_error(error, "signature is outside the rankwidth node signature space");
      ok = false;
      break;
    }
    qsop_bitset_set(coord, (uint32_t)index);
    qsop_bitset_xor(row, section->basis + (size_t)index * section->words, section->words);
  }
  free(row);
  return ok;
}

void rw_linear_section_apply(const rw_linear_section_t *section, const uint64_t *coord,
                             uint64_t *assignment) {
  if (section == NULL || assignment == NULL) {
    return;
  }
  qsop_bitset_zero(assignment, section->words);
  for (uint32_t i = 0; i < section->dim; i++) {
    if (qsop_bitset_get(coord, i)) {
      qsop_bitset_xor(assignment, section->preimages + (size_t)i * section->words,
                      section->words);
    }
  }
}

static uint32_t crossing_parity(uint32_t nvars, const uint64_t *adj, const uint64_t *left,
                                const uint64_t *right, size_t words) {
  uint32_t parity = 0;
  for (size_t word = 0; word < words; word++) {
    uint64_t bits = left[word];
    while (bits != 0U) {
      const uint32_t v = (uint32_t)(word * 64U) + rw_ctz_u64(bits);
      if (v >= nvars) {
        break;
      }
      parity ^= qsop_bitset_popcount_intersection(
                    qsop_bitset_const_row(adj, words, v), right, words) &
                1U;
      bits &= bits - 1U;
    }
  }
  return parity & 1U;
}

bool rw_qpf_crossing_matrix(const qsop_instance_t *qsop,
                            const rw_linear_section_t *left,
                            const rw_linear_section_t *right, const uint64_t *adj,
                            uint64_t **out_rows, size_t *out_words, qsop_error_t *error) {
  if (qsop == NULL || left == NULL || right == NULL || out_rows == NULL || out_words == NULL ||
      (qsop->nedges != 0U && adj == NULL)) {
    qsop_set_error(error, "invalid rankwidth QPF crossing-matrix argument");
    return false;
  }
  *out_rows = NULL;
  *out_words = ((size_t)right->dim + 63U) / 64U;
  const size_t allocation_words = *out_words == 0 ? 1U : *out_words;
  uint64_t *rows = calloc((size_t)(left->dim == 0 ? 1U : left->dim) * allocation_words,
                          sizeof(*rows));
  if (rows == NULL) {
    qsop_set_error(error, "out of memory while allocating a rankwidth QPF crossing matrix");
    return false;
  }
  if (adj != NULL) {
    for (uint32_t i = 0; i < left->dim; i++) {
      for (uint32_t j = 0; j < right->dim; j++) {
        if (crossing_parity(qsop->nvars, adj,
                            left->preimages + (size_t)i * left->words,
                            right->preimages + (size_t)j * right->words, left->words)) {
          qsop_bitset_set(rows + (size_t)i * allocation_words, j);
        }
      }
    }
  }
  *out_rows = rows;
  return true;
}

static uint64_t qpf_gcd(uint64_t left, uint64_t right) {
  while (right != 0U) {
    const uint64_t remainder = left % right;
    left = right;
    right = remainder;
  }
  return left;
}

static bool rw_qpf_order(uint64_t r, uint32_t *out, qsop_error_t *error) {
  if (r == 0U || r > UINT32_MAX) {
    qsop_set_error(error, "rankwidth QPF requires R <= 2^32-1");
    return false;
  }
  const uint64_t multiplier = 8U / qpf_gcd(r, 8U);
  if (r > UINT32_MAX / multiplier) {
    qsop_set_error(error, "rankwidth QPF phase order exceeds 32 bits");
    return false;
  }
  *out = (uint32_t)(r * multiplier);
  return true;
}

static void free_induced_instance(qsop_instance_t *sub) {
  free(sub->unary);
  free(sub->edge_u);
  free(sub->edge_v);
  *sub = (qsop_instance_t){0};
}

static bool build_induced_instance(const qsop_instance_t *qsop, const uint64_t *vertices,
                                   size_t words, qsop_instance_t *sub, uint32_t **globals,
                                   qsop_error_t *error) {
  uint32_t *map = malloc((size_t)(qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*map));
  uint32_t *vars = malloc((size_t)(qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*vars));
  if (map == NULL || vars == NULL) {
    free(map);
    free(vars);
    qsop_set_error(error, "out of memory while building a rankwidth QPF node instance");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    map[v] = UINT32_MAX;
  }
  uint32_t nvars = 0;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (qpf_bit_get(vertices, v)) {
      map[v] = nvars;
      vars[nvars++] = v;
    }
  }
  uint32_t nedges = 0;
  for (uint32_t edge = 0; edge < qsop->nedges; edge++) {
    if (map[qsop->edge_u[edge]] != UINT32_MAX && map[qsop->edge_v[edge]] != UINT32_MAX) {
      nedges++;
    }
  }
  *sub = (qsop_instance_t){.r = qsop->r, .nvars = nvars, .norm_h = 0U, .constant = 0U,
                           .nedges = nedges};
  sub->unary = calloc(nvars == 0 ? 1U : nvars, sizeof(*sub->unary));
  sub->edge_u = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_u));
  sub->edge_v = calloc(nedges == 0 ? 1U : nedges, sizeof(*sub->edge_v));
  if (sub->unary == NULL || sub->edge_u == NULL || sub->edge_v == NULL) {
    free(map);
    free(vars);
    free_induced_instance(sub);
    qsop_set_error(error, "out of memory while allocating a rankwidth QPF node instance");
    return false;
  }
  for (uint32_t i = 0; i < nvars; i++) {
    sub->unary[i] = qsop->unary[vars[i]];
  }
  uint32_t out_edge = 0;
  for (uint32_t edge = 0; edge < qsop->nedges; edge++) {
    if (map[qsop->edge_u[edge]] != UINT32_MAX && map[qsop->edge_v[edge]] != UINT32_MAX) {
      sub->edge_u[out_edge] = map[qsop->edge_u[edge]];
      sub->edge_v[out_edge] = map[qsop->edge_v[edge]];
      out_edge++;
    }
  }
  (void)words;
  free(map);
  *globals = vars;
  return true;
}

static bool rw_qpf_rebuild(const qsop_instance_t *qsop,
                           const qsop_rankwidth_decomposition_t *decomposition, uint32_t node,
                           uint32_t target_mode, uint64_t budget,
                           const qsop_qpf_scalar_t *unit, rw_qpf_list_t *out,
                           qsop_error_t *error) {
  qsop_instance_t sub = {0};
  uint32_t *globals = NULL;
  qsop_qpf_term_list_t decomposition_terms = {0};
  if (!build_induced_instance(qsop, node_vars_const(decomposition, node), decomposition->words,
                              &sub, &globals, error) ||
      !qsop_qpf_magic_decompose(&sub, target_mode, budget, unit, &decomposition_terms, error)) {
    free(globals);
    free_induced_instance(&sub);
    return false;
  }
  out->terms = calloc((size_t)decomposition_terms.len, sizeof(*out->terms));
  if (out->terms == NULL) {
    free(globals);
    free_induced_instance(&sub);
    qsop_qpf_term_list_free(&decomposition_terms);
    qsop_set_error(error, "out of memory while allocating a rebuilt rankwidth QPF list");
    return false;
  }
  out->len = decomposition_terms.len;
  out->nvars = qsop->nvars;
  out->words = decomposition->words;
  bool ok = true;
  for (uint64_t i = 0; i < out->len && ok; i++) {
    out->terms[i].qpf = decomposition_terms.terms[i];
    decomposition_terms.terms[i] = (qsop_qpf_t){0};
    out->terms[i].variables = malloc((size_t)(sub.nvars == 0 ? 1U : sub.nvars) *
                                     sizeof(*out->terms[i].variables));
    out->terms[i].fixed_assignment = calloc(decomposition->words == 0 ? 1U : decomposition->words,
                                            sizeof(*out->terms[i].fixed_assignment));
    if (out->terms[i].variables == NULL || out->terms[i].fixed_assignment == NULL) {
      qsop_set_error(error, "out of memory while allocating a rebuilt rankwidth QPF term");
      ok = false;
    } else {
      memcpy(out->terms[i].variables, globals, (size_t)sub.nvars * sizeof(*globals));
    }
  }
  free(globals);
  free_induced_instance(&sub);
  qsop_qpf_term_list_free(&decomposition_terms);
  if (!ok) {
    rw_qpf_list_free(out);
  }
  return ok;
}

static bool rw_qpf_term_product(const qsop_instance_t *qsop, const uint64_t *adj,
                                uint32_t target_mode, const rw_qpf_list_term_t *left,
                                const rw_qpf_list_term_t *right, size_t global_words,
                                rw_qpf_list_term_t *out, qsop_error_t *error) {
  qsop_qpf_scalar_t eps = left->qpf.eps;
  if (!qsop_qpf_scalar_mul(&eps, &right->qpf.eps, error)) {
    return false;
  }
  const uint32_t left_m = left->qpf.m;
  const uint32_t right_m = right->qpf.m;
  if (right_m > UINT32_MAX - left_m || !qsop_qpf_init(&out->qpf, left_m + right_m, &eps, error)) {
    return false;
  }
  out->variables = malloc((size_t)(left_m + right_m == 0 ? 1U : left_m + right_m) *
                          sizeof(*out->variables));
  out->fixed_assignment = calloc(global_words == 0 ? 1U : global_words,
                                 sizeof(*out->fixed_assignment));
  if (out->variables == NULL || out->fixed_assignment == NULL) {
    qsop_set_error(error, "out of memory while joining rankwidth QPF terms");
    return false;
  }
  memcpy(out->variables, left->variables, (size_t)left_m * sizeof(*out->variables));
  memcpy(out->variables + left_m, right->variables, (size_t)right_m * sizeof(*out->variables));
  for (size_t word = 0; word < global_words; word++) {
    out->fixed_assignment[word] = left->fixed_assignment[word] ^ right->fixed_assignment[word];
  }
  for (uint32_t i = 0; i < left_m; i++) {
    qsop_qpf_set_linear(&out->qpf, i, left->qpf.ell[i]);
    for (uint32_t j = i + 1U; j < left_m; j++) {
      if (qpf_bit_get(left->qpf.quad + (size_t)i * left->qpf.words, j)) {
        qsop_qpf_toggle_quadratic(&out->qpf, i, j);
      }
    }
  }
  for (uint32_t i = 0; i < right_m; i++) {
    qsop_qpf_set_linear(&out->qpf, left_m + i, right->qpf.ell[i]);
    for (uint32_t j = i + 1U; j < right_m; j++) {
      if (qpf_bit_get(right->qpf.quad + (size_t)i * right->qpf.words, j)) {
        qsop_qpf_toggle_quadratic(&out->qpf, left_m + i, left_m + j);
      }
    }
  }
  uint64_t *constraint = calloc(out->qpf.words == 0 ? 1U : out->qpf.words,
                                sizeof(*constraint));
  if (constraint == NULL) {
    qsop_set_error(error, "out of memory while joining rankwidth QPF constraints");
    return false;
  }
  for (uint32_t row = 0; row < left->qpf.aff_len; row++) {
    qsop_bitset_zero(constraint, out->qpf.words);
    for (uint32_t bit = 0; bit < left_m; bit++) {
      if (qpf_bit_get(left->qpf.aff_rows + (size_t)row * left->qpf.words, bit)) {
        qsop_bitset_set(constraint, bit);
      }
    }
    if (!qsop_qpf_add_constraint(&out->qpf, constraint, left->qpf.aff_rhs[row], error)) {
      free(constraint);
      return false;
    }
  }
  for (uint32_t row = 0; row < right->qpf.aff_len; row++) {
    qsop_bitset_zero(constraint, out->qpf.words);
    for (uint32_t bit = 0; bit < right_m; bit++) {
      if (qpf_bit_get(right->qpf.aff_rows + (size_t)row * right->qpf.words, bit)) {
        qsop_bitset_set(constraint, left_m + bit);
      }
    }
    if (!qsop_qpf_add_constraint(&out->qpf, constraint, right->qpf.aff_rhs[row], error)) {
      free(constraint);
      return false;
    }
  }
  free(constraint);

  if ((target_mode & 1U) != 0U && adj != NULL) {
    if (crossing_parity(qsop->nvars, adj, left->fixed_assignment, right->fixed_assignment,
                        global_words) != 0U &&
        !qsop_qpf_scalar_mul_root(&out->qpf.eps, out->qpf.eps.order / 2U, error)) {
      return false;
    }
    for (uint32_t i = 0; i < left_m; i++) {
      const uint32_t v = left->variables[i];
      if ((qsop_bitset_popcount_intersection(qsop_bitset_const_row(adj, global_words, v),
                                             right->fixed_assignment, global_words) &
           1U) != 0U) {
        qsop_qpf_set_linear(&out->qpf, i, (uint8_t)(out->qpf.ell[i] + 2U));
      }
    }
    for (uint32_t j = 0; j < right_m; j++) {
      const uint32_t v = right->variables[j];
      if ((qsop_bitset_popcount_intersection(qsop_bitset_const_row(adj, global_words, v),
                                             left->fixed_assignment, global_words) &
           1U) != 0U) {
        qsop_qpf_set_linear(&out->qpf, left_m + j,
                            (uint8_t)(out->qpf.ell[left_m + j] + 2U));
      }
    }
    for (uint32_t i = 0; i < left_m; i++) {
      const uint64_t *row = qsop_bitset_const_row(adj, global_words, left->variables[i]);
      for (uint32_t j = 0; j < right_m; j++) {
        if (qpf_bit_get(row, right->variables[j])) {
          qsop_qpf_toggle_quadratic(&out->qpf, i, left_m + j);
        }
      }
    }
  }
  return true;
}

static bool rw_qpf_join(const qsop_instance_t *qsop, const uint64_t *adj, uint32_t target_mode,
                        const rw_qpf_list_t *left, const rw_qpf_list_t *right, uint64_t budget,
                        rw_qpf_list_t *out, qsop_error_t *error) {
  if (left->len != 0U && right->len > budget / left->len) {
    qsop_set_error(error, "rankwidth QPF join exceeds the term budget");
    return false;
  }
  out->len = left->len * right->len;
  out->nvars = qsop->nvars;
  out->words = left->words;
  out->terms = calloc((size_t)out->len, sizeof(*out->terms));
  if (out->terms == NULL) {
    qsop_set_error(error, "out of memory while allocating a rankwidth QPF join");
    return false;
  }
  uint64_t index = 0;
  for (uint64_t i = 0; i < left->len; i++) {
    for (uint64_t j = 0; j < right->len; j++, index++) {
      if (!rw_qpf_term_product(qsop, adj, target_mode, &left->terms[i], &right->terms[j],
                               out->words, &out->terms[index], error)) {
        rw_qpf_list_free(out);
        return false;
      }
    }
  }
  return true;
}

static void rw_qpf_signature(const qsop_instance_t *qsop, const uint64_t *adj,
                             const uint64_t *node_set, const uint64_t *assignment, size_t words,
                             uint64_t *signature) {
  qsop_bitset_zero(signature, words);
  if (adj != NULL) {
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if (qpf_bit_get(assignment, v)) {
        qsop_bitset_xor(signature, qsop_bitset_const_row(adj, words, v), words);
      }
    }
  }
  qsop_bitset_and_not(signature, node_set, words);
}

static bool rw_qpf_collapse(const qsop_instance_t *qsop,
                            const qsop_rankwidth_decomposition_t *decomposition,
                            uint32_t node, const uint64_t *adj, const rw_qpf_list_t *source,
                            const rw_linear_section_t *section, rw_qpf_list_t *out,
                            qsop_error_t *error) {
  if (section->dim >= 64U) {
    qsop_set_error(error, "rankwidth QPF point collapse needs fewer than 64 signature bits");
    return false;
  }
  out->len = UINT64_C(1) << section->dim;
  out->nvars = qsop->nvars;
  out->words = decomposition->words;
  out->terms = calloc((size_t)out->len, sizeof(*out->terms));
  uint64_t *signature = calloc(out->words == 0 ? 1U : out->words, sizeof(*signature));
  uint64_t *coordinate = calloc(1U, sizeof(*coordinate));
  if (out->terms == NULL || signature == NULL || coordinate == NULL) {
    free(signature);
    free(coordinate);
    rw_qpf_list_free(out);
    qsop_set_error(error, "out of memory while collapsing a rankwidth QPF list");
    return false;
  }

  bool ok = true;
  for (uint64_t alpha = 0; alpha < out->len && ok; alpha++) {
    qsop_qpf_scalar_t sum = source->terms[0].qpf.eps;
    scalar_set_zero(&sum);
    for (uint64_t term_index = 0; term_index < source->len && ok; term_index++) {
      const rw_qpf_list_term_t *term = &source->terms[term_index];
      rw_qpf_signature(qsop, adj, node_vars_const(decomposition, node), term->fixed_assignment,
                       out->words, signature);
      if (!rw_linear_section_coord(section, signature, coordinate, error)) {
        ok = false;
        break;
      }
      const uint64_t fixed_coordinate = coordinate[0];
      uint64_t *variable_coordinates = calloc(term->qpf.m == 0 ? 1U : term->qpf.m,
                                              sizeof(*variable_coordinates));
      uint64_t *unit_assignment = calloc(out->words == 0 ? 1U : out->words,
                                         sizeof(*unit_assignment));
      uint64_t *constraint = calloc(term->qpf.words == 0 ? 1U : term->qpf.words,
                                    sizeof(*constraint));
      if (variable_coordinates == NULL || unit_assignment == NULL || constraint == NULL) {
        free(variable_coordinates);
        free(unit_assignment);
        free(constraint);
        qsop_set_error(error, "out of memory while building QPF collapse constraints");
        ok = false;
        break;
      }
      for (uint32_t variable = 0; variable < term->qpf.m && ok; variable++) {
        qsop_bitset_zero(unit_assignment, out->words);
        qsop_bitset_set(unit_assignment, term->variables[variable]);
        rw_qpf_signature(qsop, adj, node_vars_const(decomposition, node), unit_assignment,
                         out->words, signature);
        ok = rw_linear_section_coord(section, signature, coordinate, error);
        variable_coordinates[variable] = coordinate[0];
      }
      qsop_qpf_t constrained = {0};
      if (ok) {
        ok = qsop_qpf_clone(&term->qpf, &constrained, error);
      }
      for (uint32_t bit = 0; bit < section->dim && ok; bit++) {
        qsop_bitset_zero(constraint, term->qpf.words);
        for (uint32_t variable = 0; variable < term->qpf.m; variable++) {
          if (((variable_coordinates[variable] >> bit) & 1U) != 0U) {
            qsop_bitset_set(constraint, variable);
          }
        }
        const uint64_t rhs = ((alpha ^ fixed_coordinate) >> bit) & 1U;
        ok = qsop_qpf_add_constraint(&constrained, constraint, rhs, error);
      }
      qsop_qpf_scalar_t value = {0};
      if (ok) {
        ok = qsop_qpf_total_sum(&constrained, &value, error) &&
             qsop_qpf_scalar_add(&sum, &value, error);
      }
      qsop_qpf_free(&constrained);
      free(variable_coordinates);
      free(unit_assignment);
      free(constraint);
    }
    if (!ok) {
      break;
    }
    if (!qsop_qpf_init(&out->terms[alpha].qpf, 0U, &sum, error)) {
      ok = false;
      break;
    }
    out->terms[alpha].variables = calloc(1U, sizeof(*out->terms[alpha].variables));
    out->terms[alpha].fixed_assignment = calloc(out->words == 0 ? 1U : out->words,
                                                sizeof(*out->terms[alpha].fixed_assignment));
    coordinate[0] = alpha;
    if (out->terms[alpha].variables == NULL || out->terms[alpha].fixed_assignment == NULL) {
      qsop_set_error(error, "out of memory while storing a collapsed rankwidth QPF point");
      ok = false;
      break;
    }
    rw_linear_section_apply(section, coordinate, out->terms[alpha].fixed_assignment);
  }
  free(signature);
  free(coordinate);
  if (!ok) {
    rw_qpf_list_free(out);
  }
  return ok;
}

static uint32_t rw_qpf_node_magic(const qsop_instance_t *qsop,
                                  const qsop_rankwidth_decomposition_t *decomposition,
                                  uint32_t node, uint32_t target_mode) {
  const uint64_t mode = target_mode % qsop->r;
  uint32_t magic = 0;
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    if (!qpf_bit_get(node_vars_const(decomposition, node), v)) {
      continue;
    }
    const uint64_t phase = (uint64_t)(((qsop_qpf_u128_t)mode * qsop->unary[v]) % qsop->r);
    if (((qsop_qpf_u128_t)4U * phase) % qsop->r != 0U) {
      magic++;
    }
  }
  return magic;
}

static uint64_t rw_qpf_node_rebuild_bound(const qsop_instance_t *qsop,
                                          const qsop_rankwidth_decomposition_t *decomposition,
                                          uint32_t node, uint32_t target_mode) {
  const uint32_t magic = rw_qpf_node_magic(qsop, decomposition, node, target_mode);
  if (qsop->r != 8U || magic < 6U) {
    return qsop_qpf_naive_term_bound(magic);
  }
  uint64_t bound = 1U;
  for (uint32_t block = 0; block < magic / 6U; block++) {
    if (bound > UINT64_MAX / 6U) {
      return UINT64_MAX;
    }
    bound *= 6U;
  }
  return bound > (UINT64_MAX >> (magic % 6U)) ? UINT64_MAX : bound << (magic % 6U);
}

static bool rw_qpf_forecast(const qsop_instance_t *qsop,
                            const qsop_rankwidth_decomposition_t *decomposition,
                            const uint64_t *adj, uint32_t target_mode, uint64_t budget,
                            uint64_t *bounds, rw_linear_section_t *sections,
                            qsop_error_t *error) {
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node = decomposition->postorder[i];
    if (!rw_linear_section_build(qsop, decomposition, node, adj, &sections[node], error)) {
      return false;
    }
    const uint64_t rebuild = rw_qpf_node_rebuild_bound(qsop, decomposition, node, target_mode);
    uint64_t source = rebuild;
    if (decomposition->nodes[node].kind == RW_NODE_JOIN) {
      const rw_node_t *join = &decomposition->nodes[node];
      const uint64_t left = bounds[join->left];
      const uint64_t right = bounds[join->right];
      const uint64_t joined = left == 0U || right <= UINT64_MAX / left ? left * right : UINT64_MAX;
      if (joined < source) {
        source = joined;
      }
    }
    if (source == UINT64_MAX || source > budget) {
      return true;
    }
    const uint64_t point = sections[node].dim >= 64U
                               ? UINT64_MAX
                               : UINT64_C(1) << sections[node].dim;
    bounds[node] = point < source ? point : source;
  }
  return true;
}

bool rw_qpf_hybrid_single_mode(const qsop_instance_t *qsop,
                               const qsop_rankwidth_decomposition_t *decomposition,
                               const uint64_t *adj, uint32_t target_mode, uint64_t max_terms,
                               bool *out_handled, qsop_amplitude_t *out,
                               qsop_solve_stats_t *stats, qsop_error_t *error) {
  if (out_handled == NULL || out == NULL) {
    qsop_set_error(error, "invalid rankwidth QPF hybrid output");
    return false;
  }
  *out_handled = false;
  const uint64_t budget = max_terms == 0U ? RW_QPF_DEFAULT_MAX_TERMS : max_terms;
  uint32_t order = 0;
  if (!rw_qpf_order(qsop->r, &order, error)) {
    return false;
  }
  qsop_qpf_scalar_t unit = {0};
  qsop_qpf_scalar_complex(&unit, order, 0U);
  uint64_t *bounds = calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes,
                            sizeof(*bounds));
  rw_linear_section_t *sections =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*sections));
  rw_qpf_list_t *lists =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*lists));
  if (bounds == NULL || sections == NULL || lists == NULL) {
    free(bounds);
    free(sections);
    free(lists);
    qsop_set_error(error, "out of memory while allocating rankwidth QPF hybrid state");
    return false;
  }
  bool ok = rw_qpf_forecast(qsop, decomposition, adj, target_mode, budget, bounds, sections, error);
  if (!ok || bounds[decomposition->root] == 0U) {
    for (uint32_t node = 0; node < decomposition->nnodes; node++) {
      rw_linear_section_free(&sections[node]);
    }
    free(bounds);
    free(sections);
    free(lists);
    return ok;
  }

  for (uint32_t i = 0; i < decomposition->postorder_len && ok; i++) {
    const uint32_t node = decomposition->postorder[i];
    const rw_node_t *entry = &decomposition->nodes[node];
    const uint64_t rebuild_bound =
        rw_qpf_node_rebuild_bound(qsop, decomposition, node, target_mode);
    uint64_t join_bound = UINT64_MAX;
    if (entry->kind == RW_NODE_JOIN && lists[entry->left].len != 0U &&
        lists[entry->right].len <= UINT64_MAX / lists[entry->left].len) {
      join_bound = lists[entry->left].len * lists[entry->right].len;
    }
    rw_qpf_list_t source = {0};
    if (entry->kind == RW_NODE_JOIN && join_bound <= rebuild_bound) {
      ok = rw_qpf_join(qsop, adj, target_mode, &lists[entry->left], &lists[entry->right], budget,
                       &source, error);
      if (ok && stats != NULL) {
        stats->qpf_joins++;
      }
    } else {
      ok = rw_qpf_rebuild(qsop, decomposition, node, target_mode, budget, &unit, &source, error);
      if (ok && stats != NULL) {
        stats->qpf_rebuilds++;
        stats->qpf_decompositions++;
      }
    }
    if (!ok) {
      rw_qpf_list_free(&source);
      break;
    }
    const uint64_t point_bound = sections[node].dim >= 64U
                                     ? UINT64_MAX
                                     : UINT64_C(1) << sections[node].dim;
    /* Point form wins ties: this preserves the established table representation whenever it is
     * no larger, and guarantees the zero-dimensional root is actually summed out. */
    if (point_bound <= source.len) {
      ok = rw_qpf_collapse(qsop, decomposition, node, adj, &source, &sections[node], &lists[node],
                           error);
      rw_qpf_list_free(&source);
      if (ok && stats != NULL) {
        stats->qpf_collapses++;
      }
    } else {
      lists[node] = source;
    }
    if (ok && stats != NULL) {
      stats->qpf_terms += lists[node].len;
      if (lists[node].len > stats->qpf_max_terms) {
        stats->qpf_max_terms = lists[node].len;
      }
      const uint32_t magic = rw_qpf_node_magic(qsop, decomposition, node, target_mode);
      if (magic > stats->qpf_magic_vertices) {
        stats->qpf_magic_vertices = magic;
      }
    }
    if (entry->kind == RW_NODE_JOIN) {
      rw_qpf_list_free(&lists[entry->left]);
      if (entry->right != entry->left) {
        rw_qpf_list_free(&lists[entry->right]);
      }
    }
  }
  if (ok) {
    rw_qpf_list_t *root = &lists[decomposition->root];
    if (root->len != 1U || root->terms[0].qpf.m != 0U) {
      qsop_set_error(error, "rankwidth QPF root did not collapse to one scalar");
      ok = false;
    } else {
      qsop_qpf_scalar_t scalar = root->terms[0].qpf.eps;
      const uint64_t constant_phase =
          (uint64_t)(((qsop_qpf_u128_t)(target_mode % qsop->r) *
                      (qsop->constant % qsop->r)) %
                     qsop->r);
      ok = qsop_qpf_scalar_mul_root(&scalar, constant_phase * (order / qsop->r), error);
      if (ok) {
        out->re = scalar.re;
        out->im = scalar.im;
        out->scale_exp2 = scalar.scale_exp2;
        out->numeric_error_bound = scalar.numeric_error_bound;
        *out_handled = true;
      }
    }
  }
  for (uint32_t node = 0; node < decomposition->nnodes; node++) {
    rw_qpf_list_free(&lists[node]);
    rw_linear_section_free(&sections[node]);
  }
  free(bounds);
  free(sections);
  free(lists);
  return ok;
}
