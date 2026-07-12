/* Rank-decomposition tree construction, generation (balanced/left-deep/min-fill/from-treewidth),
 * and (de)serialization.
 *
 * Part of the rankwidth.c file split (pure movement, no logic changes) -- see
 * rankwidth_internal.h for the shared types and cross-TU declarations. */
#include "../core/qsop_internal.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "dlx4sop/simd.h"
#include "rankwidth_internal.h"
#include "trace.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_decomposition_scores(rw_decomposition_score_t left,
                                        rw_decomposition_score_t right) {
  if (left.table_forecast != right.table_forecast) {
    return left.table_forecast < right.table_forecast ? -1 : 1;
  }
  if (left.join_pair_forecast != right.join_pair_forecast) {
    return left.join_pair_forecast < right.join_pair_forecast ? -1 : 1;
  }
  if (left.cutrank_width != right.cutrank_width) {
    return left.cutrank_width < right.cutrank_width ? -1 : 1;
  }
  return 0;
}
static void set_parse_error(qsop_error_t *error, const char *path, size_t line, const char *fmt,
                            ...) {
  if (error == NULL) {
    return;
  }

  error->path = path;
  error->line = line;
  error->column = 1;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
}
bool rw_extract_left_deep_order(const qsop_rankwidth_decomposition_t *decomposition,
                                uint32_t *order) {
  if (decomposition == NULL || order == NULL) {
    return false;
  }
  if (decomposition->nvars == 0) {
    return true;
  }
  uint32_t pos = decomposition->nvars;
  uint32_t node = decomposition->root;
  for (;;) {
    if (node >= decomposition->nnodes) {
      return false;
    }
    const rw_node_t *entry = &decomposition->nodes[node];
    if (entry->kind == RW_NODE_LEAF) {
      if (pos != 1U) {
        return false;
      }
      order[0] = entry->var;
      return true;
    }
    if (entry->kind != RW_NODE_JOIN || entry->right >= decomposition->nnodes) {
      return false;
    }
    const rw_node_t *right = &decomposition->nodes[entry->right];
    if (right->kind != RW_NODE_LEAF || pos == 0) {
      return false;
    }
    order[--pos] = right->var;
    node = entry->left;
  }
}
static bool parse_u32_token(const char *text, uint32_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    return false;
  }
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)value;
  return true;
}
static bool validate_decomposition_dfs(qsop_rankwidth_decomposition_t *decomposition, uint32_t node,
                                       uint8_t *state, uint8_t *seen_var, qsop_error_t *error) {
  if (node >= decomposition->nnodes) {
    qsop_set_error(error, "rankwidth decomposition references node outside range");
    return false;
  }
  if (state[node] == 1U) {
    qsop_set_error(error, "rankwidth decomposition contains a cycle");
    return false;
  }
  if (state[node] == 2U) {
    return true;
  }

  rw_node_t *entry = &decomposition->nodes[node];
  if (entry->kind == RW_NODE_UNDEFINED) {
    qsop_set_error(error, "rankwidth decomposition references undefined node %" PRIu32, node);
    return false;
  }
  state[node] = 1U;
  if (entry->kind == RW_NODE_LEAF) {
    if (entry->var >= decomposition->nvars) {
      qsop_set_error(error, "rankwidth decomposition leaf variable is outside range");
      return false;
    }
    if (seen_var[entry->var] != 0) {
      qsop_set_error(error, "rankwidth decomposition maps variable %" PRIu32 " more than once",
                     entry->var);
      return false;
    }
    seen_var[entry->var] = 1U;
    uint64_t *vars = node_vars(decomposition, node);
    qsop_bitset_zero(vars, decomposition->words);
    qsop_bitset_set(vars, entry->var);
  } else {
    if (!validate_decomposition_dfs(decomposition, entry->left, state, seen_var, error) ||
        !validate_decomposition_dfs(decomposition, entry->right, state, seen_var, error)) {
      return false;
    }
    const uint64_t *left = node_vars_const(decomposition, entry->left);
    const uint64_t *right = node_vars_const(decomposition, entry->right);
    uint64_t *vars = node_vars(decomposition, node);
    for (size_t w = 0; w < decomposition->words; w++) {
      if ((left[w] & right[w]) != 0) {
        qsop_set_error(error, "rankwidth decomposition children are not disjoint");
        return false;
      }
      vars[w] = left[w] | right[w];
    }
    if (qsop_bitset_empty(vars, decomposition->words)) {
      qsop_set_error(error, "rankwidth decomposition children are not disjoint");
      return false;
    }
  }
  state[node] = 2U;
  decomposition->postorder[decomposition->postorder_len++] = node;
  return true;
}
static bool validate_decomposition(qsop_rankwidth_decomposition_t *decomposition,
                                   qsop_error_t *error) {
  decomposition->postorder_len = 0;
  memset(decomposition->node_vars, 0,
         (size_t)decomposition->nnodes * decomposition->words * sizeof(*decomposition->node_vars));

  uint8_t *state = calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*state));
  uint8_t *seen_var =
      calloc(decomposition->nvars == 0 ? 1U : decomposition->nvars, sizeof(*seen_var));
  if (state == NULL || seen_var == NULL) {
    free(state);
    free(seen_var);
    qsop_set_error(error, "out of memory while validating rankwidth decomposition");
    return false;
  }

  const bool ok =
      validate_decomposition_dfs(decomposition, decomposition->root, state, seen_var, error);
  uint64_t *all = calloc(decomposition->words == 0 ? 1U : decomposition->words, sizeof(*all));
  if (all == NULL) {
    free(state);
    free(seen_var);
    qsop_set_error(error, "out of memory while validating rankwidth decomposition");
    return false;
  }
  for (uint32_t v = 0; v < decomposition->nvars; v++) {
    qsop_bitset_set(all, v);
  }
  if (ok && !qsop_bitset_equal(node_vars_const(decomposition, decomposition->root), all,
                               decomposition->words)) {
    qsop_set_error(error, "rankwidth decomposition root does not cover every variable");
    free(all);
    free(state);
    free(seen_var);
    return false;
  }
  for (uint32_t v = 0; ok && v < decomposition->nvars; v++) {
    if (seen_var[v] == 0) {
      qsop_set_error(error, "rankwidth decomposition does not include variable %" PRIu32, v);
      free(all);
      free(state);
      free(seen_var);
      return false;
    }
  }

  free(all);
  free(state);
  free(seen_var);
  return ok;
}
bool qsop_rankwidth_decomposition_parse_file(FILE *file, const char *path, uint32_t nvars,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error) {
  if (file == NULL || out == NULL) {
    qsop_set_error(error, "internal error: null rankwidth decomposition parse argument");
    return false;
  }
  *out = NULL;

  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    qsop_set_error(error, "out of memory while allocating rankwidth decomposition");
    return false;
  }

  char line[512];
  size_t line_number = 0;
  bool saw_header = false;
  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') {
      cursor++;
    }
    if (*cursor == '\0' || *cursor == '\n' || *cursor == 'c' || *cursor == '#') {
      continue;
    }

    char tag[32] = {0};
    char a[64] = {0};
    char b[64] = {0};
    char c[64] = {0};
    char d[64] = {0};
    const int fields = sscanf(cursor, "%31s %63s %63s %63s %63s", tag, a, b, c, d);
    if (fields <= 0) {
      continue;
    }
    if (strcmp(tag, "p") == 0) {
      uint32_t header_nvars = 0;
      uint32_t nnodes = 0;
      uint32_t root = 0;
      if (saw_header || fields != 5 || strcmp(a, "rwdec") != 0 ||
          !parse_u32_token(b, &header_nvars) || !parse_u32_token(c, &nnodes) ||
          !parse_u32_token(d, &root)) {
        set_parse_error(error, path, line_number,
                        "expected header: p rwdec <variables> <nodes> <root>");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      if (header_nvars != nvars) {
        set_parse_error(error, path, line_number,
                        "rankwidth decomposition variable count does not match QSOP");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      if (nnodes == 0 || root >= nnodes) {
        set_parse_error(error, path, line_number, "invalid rankwidth decomposition size or root");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      decomposition->nvars = nvars;
      decomposition->nnodes = nnodes;
      decomposition->root = root;
      decomposition->words = qsop_bitset_words(nvars);
      decomposition->nodes = calloc(nnodes, sizeof(*decomposition->nodes));
      decomposition->node_vars =
          calloc((size_t)nnodes * decomposition->words, sizeof(*decomposition->node_vars));
      decomposition->postorder = calloc(nnodes, sizeof(*decomposition->postorder));
      if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
          decomposition->postorder == NULL) {
        qsop_set_error(error, "out of memory while allocating rankwidth decomposition nodes");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      saw_header = true;
      continue;
    }

    if (!saw_header) {
      set_parse_error(error, path, line_number, "rankwidth decomposition missing header");
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }

    uint32_t node = 0;
    if (!parse_u32_token(a, &node) || node >= decomposition->nnodes ||
        decomposition->nodes[node].kind != RW_NODE_UNDEFINED) {
      set_parse_error(error, path, line_number, "invalid or duplicate rankwidth node id");
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (strcmp(tag, "l") == 0) {
      uint32_t var = 0;
      if (fields != 3 || !parse_u32_token(b, &var)) {
        set_parse_error(error, path, line_number, "expected leaf: l <node> <variable>");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_LEAF,
          .var = var,
      };
    } else if (strcmp(tag, "j") == 0) {
      uint32_t left = 0;
      uint32_t right = 0;
      if (fields != 4 || !parse_u32_token(b, &left) || !parse_u32_token(c, &right)) {
        set_parse_error(error, path, line_number, "expected join: j <node> <left> <right>");
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = left,
          .right = right,
      };
    } else {
      set_parse_error(error, path, line_number, "unsupported rankwidth decomposition record");
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
  }

  if (!saw_header) {
    qsop_set_error(error, "rankwidth decomposition missing header");
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }

  *out = decomposition;
  return true;
}
bool qsop_rankwidth_decomposition_write_file(FILE *file,
                                             const qsop_rankwidth_decomposition_t *decomposition,
                                             qsop_error_t *error) {
  if (file == NULL || decomposition == NULL) {
    qsop_set_error(error, "internal error: null rankwidth decomposition write argument");
    return false;
  }

  fprintf(file, "p rwdec %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", decomposition->nvars,
          decomposition->nnodes, decomposition->root);

  for (uint32_t node = 0; node < decomposition->nnodes; node++) {
    const rw_node_t *n = &decomposition->nodes[node];
    if (n->kind == RW_NODE_LEAF) {
      fprintf(file, "l %" PRIu32 " %" PRIu32 "\n", node, n->var);
    } else if (n->kind == RW_NODE_JOIN) {
      fprintf(file, "j %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", node, n->left, n->right);
    }
  }

  if (ferror(file)) {
    qsop_set_error(error, "write error while serializing rankwidth decomposition");
    return false;
  }
  return true;
}
static uint64_t bitset_missing_edges_for(uint32_t v, const uint64_t *work, const uint64_t *active,
                                         uint64_t *remaining, size_t words, uint32_t nvars) {
  const uint64_t *neighbors = qsop_bitset_const_row(work, words, v);
  for (size_t w = 0; w < words; w++) {
    remaining[w] = neighbors[w] & active[w];
  }

  uint64_t fill = 0;
  for (size_t word = 0; word < words; word++) {
    uint64_t bits = remaining[word];
    while (bits != 0) {
      const uint32_t bit = qsop_ctz_u64(bits);
      const uint32_t u = (uint32_t)(word * 64U + bit);
      bits &= bits - 1U;
      if (u >= nvars) {
        continue;
      }
      qsop_bitset_clear(remaining, u);
      const uint64_t *u_neighbors = qsop_bitset_const_row(work, words, u);
      for (size_t w = 0; w < words; w++) {
        fill += qsop_popcount_u64(remaining[w] & ~u_neighbors[w]);
      }
    }
  }
  return fill;
}
static bool make_min_fill_order(const qsop_instance_t *qsop, uint32_t *order, qsop_error_t *error) {
  const size_t words = qsop_bitset_words(qsop->nvars);
  uint64_t *work = rw_adjacency_bitsets(qsop, words, error);
  uint64_t *active = calloc(words == 0 ? 1U : words, sizeof(*active));
  uint64_t *scratch = calloc(words == 0 ? 1U : words, sizeof(*scratch));
  if (work == NULL || active == NULL || scratch == NULL) {
    free(work);
    free(active);
    free(scratch);
    qsop_set_error(error, "out of memory while building rankwidth min-fill order");
    return false;
  }
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    qsop_bitset_set(active, v);
  }

  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    bool found = false;
    uint32_t best = 0;
    uint64_t best_fill = UINT64_MAX;
    uint32_t best_degree = UINT32_MAX;
    for (size_t word = 0; word < words; word++) {
      uint64_t bits = active[word];
      while (bits != 0) {
        const uint32_t bit = qsop_ctz_u64(bits);
        const uint32_t v = (uint32_t)(word * 64U + bit);
        bits &= bits - 1U;
        if (v >= qsop->nvars) {
          continue;
        }
        const uint32_t degree =
            qsop_bitset_popcount_intersection(qsop_bitset_const_row(work, words, v), active, words);
        const uint64_t fill =
            bitset_missing_edges_for(v, work, active, scratch, words, qsop->nvars);
        if (!found || fill < best_fill || (fill == best_fill && degree < best_degree)) {
          found = true;
          best = v;
          best_fill = fill;
          best_degree = degree;
        }
      }
    }
    if (!found) {
      free(work);
      free(active);
      free(scratch);
      qsop_set_error(error, "internal error: rankwidth min-fill order stopped early");
      return false;
    }
    order[pos] = best;

    const uint64_t *best_row = qsop_bitset_const_row(work, words, best);
    for (size_t w = 0; w < words; w++) {
      scratch[w] = best_row[w] & active[w];
    }
    qsop_bitset_clear(active, best);
    for (size_t word = 0; word < words; word++) {
      uint64_t bits = scratch[word];
      while (bits != 0) {
        const uint32_t bit = qsop_ctz_u64(bits);
        const uint32_t u = (uint32_t)(word * 64U + bit);
        bits &= bits - 1U;
        if (u >= qsop->nvars) {
          continue;
        }
        uint64_t *u_row = qsop_bitset_row(work, words, u);
        for (size_t w = 0; w < words; w++) {
          u_row[w] |= scratch[w];
          u_row[w] &= active[w];
        }
        qsop_bitset_clear(u_row, u);
      }
    }
  }

  free(work);
  free(active);
  free(scratch);
  return true;
}
static uint32_t build_balanced_nodes(qsop_rankwidth_decomposition_t *decomposition,
                                     const uint32_t *leaf_nodes, uint32_t begin, uint32_t end,
                                     uint32_t *next_join) {
  if (end - begin == 1U) {
    return leaf_nodes[begin];
  }
  const uint32_t mid = begin + (end - begin) / 2U;
  const uint32_t left = build_balanced_nodes(decomposition, leaf_nodes, begin, mid, next_join);
  const uint32_t right = build_balanced_nodes(decomposition, leaf_nodes, mid, end, next_join);
  const uint32_t node = (*next_join)++;
  decomposition->nodes[node] = (rw_node_t){
      .kind = RW_NODE_JOIN,
      .left = left,
      .right = right,
  };
  return node;
}
static void range_bits(const uint64_t *prefix_masks, size_t words, uint32_t begin, uint32_t end,
                       uint64_t *out) {
  const uint64_t *begin_bits = qsop_bitset_const_row(prefix_masks, words, begin);
  const uint64_t *end_bits = qsop_bitset_const_row(prefix_masks, words, end);
  for (size_t w = 0; w < words; w++) {
    out[w] = end_bits[w] & ~begin_bits[w];
  }
}
static bool choose_cut_rank_split(uint32_t nvars, const uint64_t *adj, const uint64_t *prefix_masks,
                                  const uint64_t *all, size_t words, uint32_t begin, uint32_t end,
                                  uint32_t *out, qsop_error_t *error) {
  uint64_t *left = calloc(words == 0 ? 1U : words, sizeof(*left));
  uint64_t *right = calloc(words == 0 ? 1U : words, sizeof(*right));
  uint64_t *outside = calloc(words == 0 ? 1U : words, sizeof(*outside));
  if (left == NULL || right == NULL || outside == NULL) {
    free(left);
    free(right);
    free(outside);
    qsop_set_error(error, "out of memory while choosing rankwidth cut-rank split");
    return false;
  }

  bool found = false;
  uint32_t best_split = begin + 1U;
  uint32_t best_rank = UINT32_MAX;
  uint32_t best_balance = UINT32_MAX;
  uint32_t best_rank_sum = UINT32_MAX;

  for (uint32_t split = begin + 1U; split < end; split++) {
    range_bits(prefix_masks, words, begin, split, left);
    range_bits(prefix_masks, words, split, end, right);
    qsop_bitset_copy(outside, all, words);
    qsop_bitset_and_not(outside, left, words);
    const uint32_t left_rank = rw_cut_rank_bitsets(nvars, adj, left, outside, words, NULL, error);
    if (left_rank == UINT32_MAX) {
      free(left);
      free(right);
      free(outside);
      return false;
    }
    qsop_bitset_copy(outside, all, words);
    qsop_bitset_and_not(outside, right, words);
    const uint32_t right_rank = rw_cut_rank_bitsets(nvars, adj, right, outside, words, NULL, error);
    if (right_rank == UINT32_MAX) {
      free(left);
      free(right);
      free(outside);
      return false;
    }
    const uint32_t rank = left_rank > right_rank ? left_rank : right_rank;
    const uint32_t rank_sum = left_rank + right_rank;
    const uint32_t left_size = split - begin;
    const uint32_t right_size = end - split;
    const uint32_t balance =
        left_size > right_size ? left_size - right_size : right_size - left_size;

    if (!found || rank < best_rank || (rank == best_rank && balance < best_balance) ||
        (rank == best_rank && balance == best_balance && rank_sum < best_rank_sum)) {
      found = true;
      best_split = split;
      best_rank = rank;
      best_balance = balance;
      best_rank_sum = rank_sum;
    }
  }

  *out = best_split;
  free(left);
  free(right);
  free(outside);
  return true;
}
static uint32_t build_cut_rank_nodes(qsop_rankwidth_decomposition_t *decomposition,
                                     const uint32_t *leaf_nodes, const uint64_t *prefix_masks,
                                     const uint64_t *all, const uint64_t *adj, uint32_t begin,
                                     uint32_t end, uint32_t *next_join, qsop_error_t *error) {
  if (end - begin == 1U) {
    return leaf_nodes[begin];
  }

  uint32_t split = 0;
  if (!choose_cut_rank_split(decomposition->nvars, adj, prefix_masks, all, decomposition->words,
                             begin, end, &split, error)) {
    return UINT32_MAX;
  }
  const uint32_t left = build_cut_rank_nodes(decomposition, leaf_nodes, prefix_masks, all, adj,
                                             begin, split, next_join, error);
  if (left == UINT32_MAX) {
    return UINT32_MAX;
  }
  const uint32_t right = build_cut_rank_nodes(decomposition, leaf_nodes, prefix_masks, all, adj,
                                              split, end, next_join, error);
  if (right == UINT32_MAX) {
    return UINT32_MAX;
  }
  const uint32_t node = (*next_join)++;
  decomposition->nodes[node] = (rw_node_t){
      .kind = RW_NODE_JOIN,
      .left = left,
      .right = right,
  };
  return node;
}
static bool fill_left_deep_from_order(qsop_rankwidth_decomposition_t *decomposition,
                                      const uint32_t *order, qsop_error_t *error) {
  const uint32_t nvars = decomposition->nvars;
  for (uint32_t i = 0; i < nvars; i++) {
    decomposition->nodes[i] = (rw_node_t){.kind = RW_NODE_LEAF, .var = order[i]};
  }
  if (nvars == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t current = 0;
    uint32_t next_join = nvars;
    for (uint32_t i = 1; i < nvars; i++) {
      decomposition->nodes[next_join] =
          (rw_node_t){.kind = RW_NODE_JOIN, .left = current, .right = i};
      current = next_join++;
    }
    decomposition->root = current;
  }
  return validate_decomposition(decomposition, error);
}
static bool generate_left_deep_search(const qsop_instance_t *qsop, const uint32_t *initial_order,
                                      qsop_rankwidth_decomposition_t **out, qsop_error_t *error) {
  const uint32_t nvars = qsop->nvars;
  const size_t words = qsop_bitset_words(nvars);

  qsop_rankwidth_decomposition_t *decomp = calloc(1, sizeof(*decomp));
  uint32_t *order = calloc(nvars == 0 ? 1U : nvars, sizeof(*order));
  if (decomp == NULL || order == NULL) {
    free(decomp);
    free(order);
    qsop_set_error(error, "out of memory in left-deep search");
    return false;
  }
  decomp->nvars = nvars;
  decomp->words = words;
  decomp->nnodes = nvars == 0U ? 0U : 2U * nvars - 1U;
  decomp->nodes = calloc(decomp->nnodes == 0U ? 1U : decomp->nnodes, sizeof(*decomp->nodes));
  decomp->node_vars =
      calloc((decomp->nnodes == 0U ? 1U : decomp->nnodes) * (words == 0U ? 1U : words),
             sizeof(*decomp->node_vars));
  decomp->postorder =
      calloc(decomp->nnodes == 0U ? 1U : decomp->nnodes, sizeof(*decomp->postorder));
  if (decomp->nodes == NULL || decomp->node_vars == NULL || decomp->postorder == NULL) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    qsop_set_error(error, "out of memory in left-deep search nodes");
    return false;
  }

  if (nvars == 0U) {
    free(order);
    *out = decomp;
    return true;
  }

  for (uint32_t i = 0; i < nvars; i++) {
    order[i] = initial_order[i];
  }

  if (!fill_left_deep_from_order(decomp, order, error)) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    return false;
  }

  uint64_t best_max = 0, best_pairs = 0;
  if (!qsop_rankwidth_decomposition_forecast(qsop, decomp, &best_max, &best_pairs, error)) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    return false;
  }

  bool improved = true;
  while (improved) {
    improved = false;
    for (uint32_t i = 0; i + 1U < nvars; i++) {
      uint32_t tmp = order[i];
      order[i] = order[i + 1U];
      order[i + 1U] = tmp;
      if (!fill_left_deep_from_order(decomp, order, error)) {
        qsop_rankwidth_decomposition_free(decomp);
        free(order);
        return false;
      }
      uint64_t cand_max = 0, cand_pairs = 0;
      if (!qsop_rankwidth_decomposition_forecast(qsop, decomp, &cand_max, &cand_pairs, error)) {
        qsop_rankwidth_decomposition_free(decomp);
        free(order);
        return false;
      }
      if (cand_max < best_max || (cand_max == best_max && cand_pairs < best_pairs)) {
        best_max = cand_max;
        best_pairs = cand_pairs;
        improved = true;
      } else {
        tmp = order[i];
        order[i] = order[i + 1U];
        order[i + 1U] = tmp;
      }
    }
  }

  if (!fill_left_deep_from_order(decomp, order, error)) {
    qsop_rankwidth_decomposition_free(decomp);
    free(order);
    return false;
  }

  free(order);
  *out = decomp;
  return true;
}
static bool make_left_deep_generated_decomposition(const qsop_instance_t *qsop,
                                                   qsop_rankwidth_decomposition_t **out,
                                                   qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    qsop_set_error(error, "internal error: null left-deep rankwidth generation argument");
    return false;
  }
  *out = NULL;

  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    qsop_set_error(error, "out of memory while allocating left-deep rankwidth decomposition");
    return false;
  }

  const size_t words = qsop_bitset_words(qsop->nvars);
  decomposition->nvars = qsop->nvars;
  decomposition->words = words;
  decomposition->nnodes = 2U * qsop->nvars - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * decomposition->words,
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_set_error(error, "out of memory while allocating left-deep rankwidth decomposition nodes");
    return false;
  }

  for (uint32_t i = 0; i < qsop->nvars; i++) {
    decomposition->nodes[i] = (rw_node_t){
        .kind = RW_NODE_LEAF,
        .var = i,
    };
  }
  if (qsop->nvars == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t current = 0;
    uint32_t next_join = qsop->nvars;
    for (uint32_t i = 1; i < qsop->nvars; i++) {
      const uint32_t node = next_join++;
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = current,
          .right = i,
      };
      current = node;
    }
    decomposition->root = current;
  }

  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  *out = decomposition;
  return true;
}
static bool make_fill_in_and_parents(const qsop_instance_t *qsop, const uint32_t *order,
                                     uint64_t *fill, uint32_t *parent_pos, qsop_error_t *error) {
  const uint32_t n = qsop->nvars;
  const size_t words = qsop_bitset_words(n);
  uint64_t *active = calloc(words == 0 ? 1U : words, sizeof(*active));
  uint64_t *scratch = calloc(words == 0 ? 1U : words, sizeof(*scratch));
  if (active == NULL || scratch == NULL) {
    free(active);
    free(scratch);
    qsop_set_error(error, "out of memory building fill-in graph");
    return false;
  }

  for (uint32_t v = 0; v < n; v++) {
    qsop_bitset_set(active, v);
  }

  /* Replay elimination: at each step, clique-ify active neighbors. */
  for (uint32_t pos = 0; pos < n; pos++) {
    const uint32_t v = order[pos];
    uint64_t *v_row = qsop_bitset_row(fill, words, v);

    /* Collect active neighbors into scratch. */
    for (size_t w = 0; w < words; w++) {
      scratch[w] = v_row[w] & active[w];
    }

    /* Add fill edges: connect all pairs of active neighbors. */
    for (uint32_t u = 0; u < n; u++) {
      if (!qsop_bitset_get(scratch, u)) {
        continue;
      }
      uint64_t *u_row = qsop_bitset_row(fill, words, u);
      for (size_t w = 0; w < words; w++) {
        u_row[w] |= scratch[w];
      }
      qsop_bitset_clear(u_row, u); /* no self-loop */
    }

    /* Remove v from active. */
    qsop_bitset_clear(active, v);
  }

  /* Build pos_of inverse map. */
  uint32_t *pos_of = calloc(n == 0 ? 1U : n, sizeof(*pos_of));
  if (pos_of == NULL) {
    free(active);
    free(scratch);
    qsop_set_error(error, "out of memory building pos_of map");
    return false;
  }
  for (uint32_t pos = 0; pos < n; pos++) {
    pos_of[order[pos]] = pos;
  }

  /* For each pos, parent = fill-in neighbor with smallest pos > pos. */
  for (uint32_t pos = 0; pos < n; pos++) {
    const uint32_t v = order[pos];
    const uint64_t *v_row = qsop_bitset_const_row(fill, words, v);
    uint32_t best = UINT32_MAX;
    for (uint32_t u = 0; u < n; u++) {
      if (!qsop_bitset_get(v_row, u)) {
        continue;
      }
      const uint32_t q = pos_of[u];
      if (q > pos && (best == UINT32_MAX || q < best)) {
        best = q;
      }
    }
    parent_pos[pos] = best;
  }

  free(active);
  free(scratch);
  free(pos_of);
  return true;
}
typedef struct etree_dfs_frame {
  uint32_t pos;
  uint32_t child_idx; /* which child we're processing next */
} etree_dfs_frame_t;
static bool build_etree_subtrees(qsop_rankwidth_decomposition_t *d, uint32_t root_pos,
                                 uint32_t **children_arr, uint32_t *children_cnt,
                                 uint32_t *subtree_root, uint32_t n, uint32_t *next_join,
                                 qsop_error_t *error) {
  /* Iterative post-order DFS to avoid stack overflow on deep trees. */
  uint32_t *stack = calloc(n == 0 ? 1U : n, sizeof(*stack));
  uint32_t *result = calloc(n == 0 ? 1U : n, sizeof(*result));
  if (stack == NULL || result == NULL) {
    free(stack);
    free(result);
    qsop_set_error(error, "out of memory building from-treewidth subtrees");
    return false;
  }
  uint32_t sp = 0;
  stack[sp++] = root_pos;

  /* First pass: topological sort (post-order) via DFS. */
  uint32_t *visit_order = calloc(n == 0 ? 1U : n, sizeof(*visit_order));
  if (visit_order == NULL) {
    free(stack);
    free(result);
    qsop_set_error(error, "out of memory building from-treewidth visit order");
    return false;
  }
  uint32_t visit_len = 0;

  /* Iterative DFS for post-order: push right-to-left so left is processed first. */
  while (sp > 0) {
    const uint32_t pos = stack[--sp];
    visit_order[visit_len++] = pos;
    /* Push children right-to-left so they're processed before this node. */
    for (uint32_t i = children_cnt[pos]; i > 0U; i--) {
      stack[sp++] = children_arr[pos][i - 1U];
    }
  }

  /* Second pass: build rank decomposition nodes in reverse visit order (post-order). */
  /* But visit_order above is pre-order; reverse it for post-order. */
  for (uint32_t i = visit_len; i > 0U; i--) {
    const uint32_t pos = visit_order[i - 1U];
    const uint32_t own_leaf = pos; /* node index = pos for leaves */
    const uint32_t k = children_cnt[pos];
    if (k == 0U) {
      result[pos] = own_leaf;
    } else {
      /* Collect child subtree roots. */
      uint32_t current = result[children_arr[pos][0]];
      for (uint32_t j = 1U; j < k; j++) {
        const uint32_t node = (*next_join)++;
        d->nodes[node] = (rw_node_t){
            .kind = RW_NODE_JOIN,
            .left = current,
            .right = result[children_arr[pos][j]],
        };
        current = node;
      }
      /* Join with own leaf. */
      const uint32_t node = (*next_join)++;
      d->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = current,
          .right = own_leaf,
      };
      result[pos] = node;
    }
  }

  *subtree_root = result[root_pos];
  free(stack);
  free(result);
  free(visit_order);
  return true;
}
static bool make_from_treewidth_decomposition(const qsop_instance_t *qsop,
                                              qsop_rankwidth_decomposition_t **out,
                                              qsop_error_t *error) {
  const uint32_t n = qsop->nvars;
  const size_t words = qsop_bitset_words(n);

  /* Step 1: compute min-fill elimination order. */
  uint32_t *order = calloc(n == 0 ? 1U : n, sizeof(*order));
  if (order == NULL) {
    qsop_set_error(error, "out of memory in from-treewidth generator");
    return false;
  }
  for (uint32_t v = 0; v < n; v++) {
    order[v] = v;
  }
  if (!make_min_fill_order(qsop, order, error)) {
    free(order);
    return false;
  }

  /* Step 2: replay elimination to get fill-in graph + elimination tree parents. */
  uint64_t *fill = calloc((n == 0 ? 1U : n) * (words == 0 ? 1U : words), sizeof(*fill));
  uint32_t *parent_pos = calloc(n == 0 ? 1U : n, sizeof(*parent_pos));
  if (fill == NULL || parent_pos == NULL) {
    free(order);
    free(fill);
    free(parent_pos);
    qsop_set_error(error, "out of memory in from-treewidth fill-in allocation");
    return false;
  }
  /* Initialize fill from original graph. */
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  if (!make_fill_in_and_parents(qsop, order, fill, parent_pos, error)) {
    free(order);
    free(fill);
    free(parent_pos);
    return false;
  }
  free(fill); /* no longer needed */

  /* Step 3: build children lists for elimination tree. */
  uint32_t *children_flat = calloc(n == 0 ? 1U : n, sizeof(*children_flat));
  uint32_t **children_arr = calloc(n == 0 ? 1U : n, sizeof(*children_arr));
  uint32_t *children_cnt = calloc(n == 0 ? 1U : n, sizeof(*children_cnt));
  if (children_flat == NULL || children_arr == NULL || children_cnt == NULL) {
    free(order);
    free(parent_pos);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    qsop_set_error(error, "out of memory building elimination tree children");
    return false;
  }

  uint32_t *root_positions = calloc(n == 0 ? 1U : n, sizeof(*root_positions));
  if (root_positions == NULL) {
    free(order);
    free(parent_pos);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    qsop_set_error(error, "out of memory building elimination tree roots");
    return false;
  }
  uint32_t root_count = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] == UINT32_MAX) {
      root_positions[root_count++] = pos;
    } else {
      children_cnt[parent_pos[pos]]++;
    }
  }

  /* Allocate children_arr[pos] using offsets into children_flat. */
  uint32_t offset = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    children_arr[pos] = children_flat + offset;
    offset += children_cnt[pos];
    children_cnt[pos] = 0; /* reset for fill pass */
  }
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] != UINT32_MAX) {
      const uint32_t par = parent_pos[pos];
      children_arr[par][children_cnt[par]++] = pos;
    }
  }
  free(parent_pos);

  /* Step 4: allocate decomposition (2n-1 nodes). */
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    free(order);
    free(root_positions);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    qsop_set_error(error, "out of memory allocating from-treewidth decomposition");
    return false;
  }
  decomposition->nvars = n;
  decomposition->words = words;
  decomposition->nnodes = n <= 1U ? 1U : 2U * n - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * (words == 0 ? 1U : words),
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    free(order);
    free(root_positions);
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_set_error(error, "out of memory allocating from-treewidth decomposition nodes");
    return false;
  }

  /* Leaf nodes: node[pos].var = order[pos] (same layout as other generators). */
  for (uint32_t pos = 0; pos < n; pos++) {
    decomposition->nodes[pos] = (rw_node_t){.kind = RW_NODE_LEAF, .var = order[pos]};
  }
  free(order);

  if (n == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t next_join = n;
    uint32_t combined_root = UINT32_MAX;
    for (uint32_t i = 0; i < root_count; i++) {
      uint32_t subtree_root = 0;
      if (!build_etree_subtrees(decomposition, root_positions[i], children_arr, children_cnt,
                                &subtree_root, n, &next_join, error)) {
        free(root_positions);
        free(children_flat);
        free(children_arr);
        free(children_cnt);
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      if (combined_root == UINT32_MAX) {
        combined_root = subtree_root;
      } else {
        const uint32_t node = next_join++;
        decomposition->nodes[node] = (rw_node_t){
            .kind = RW_NODE_JOIN,
            .left = combined_root,
            .right = subtree_root,
        };
        combined_root = node;
      }
    }
    decomposition->root = combined_root;
  }

  free(root_positions);
  free(children_flat);
  free(children_arr);
  free(children_cnt);

  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  *out = decomposition;
  return true;
}
bool qsop_rankwidth_decomposition_from_order(const qsop_instance_t *qsop, const uint32_t *order,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    qsop_set_error(error, "internal error: null argument to rankwidth decomposition from-order");
    return false;
  }
  *out = NULL;
  if (qsop->nvars == 0) {
    qsop_rankwidth_decomposition_t *empty = calloc(1, sizeof(*empty));
    if (empty == NULL) {
      qsop_set_error(error, "out of memory while allocating empty rankwidth decomposition");
      return false;
    }
    *out = empty;
    return true;
  }
  if (order == NULL) {
    qsop_set_error(error, "internal error: null order for rankwidth decomposition from-order");
    return false;
  }
  /* Copy the provided order into a writable buffer (make_fill_in_and_parents needs a uint32_t[]).
   */
  const uint32_t n = qsop->nvars;
  uint32_t *order_copy = malloc(n * sizeof(*order_copy));
  if (order_copy == NULL) {
    qsop_set_error(error, "out of memory while copying order for rankwidth from-order");
    return false;
  }
  memcpy(order_copy, order, n * sizeof(*order_copy));

  /* Delegate to the internal from-treewidth builder, which owns order_copy. */
  const size_t words = qsop_bitset_words(n);
  uint64_t *fill = calloc((size_t)n * (words == 0 ? 1U : words), sizeof(*fill));
  uint32_t *parent_pos = calloc(n, sizeof(*parent_pos));
  if (fill == NULL || parent_pos == NULL) {
    free(order_copy);
    free(fill);
    free(parent_pos);
    qsop_set_error(error, "out of memory in from-order fill-in allocation");
    return false;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_u[e]), qsop->edge_v[e]);
    qsop_bitset_set(qsop_bitset_row(fill, words, qsop->edge_v[e]), qsop->edge_u[e]);
  }
  if (!make_fill_in_and_parents(qsop, order_copy, fill, parent_pos, error)) {
    free(order_copy);
    free(fill);
    free(parent_pos);
    return false;
  }
  free(fill);
  free(order_copy);

  /* Build children lists, then the decomposition tree (same as make_from_treewidth_decomposition).
   */
  uint32_t *children_flat = calloc(n, sizeof(*children_flat));
  uint32_t **children_arr = calloc(n, sizeof(*children_arr));
  uint32_t *children_cnt = calloc(n, sizeof(*children_cnt));
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (children_flat == NULL || children_arr == NULL || children_cnt == NULL ||
      decomposition == NULL) {
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    free(parent_pos);
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_set_error(error, "out of memory in from-order children allocation");
    return false;
  }
  decomposition->nvars = n;
  decomposition->words = words;
  decomposition->nnodes = 2U * n - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars =
      calloc((size_t)decomposition->nnodes * words, sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    free(parent_pos);
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_set_error(error, "out of memory in from-order decomposition nodes");
    return false;
  }

  /* Build children lists from parent_pos (same logic as make_from_treewidth_decomposition). */
  uint32_t *root_positions = calloc(n, sizeof(*root_positions));
  if (root_positions == NULL) {
    free(children_flat);
    free(children_arr);
    free(children_cnt);
    free(parent_pos);
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_set_error(error, "out of memory in from-order root allocation");
    return false;
  }
  uint32_t root_count = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] == UINT32_MAX) {
      root_positions[root_count++] = pos;
    } else {
      children_cnt[parent_pos[pos]]++;
    }
  }
  uint32_t offset = 0;
  for (uint32_t pos = 0; pos < n; pos++) {
    children_arr[pos] = children_flat + offset;
    offset += children_cnt[pos];
    children_cnt[pos] = 0;
  }
  for (uint32_t pos = 0; pos < n; pos++) {
    if (parent_pos[pos] != UINT32_MAX) {
      const uint32_t par = parent_pos[pos];
      children_arr[par][children_cnt[par]++] = pos;
    }
  }
  free(parent_pos);

  for (uint32_t i = 0; i < n; i++) {
    decomposition->nodes[i] = (rw_node_t){.kind = RW_NODE_LEAF, .var = order[i]};
  }
  if (n == 1U) {
    decomposition->root = 0;
  } else {
    uint32_t next_join = n;
    uint32_t combined_root = UINT32_MAX;
    for (uint32_t i = 0; i < root_count; i++) {
      uint32_t subtree_root = 0;
      if (!build_etree_subtrees(decomposition, root_positions[i], children_arr, children_cnt,
                                &subtree_root, n, &next_join, error)) {
        free(root_positions);
        free(children_flat);
        free(children_arr);
        free(children_cnt);
        qsop_rankwidth_decomposition_free(decomposition);
        return false;
      }
      if (combined_root == UINT32_MAX) {
        combined_root = subtree_root;
      } else {
        const uint32_t node = next_join++;
        decomposition->nodes[node] = (rw_node_t){
            .kind = RW_NODE_JOIN,
            .left = combined_root,
            .right = subtree_root,
        };
        combined_root = node;
      }
    }
    decomposition->root = combined_root;
  }
  free(root_positions);
  free(children_flat);
  free(children_arr);
  free(children_cnt);

  if (!validate_decomposition(decomposition, error)) {
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }
  *out = decomposition;
  return true;
}
bool qsop_rankwidth_decomposition_generate(const qsop_instance_t *qsop,
                                           qsop_rankwidth_generator_t generator,
                                           qsop_rankwidth_decomposition_t **out,
                                           qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    qsop_set_error(error, "internal error: null rankwidth decomposition generation argument");
    return false;
  }
  *out = NULL;
  if (qsop->nvars == 0) {
    qsop_rankwidth_decomposition_t *empty = calloc(1, sizeof(*empty));
    if (empty == NULL) {
      qsop_set_error(error, "out of memory while allocating empty rankwidth decomposition");
      return false;
    }
    *out = empty;
    return true;
  }

  if (generator == QSOP_RANKWIDTH_GENERATOR_FROM_TREEWIDTH) {
    return make_from_treewidth_decomposition(qsop, out, error);
  }

  /* BEST: generate all base generators (including search), return lowest forecast. */
  if (generator == QSOP_RANKWIDTH_GENERATOR_BEST) {
    static const qsop_rankwidth_generator_t kCandidates[] = {
        QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP,       QSOP_RANKWIDTH_GENERATOR_BALANCED,
        QSOP_RANKWIDTH_GENERATOR_MIN_FILL,        QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT,
        QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH,
    };
    const size_t ncandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);
    qsop_rankwidth_decomposition_t *winner = NULL;
    uint64_t winner_max = UINT64_MAX, winner_pairs = UINT64_MAX;
    for (size_t k = 0; k < ncandidates; k++) {
      qsop_rankwidth_decomposition_t *cand = NULL;
      if (!qsop_rankwidth_decomposition_generate(qsop, kCandidates[k], &cand, error)) {
        qsop_rankwidth_decomposition_free(winner);
        return false;
      }
      uint64_t cand_max = 0, cand_pairs = 0;
      if (!qsop_rankwidth_decomposition_forecast(qsop, cand, &cand_max, &cand_pairs, error)) {
        qsop_rankwidth_decomposition_free(cand);
        qsop_rankwidth_decomposition_free(winner);
        return false;
      }
      if (winner == NULL || cand_max < winner_max ||
          (cand_max == winner_max && cand_pairs < winner_pairs)) {
        qsop_rankwidth_decomposition_free(winner);
        winner = cand;
        winner_max = cand_max;
        winner_pairs = cand_pairs;
      } else {
        qsop_rankwidth_decomposition_free(cand);
      }
    }
    *out = winner;
    return true;
  }

  /* MIN_FILL_SEARCH: min-fill ordering refined by adjacent-swap hill climbing. */
  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_SEARCH) {
    uint32_t *mfs_order = calloc(qsop->nvars, sizeof(*mfs_order));
    if (mfs_order == NULL) {
      qsop_set_error(error, "out of memory while allocating min-fill-search order");
      return false;
    }
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      mfs_order[v] = v;
    }
    if (!make_min_fill_order(qsop, mfs_order, error)) {
      free(mfs_order);
      return false;
    }
    const bool ok = generate_left_deep_search(qsop, mfs_order, out, error);
    free(mfs_order);
    return ok;
  }

  uint32_t *order = calloc(qsop->nvars, sizeof(*order));
  uint32_t *leaf_nodes = calloc(qsop->nvars, sizeof(*leaf_nodes));
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (order == NULL || leaf_nodes == NULL || decomposition == NULL) {
    free(order);
    free(leaf_nodes);
    free(decomposition);
    qsop_set_error(error, "out of memory while allocating generated rankwidth decomposition");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    order[v] = v;
  }
  if ((generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL ||
       generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT) &&
      !make_min_fill_order(qsop, order, error)) {
    free(order);
    free(leaf_nodes);
    free(decomposition);
    return false;
  }

  const size_t words = qsop_bitset_words(qsop->nvars);
  uint64_t *adj = NULL;
  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT && qsop->nvars > 1U) {
    adj = rw_adjacency_bitsets(qsop, words, error);
    if (adj == NULL) {
      free(order);
      free(leaf_nodes);
      free(decomposition);
      return false;
    }
  }

  decomposition->nvars = qsop->nvars;
  decomposition->words = words;
  decomposition->nnodes = 2U * qsop->nvars - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->node_vars = calloc((size_t)decomposition->nnodes * decomposition->words,
                                    sizeof(*decomposition->node_vars));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->node_vars == NULL ||
      decomposition->postorder == NULL) {
    free(adj);
    free(order);
    free(leaf_nodes);
    qsop_rankwidth_decomposition_free(decomposition);
    qsop_set_error(error, "out of memory while allocating generated rankwidth decomposition nodes");
    return false;
  }

  for (uint32_t i = 0; i < qsop->nvars; i++) {
    decomposition->nodes[i] = (rw_node_t){
        .kind = RW_NODE_LEAF,
        .var = order[i],
    };
    leaf_nodes[i] = i;
  }

  if (qsop->nvars == 1U) {
    decomposition->root = 0;
  } else if (generator == QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP) {
    uint32_t current = leaf_nodes[0];
    uint32_t next_join = qsop->nvars;
    for (uint32_t i = 1; i < qsop->nvars; i++) {
      const uint32_t node = next_join++;
      decomposition->nodes[node] = (rw_node_t){
          .kind = RW_NODE_JOIN,
          .left = current,
          .right = leaf_nodes[i],
      };
      current = node;
    }
    decomposition->root = current;
  } else if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT) {
    uint64_t *prefix_masks = calloc(((size_t)qsop->nvars + 1U) * words, sizeof(*prefix_masks));
    uint64_t *all = calloc(words == 0 ? 1U : words, sizeof(*all));
    if (prefix_masks == NULL || all == NULL) {
      free(prefix_masks);
      free(all);
      free(adj);
      free(order);
      free(leaf_nodes);
      qsop_rankwidth_decomposition_free(decomposition);
      qsop_set_error(error, "out of memory while building rankwidth cut-rank prefix masks");
      return false;
    }
    for (uint32_t i = 0; i < qsop->nvars; i++) {
      uint64_t *next = qsop_bitset_row(prefix_masks, words, i + 1U);
      const uint64_t *prev = qsop_bitset_const_row(prefix_masks, words, i);
      qsop_bitset_copy(next, prev, words);
      qsop_bitset_set(next, order[i]);
      qsop_bitset_set(all, order[i]);
    }

    uint32_t next_join = qsop->nvars;
    decomposition->root = build_cut_rank_nodes(decomposition, leaf_nodes, prefix_masks, all, adj, 0,
                                               qsop->nvars, &next_join, error);
    free(prefix_masks);
    free(all);
    if (decomposition->root == UINT32_MAX) {
      free(adj);
      free(order);
      free(leaf_nodes);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
  } else {
    uint32_t next_join = qsop->nvars;
    decomposition->root =
        build_balanced_nodes(decomposition, leaf_nodes, 0, qsop->nvars, &next_join);
  }

  free(order);
  free(leaf_nodes);
  if (!validate_decomposition(decomposition, error)) {
    free(adj);
    qsop_rankwidth_decomposition_free(decomposition);
    return false;
  }

  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL_CUT && adj != NULL) {
    rw_decomposition_score_t selected_score = {0};
    if (!rw_decomposition_score(qsop, decomposition, adj, &selected_score, error)) {
      free(adj);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }

    qsop_rankwidth_decomposition_t *min_fill = NULL;
    if (!qsop_rankwidth_decomposition_generate(qsop, QSOP_RANKWIDTH_GENERATOR_MIN_FILL, &min_fill,
                                               error)) {
      free(adj);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    rw_decomposition_score_t min_fill_score = {0};
    if (!rw_decomposition_score(qsop, min_fill, adj, &min_fill_score, error)) {
      free(adj);
      qsop_rankwidth_decomposition_free(min_fill);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (compare_decomposition_scores(min_fill_score, selected_score) < 0) {
      qsop_rankwidth_decomposition_free(decomposition);
      decomposition = min_fill;
      min_fill = NULL;
      selected_score = min_fill_score;
    }
    qsop_rankwidth_decomposition_free(min_fill);

    qsop_rankwidth_decomposition_t *left_deep = NULL;
    if (!make_left_deep_generated_decomposition(qsop, &left_deep, error)) {
      free(adj);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    rw_decomposition_score_t left_deep_score = {0};
    if (!rw_decomposition_score(qsop, left_deep, adj, &left_deep_score, error)) {
      free(adj);
      qsop_rankwidth_decomposition_free(left_deep);
      qsop_rankwidth_decomposition_free(decomposition);
      return false;
    }
    if (compare_decomposition_scores(left_deep_score, selected_score) <= 0) {
      qsop_rankwidth_decomposition_free(decomposition);
      decomposition = left_deep;
      left_deep = NULL;
    }
    qsop_rankwidth_decomposition_free(left_deep);
  }

  free(adj);
  *out = decomposition;
  return true;
}
void qsop_rankwidth_decomposition_free(qsop_rankwidth_decomposition_t *decomposition) {
  if (decomposition == NULL) {
    return;
  }
  free(decomposition->nodes);
  free(decomposition->node_vars);
  free(decomposition->postorder);
  free(decomposition);
}
