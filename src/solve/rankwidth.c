#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "trace.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum rw_node_kind {
  RW_NODE_UNDEFINED,
  RW_NODE_LEAF,
  RW_NODE_JOIN,
} rw_node_kind_t;

typedef struct rw_node {
  rw_node_kind_t kind;
  uint32_t var;
  uint32_t left;
  uint32_t right;
  uint64_t vars;
} rw_node_t;

struct qsop_rankwidth_decomposition {
  uint32_t nvars;
  uint32_t nnodes;
  uint32_t root;
  rw_node_t *nodes;
  uint32_t *postorder;
  uint32_t postorder_len;
};

typedef struct rw_entry {
  uint64_t signature;
  uint32_t residue;
  uint64_t count;
} rw_entry_t;

typedef struct rw_signature_rep {
  uint64_t signature;
  uint64_t assignment;
} rw_signature_rep_t;

typedef struct rw_table {
  rw_entry_t *entries;
  size_t len;
  size_t cap;
  rw_signature_rep_t *reps;
  size_t reps_len;
  size_t reps_cap;
} rw_table_t;

static void set_error(qsop_error_t *error, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }

  error->path = NULL;
  error->line = 0;
  error->column = 0;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
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

static uint64_t bit_for_var(uint32_t v) {
  return UINT64_C(1) << v;
}

static uint64_t all_vars_mask(uint32_t nvars) {
  return (UINT64_C(1) << nvars) - UINT64_C(1);
}

static bool reserve_entries(rw_table_t *table, size_t needed, qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  rw_entry_t *entries = realloc(table->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    set_error(error, "out of memory while growing rankwidth table");
    return false;
  }
  table->entries = entries;
  table->cap = new_cap;
  return true;
}

static bool reserve_reps(rw_table_t *table, size_t needed, qsop_error_t *error) {
  if (needed <= table->reps_cap) {
    return true;
  }

  size_t new_cap = table->reps_cap == 0 ? 8U : table->reps_cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth signature table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  rw_signature_rep_t *reps = realloc(table->reps, new_cap * sizeof(*reps));
  if (reps == NULL) {
    set_error(error, "out of memory while growing rankwidth signature table");
    return false;
  }
  table->reps = reps;
  table->reps_cap = new_cap;
  return true;
}

static bool table_add_rep(rw_table_t *table, uint64_t signature, uint64_t assignment,
                          qsop_error_t *error) {
  for (size_t i = 0; i < table->reps_len; i++) {
    if (table->reps[i].signature == signature) {
      return true;
    }
  }
  if (!reserve_reps(table, table->reps_len + 1U, error)) {
    return false;
  }
  table->reps[table->reps_len++] = (rw_signature_rep_t){
      .signature = signature,
      .assignment = assignment,
  };
  return true;
}

static bool table_get_rep(const rw_table_t *table, uint64_t signature, uint64_t *out) {
  for (size_t i = 0; i < table->reps_len; i++) {
    if (table->reps[i].signature == signature) {
      *out = table->reps[i].assignment;
      return true;
    }
  }
  return false;
}

static bool table_add_entry(rw_table_t *table, uint64_t signature, uint32_t residue,
                            uint64_t count, qsop_error_t *error) {
  if (count == 0) {
    return true;
  }
  for (size_t i = 0; i < table->len; i++) {
    if (table->entries[i].signature == signature && table->entries[i].residue == residue) {
      table->entries[i].count += count;
      return true;
    }
  }
  if (!reserve_entries(table, table->len + 1U, error)) {
    return false;
  }
  table->entries[table->len++] = (rw_entry_t){
      .signature = signature,
      .residue = residue,
      .count = count,
  };
  return true;
}

static void table_free(rw_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->entries);
  free(table->reps);
  *table = (rw_table_t){0};
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

static bool validate_decomposition_dfs(qsop_rankwidth_decomposition_t *decomposition,
                                       uint32_t node, uint8_t *state, uint8_t *seen_var,
                                       qsop_error_t *error) {
  if (node >= decomposition->nnodes) {
    set_error(error, "rankwidth decomposition references node outside range");
    return false;
  }
  if (state[node] == 1U) {
    set_error(error, "rankwidth decomposition contains a cycle");
    return false;
  }
  if (state[node] == 2U) {
    return true;
  }

  rw_node_t *entry = &decomposition->nodes[node];
  if (entry->kind == RW_NODE_UNDEFINED) {
    set_error(error, "rankwidth decomposition references undefined node %" PRIu32, node);
    return false;
  }
  state[node] = 1U;
  if (entry->kind == RW_NODE_LEAF) {
    if (entry->var >= decomposition->nvars) {
      set_error(error, "rankwidth decomposition leaf variable is outside range");
      return false;
    }
    if (seen_var[entry->var] != 0) {
      set_error(error, "rankwidth decomposition maps variable %" PRIu32 " more than once",
                entry->var);
      return false;
    }
    seen_var[entry->var] = 1U;
    entry->vars = bit_for_var(entry->var);
  } else {
    if (!validate_decomposition_dfs(decomposition, entry->left, state, seen_var, error) ||
        !validate_decomposition_dfs(decomposition, entry->right, state, seen_var, error)) {
      return false;
    }
    const uint64_t left = decomposition->nodes[entry->left].vars;
    const uint64_t right = decomposition->nodes[entry->right].vars;
    if ((left & right) != 0) {
      set_error(error, "rankwidth decomposition children are not disjoint");
      return false;
    }
    entry->vars = left | right;
  }
  state[node] = 2U;
  decomposition->postorder[decomposition->postorder_len++] = node;
  return true;
}

static bool validate_decomposition(qsop_rankwidth_decomposition_t *decomposition,
                                   qsop_error_t *error) {
  uint8_t *state = calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*state));
  uint8_t *seen_var =
      calloc(decomposition->nvars == 0 ? 1U : decomposition->nvars, sizeof(*seen_var));
  if (state == NULL || seen_var == NULL) {
    free(state);
    free(seen_var);
    set_error(error, "out of memory while validating rankwidth decomposition");
    return false;
  }

  const bool ok =
      validate_decomposition_dfs(decomposition, decomposition->root, state, seen_var, error);
  if (ok && decomposition->nodes[decomposition->root].vars != all_vars_mask(decomposition->nvars)) {
    set_error(error, "rankwidth decomposition root does not cover every variable");
    free(state);
    free(seen_var);
    return false;
  }
  for (uint32_t v = 0; ok && v < decomposition->nvars; v++) {
    if (seen_var[v] == 0) {
      set_error(error, "rankwidth decomposition does not include variable %" PRIu32, v);
      free(state);
      free(seen_var);
      return false;
    }
  }

  free(state);
  free(seen_var);
  return ok;
}

bool qsop_rankwidth_decomposition_parse_file(FILE *file, const char *path, uint32_t nvars,
                                             qsop_rankwidth_decomposition_t **out,
                                             qsop_error_t *error) {
  if (file == NULL || out == NULL) {
    set_error(error, "internal error: null rankwidth decomposition parse argument");
    return false;
  }
  *out = NULL;
  if (nvars > 63U) {
    set_error(error, "rankwidth backend currently supports at most 63 variables");
    return false;
  }

  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (decomposition == NULL) {
    set_error(error, "out of memory while allocating rankwidth decomposition");
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
      decomposition->nodes = calloc(nnodes, sizeof(*decomposition->nodes));
      decomposition->postorder = calloc(nnodes, sizeof(*decomposition->postorder));
      if (decomposition->nodes == NULL || decomposition->postorder == NULL) {
        set_error(error, "out of memory while allocating rankwidth decomposition nodes");
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
    set_error(error, "rankwidth decomposition missing header");
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

void qsop_rankwidth_decomposition_free(qsop_rankwidth_decomposition_t *decomposition) {
  if (decomposition == NULL) {
    return;
  }
  free(decomposition->nodes);
  free(decomposition->postorder);
  free(decomposition);
}

static bool qsop_is_sign_edge_instance(const qsop_instance_t *qsop) {
  const uint32_t sign = qsop->r / 2U;
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (qsop->edge_q[e] != sign) {
      return false;
    }
  }
  return true;
}

static uint64_t *adjacency_masks(const qsop_instance_t *qsop, qsop_error_t *error) {
  uint64_t *adj = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*adj));
  if (adj == NULL) {
    set_error(error, "out of memory while allocating rankwidth adjacency masks");
    return NULL;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint64_t u = bit_for_var(qsop->edge_u[e]);
    const uint64_t v = bit_for_var(qsop->edge_v[e]);
    adj[qsop->edge_u[e]] |= v;
    adj[qsop->edge_v[e]] |= u;
  }
  return adj;
}

static uint32_t gf2_rank_masks(uint64_t *rows, uint32_t nrows, uint32_t nvars) {
  uint32_t rank = 0;
  for (uint32_t col = 0; col < nvars && rank < nrows; col++) {
    const uint64_t bit = bit_for_var(col);
    uint32_t pivot = rank;
    while (pivot < nrows && (rows[pivot] & bit) == 0) {
      pivot++;
    }
    if (pivot == nrows) {
      continue;
    }
    if (pivot != rank) {
      const uint64_t tmp = rows[rank];
      rows[rank] = rows[pivot];
      rows[pivot] = tmp;
    }
    for (uint32_t row = 0; row < nrows; row++) {
      if (row != rank && (rows[row] & bit) != 0) {
        rows[row] ^= rows[rank];
      }
    }
    rank++;
  }
  return rank;
}

static uint32_t cut_rank(uint32_t nvars, const uint64_t *adj, uint64_t left, uint64_t right,
                         qsop_error_t *error) {
  uint64_t *rows = calloc(nvars == 0 ? 1U : nvars, sizeof(*rows));
  if (rows == NULL) {
    set_error(error, "out of memory while computing rankwidth cut rank");
    return UINT32_MAX;
  }
  uint32_t nrows = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if ((left & bit_for_var(v)) != 0) {
      rows[nrows++] = adj[v] & right;
    }
  }
  const uint32_t rank = gf2_rank_masks(rows, nrows, nvars);
  free(rows);
  return rank;
}

static uint32_t decomposition_width(const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_error_t *error) {
  const uint64_t all = all_vars_mask(decomposition->nvars);
  uint32_t width = 0;
  for (uint32_t i = 0; i < decomposition->nnodes; i++) {
    if (i == decomposition->root) {
      continue;
    }
    const uint64_t left = decomposition->nodes[i].vars;
    const uint32_t rank = cut_rank(decomposition->nvars, adj, left, all & ~left, error);
    if (rank == UINT32_MAX) {
      return UINT32_MAX;
    }
    if (rank > width) {
      width = rank;
    }
  }
  return width;
}

static uint32_t cross_parity(const qsop_instance_t *qsop, uint64_t left_assignment,
                             uint64_t right_assignment) {
  uint32_t parity = 0;
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const bool u_left = (left_assignment & bit_for_var(qsop->edge_u[e])) != 0;
    const bool v_left = (left_assignment & bit_for_var(qsop->edge_v[e])) != 0;
    const bool u_right = (right_assignment & bit_for_var(qsop->edge_u[e])) != 0;
    const bool v_right = (right_assignment & bit_for_var(qsop->edge_v[e])) != 0;
    if ((u_left && v_right) || (v_left && u_right)) {
      parity ^= 1U;
    }
  }
  return parity;
}

static bool solve_leaf(const qsop_instance_t *qsop, const uint64_t *adj, const rw_node_t *node,
                       rw_table_t *table, qsop_error_t *error) {
  const uint64_t var_bit = bit_for_var(node->var);
  const uint64_t signature = adj[node->var] & ~var_bit;
  return table_add_rep(table, 0, 0, error) &&
         table_add_entry(table, 0, 0, 1, error) &&
         table_add_rep(table, signature, var_bit, error) &&
         table_add_entry(table, signature, qsop->unary[node->var] % qsop->r, 1, error);
}

static bool solve_join(const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomp,
                       const rw_node_t *node, const rw_table_t *left, const rw_table_t *right,
                       rw_table_t *out, uint64_t *join_pairs, qsop_error_t *error) {
  const uint32_t sign = qsop->r / 2U;
  const uint64_t outside = all_vars_mask(qsop->nvars) & ~node->vars;
  for (size_t i = 0; i < left->len; i++) {
    uint64_t left_rep = 0;
    if (!table_get_rep(left, left->entries[i].signature, &left_rep)) {
      set_error(error, "internal error: missing left rankwidth signature representative");
      return false;
    }
    for (size_t j = 0; j < right->len; j++) {
      uint64_t right_rep = 0;
      if (!table_get_rep(right, right->entries[j].signature, &right_rep)) {
        set_error(error, "internal error: missing right rankwidth signature representative");
        return false;
      }
      const uint64_t signature = (left->entries[i].signature ^ right->entries[j].signature) & outside;
      const uint32_t parity = cross_parity(qsop, left_rep, right_rep);
      const uint32_t residue =
          (uint32_t)(((uint64_t)left->entries[i].residue + right->entries[j].residue +
                      (uint64_t)sign * parity) %
                     qsop->r);
      if (!table_add_rep(out, signature, left_rep | right_rep, error) ||
          !table_add_entry(out, signature, residue,
                           left->entries[i].count * right->entries[j].count, error)) {
        return false;
      }
      (*join_pairs)++;
    }
  }
  (void)decomp;
  return true;
}

bool qsop_solve_rankwidth_trace_stats(const qsop_instance_t *qsop,
                                      const qsop_rankwidth_decomposition_t *decomposition,
                                      uint32_t max_vars, qsop_result_t **out,
                                      qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                      qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL || decomposition == NULL) {
    set_error(error, "internal error: null rankwidth solve argument");
    return false;
  }
  if (qsop->nvars != decomposition->nvars) {
    set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (qsop->nvars > max_vars || qsop->nvars > 63U) {
    set_error(error,
              "rankwidth solver refuses %" PRIu32
              " variables; pass a larger --max-vars or use a future bitset backend",
              qsop->nvars);
    return false;
  }
  if (!qsop_is_sign_edge_instance(qsop)) {
    set_error(error, "rankwidth solver currently requires sign-only quadratic coefficients");
    return false;
  }

  uint64_t *adj = adjacency_masks(qsop, error);
  if (adj == NULL) {
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(adj);
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  uint64_t join_pairs = 0;
  uint64_t table_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf(qsop, adj, node, &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.leaf", 0, tables[node_id].len, start);
    } else {
      ok = solve_join(qsop, decomposition, node, &tables[node->left], &tables[node->right],
                      &tables[node_id], &join_pairs, error);
      qsop_trace_emit_elapsed(trace, "rankwidth.join", 0, tables[node_id].len, start);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      free(adj);
      qsop_result_free(result);
      return false;
    }
    table_entries += tables[node_id].len;
  }

  const rw_table_t *root = &tables[decomposition->root];
  for (size_t i = 0; i < root->len; i++) {
    if (root->entries[i].signature != 0) {
      continue;
    }
    const uint32_t residue = (root->entries[i].residue + qsop->constant) % qsop->r;
    result->counts[residue] += root->entries[i].count;
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->join_pairs = join_pairs;
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      free(adj);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  free(adj);
  *out = result;
  return true;
}
