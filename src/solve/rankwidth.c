#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "trace.h"

#include <inttypes.h>
#include <limits.h>
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

typedef struct rw_join_map_entry {
  uint64_t left_signature;
  uint64_t right_signature;
  uint64_t parent_signature;
  uint64_t assignment;
  uint32_t residue_shift;
} rw_join_map_entry_t;

typedef struct rw_join_map {
  rw_join_map_entry_t *entries;
  size_t len;
  size_t cap;
} rw_join_map_t;

typedef struct rw_fourier_table {
  uint64_t *signatures;
  uint64_t *assignments;
  uint64_t *values;
  size_t len;
  size_t cap;
} rw_fourier_table_t;

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

static uint32_t popcount_u64(uint64_t value) {
  uint32_t count = 0;
  while (value != 0) {
    value &= value - 1U;
    count++;
  }
  return count;
}

static uint32_t first_set_bit_u64(uint64_t value) {
  uint32_t bit = 0;
  while ((value & UINT64_C(1)) == 0) {
    value >>= 1U;
    bit++;
  }
  return bit;
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

static bool reserve_join_map(rw_join_map_t *map, size_t needed, qsop_error_t *error) {
  if (needed <= map->cap) {
    return true;
  }

  size_t new_cap = map->cap == 0 ? 8U : map->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth join map is too large");
      return false;
    }
    new_cap *= 2U;
  }
  rw_join_map_entry_t *entries = realloc(map->entries, new_cap * sizeof(*entries));
  if (entries == NULL) {
    set_error(error, "out of memory while growing rankwidth join map");
    return false;
  }
  map->entries = entries;
  map->cap = new_cap;
  return true;
}

static void join_map_free(rw_join_map_t *map) {
  if (map == NULL) {
    return;
  }
  free(map->entries);
  *map = (rw_join_map_t){0};
}

static bool reserve_fourier_table(rw_fourier_table_t *table, size_t needed, uint32_t r,
                                  qsop_error_t *error) {
  if (needed <= table->cap) {
    return true;
  }

  size_t new_cap = table->cap == 0 ? 8U : table->cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "rankwidth Fourier table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  if (new_cap > SIZE_MAX / (r == 0 ? 1U : (size_t)r) / sizeof(uint64_t)) {
    set_error(error, "rankwidth Fourier table is too large");
    return false;
  }

  uint64_t *signatures = calloc(new_cap, sizeof(*signatures));
  uint64_t *assignments = calloc(new_cap, sizeof(*assignments));
  uint64_t *values = calloc(new_cap * (size_t)r, sizeof(*values));
  if (signatures == NULL || assignments == NULL || values == NULL) {
    free(signatures);
    free(assignments);
    free(values);
    set_error(error, "out of memory while growing rankwidth Fourier table");
    return false;
  }
  if (table->len != 0) {
    memcpy(signatures, table->signatures, table->len * sizeof(*signatures));
    memcpy(assignments, table->assignments, table->len * sizeof(*assignments));
    memcpy(values, table->values, table->len * (size_t)r * sizeof(*values));
  }
  free(table->signatures);
  free(table->assignments);
  free(table->values);
  table->signatures = signatures;
  table->assignments = assignments;
  table->values = values;
  table->cap = new_cap;
  return true;
}

static void fourier_table_free(rw_fourier_table_t *table) {
  if (table == NULL) {
    return;
  }
  free(table->signatures);
  free(table->assignments);
  free(table->values);
  *table = (rw_fourier_table_t){0};
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
  decomposition->postorder_len = 0;
  for (uint32_t i = 0; i < decomposition->nnodes; i++) {
    decomposition->nodes[i].vars = 0;
  }

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

static uint64_t min_fill_edges_for(uint32_t v, const uint64_t *adj, uint64_t active) {
  uint64_t neighbors = adj[v] & active;
  uint64_t fill = 0;
  while (neighbors != 0) {
    const uint32_t u = first_set_bit_u64(neighbors);
    neighbors &= ~bit_for_var(u);
    const uint64_t missing = neighbors & ~adj[u];
    fill += popcount_u64(missing);
  }
  return fill;
}

static bool make_min_fill_order(const qsop_instance_t *qsop, uint32_t *order,
                                qsop_error_t *error) {
  uint64_t *work = calloc(qsop->nvars == 0 ? 1U : qsop->nvars, sizeof(*work));
  if (work == NULL) {
    set_error(error, "out of memory while building rankwidth min-fill order");
    return false;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    work[qsop->edge_u[e]] |= bit_for_var(qsop->edge_v[e]);
    work[qsop->edge_v[e]] |= bit_for_var(qsop->edge_u[e]);
  }

  uint64_t active = all_vars_mask(qsop->nvars);
  for (uint32_t pos = 0; pos < qsop->nvars; pos++) {
    bool found = false;
    uint32_t best = 0;
    uint64_t best_fill = UINT64_MAX;
    uint32_t best_degree = UINT32_MAX;
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      if ((active & bit_for_var(v)) == 0) {
        continue;
      }
      const uint32_t degree = popcount_u64(work[v] & active);
      const uint64_t fill = min_fill_edges_for(v, work, active);
      if (!found || fill < best_fill || (fill == best_fill && degree < best_degree)) {
        found = true;
        best = v;
        best_fill = fill;
        best_degree = degree;
      }
    }
    if (!found) {
      free(work);
      set_error(error, "internal error: rankwidth min-fill order stopped early");
      return false;
    }
    order[pos] = best;

    uint64_t neighbors = work[best] & active;
    for (uint32_t u = 0; u < qsop->nvars; u++) {
      if ((neighbors & bit_for_var(u)) == 0) {
        continue;
      }
      for (uint32_t v = u + 1U; v < qsop->nvars; v++) {
        if ((neighbors & bit_for_var(v)) != 0) {
          work[u] |= bit_for_var(v);
          work[v] |= bit_for_var(u);
        }
      }
    }
    active &= ~bit_for_var(best);
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      work[v] &= active;
    }
  }

  free(work);
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

bool qsop_rankwidth_decomposition_generate(const qsop_instance_t *qsop,
                                           qsop_rankwidth_generator_t generator,
                                           qsop_rankwidth_decomposition_t **out,
                                           qsop_error_t *error) {
  if (qsop == NULL || out == NULL) {
    set_error(error, "internal error: null rankwidth decomposition generation argument");
    return false;
  }
  *out = NULL;
  if (qsop->nvars == 0) {
    set_error(error, "rankwidth decomposition generation requires at least one variable");
    return false;
  }
  if (qsop->nvars > 63U) {
    set_error(error, "rankwidth backend currently supports at most 63 variables");
    return false;
  }

  uint32_t *order = calloc(qsop->nvars, sizeof(*order));
  uint32_t *leaf_nodes = calloc(qsop->nvars, sizeof(*leaf_nodes));
  qsop_rankwidth_decomposition_t *decomposition = calloc(1, sizeof(*decomposition));
  if (order == NULL || leaf_nodes == NULL || decomposition == NULL) {
    free(order);
    free(leaf_nodes);
    free(decomposition);
    set_error(error, "out of memory while allocating generated rankwidth decomposition");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    order[v] = v;
  }
  if (generator == QSOP_RANKWIDTH_GENERATOR_MIN_FILL &&
      !make_min_fill_order(qsop, order, error)) {
    free(order);
    free(leaf_nodes);
    free(decomposition);
    return false;
  }

  decomposition->nvars = qsop->nvars;
  decomposition->nnodes = 2U * qsop->nvars - 1U;
  decomposition->nodes = calloc(decomposition->nnodes, sizeof(*decomposition->nodes));
  decomposition->postorder = calloc(decomposition->nnodes, sizeof(*decomposition->postorder));
  if (decomposition->nodes == NULL || decomposition->postorder == NULL) {
    free(order);
    free(leaf_nodes);
    qsop_rankwidth_decomposition_free(decomposition);
    set_error(error, "out of memory while allocating generated rankwidth decomposition nodes");
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
  } else if (generator == QSOP_RANKWIDTH_GENERATOR_LINEAR) {
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
  } else {
    uint32_t next_join = qsop->nvars;
    decomposition->root =
        build_balanced_nodes(decomposition, leaf_nodes, 0, qsop->nvars, &next_join);
  }

  free(order);
  free(leaf_nodes);
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

static uint32_t cross_parity_masks(uint32_t nvars, const uint64_t *adj, uint64_t left_assignment,
                                   uint64_t right_assignment) {
  uint32_t parity = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if ((left_assignment & bit_for_var(v)) != 0) {
      parity ^= popcount_u64(adj[v] & right_assignment) & 1U;
    }
  }
  return parity;
}

static bool build_join_map(const qsop_instance_t *qsop, const uint64_t *adj, const rw_node_t *node,
                           const rw_table_t *left, const rw_table_t *right, rw_join_map_t *map,
                           qsop_error_t *error) {
  const uint32_t sign = qsop->r / 2U;
  const uint64_t outside = all_vars_mask(qsop->nvars) & ~node->vars;
  if (left->reps_len > 0 && right->reps_len > SIZE_MAX / left->reps_len) {
    set_error(error, "rankwidth join map is too large");
    return false;
  }
  if (!reserve_join_map(map, left->reps_len * right->reps_len, error)) {
    return false;
  }
  for (size_t i = 0; i < left->reps_len; i++) {
    for (size_t j = 0; j < right->reps_len; j++) {
      const uint64_t left_rep = left->reps[i].assignment;
      const uint64_t right_rep = right->reps[j].assignment;
      const uint32_t parity = cross_parity_masks(qsop->nvars, adj, left_rep, right_rep);
      map->entries[map->len++] = (rw_join_map_entry_t){
          .left_signature = left->reps[i].signature,
          .right_signature = right->reps[j].signature,
          .parent_signature = (left->reps[i].signature ^ right->reps[j].signature) & outside,
          .assignment = left_rep | right_rep,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  return true;
}

static const rw_join_map_entry_t *join_map_get(const rw_join_map_t *map, uint64_t left_signature,
                                               uint64_t right_signature) {
  for (size_t i = 0; i < map->len; i++) {
    if (map->entries[i].left_signature == left_signature &&
        map->entries[i].right_signature == right_signature) {
      return &map->entries[i];
    }
  }
  return NULL;
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

static bool solve_join(const qsop_instance_t *qsop, const rw_join_map_t *map,
                       const rw_table_t *left, const rw_table_t *right, rw_table_t *out,
                       uint64_t *join_pairs, qsop_error_t *error) {
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      const rw_join_map_entry_t *mapped =
          join_map_get(map, left->entries[i].signature, right->entries[j].signature);
      if (mapped == NULL) {
        set_error(error, "internal error: missing rankwidth join-map entry");
        return false;
      }
      const uint32_t residue =
          (uint32_t)(((uint64_t)left->entries[i].residue + right->entries[j].residue +
                      mapped->residue_shift) %
                     qsop->r);
      if (!table_add_rep(out, mapped->parent_signature, mapped->assignment, error) ||
          !table_add_entry(out, mapped->parent_signature, residue,
                           left->entries[i].count * right->entries[j].count, error)) {
        return false;
      }
      (*join_pairs)++;
    }
  }
  return true;
}

static uint64_t add_mod_u64(uint64_t a, uint64_t b, uint64_t mod) {
  return a >= mod - b ? a - (mod - b) : a + b;
}

static uint64_t mul_mod_u64(uint64_t a, uint64_t b, uint64_t mod) {
  __extension__ typedef unsigned __int128 uint128_t;
  return (uint64_t)(((uint128_t)a * b) % mod);
}

static uint64_t pow_mod_u64(uint64_t base, uint64_t exp, uint64_t mod) {
  uint64_t result = 1;
  uint64_t value = base % mod;
  while (exp != 0) {
    if ((exp & 1U) != 0) {
      result = mul_mod_u64(result, value, mod);
    }
    exp >>= 1U;
    if (exp != 0) {
      value = mul_mod_u64(value, value, mod);
    }
  }
  return result;
}

static bool miller_rabin_witness(uint64_t n, uint64_t base, uint64_t d, uint32_t s) {
  if (base % n == 0) {
    return false;
  }
  uint64_t x = pow_mod_u64(base, d, n);
  if (x == 1 || x == n - 1U) {
    return false;
  }
  for (uint32_t r = 1; r < s; r++) {
    x = mul_mod_u64(x, x, n);
    if (x == n - 1U) {
      return false;
    }
  }
  return true;
}

static bool is_prime_u64(uint64_t n) {
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

static bool find_ntt_prime(uint32_t r, uint32_t nvars, uint64_t *prime, qsop_error_t *error) {
  const uint64_t count_bound = UINT64_C(1) << nvars;
  uint64_t k = (UINT64_MAX - 1U) / r;
  for (uint64_t attempts = 0; attempts < UINT64_C(2000000) && k > 0; attempts++, k--) {
    const uint64_t candidate = k * (uint64_t)r + 1U;
    if (candidate <= count_bound) {
      break;
    }
    if (is_prime_u64(candidate)) {
      *prime = candidate;
      return true;
    }
  }
  set_error(error, "rankwidth Fourier mode could not find a 64-bit NTT prime for modulus %" PRIu32,
            r);
  return false;
}

static bool find_order_root(uint64_t prime, uint32_t r, uint64_t *root, qsop_error_t *error) {
  uint32_t factors[32] = {0};
  const uint32_t nfactors = factor_u32(r, factors, 32);
  for (uint64_t g = 2; g < UINT64_C(1000000); g++) {
    const uint64_t candidate = pow_mod_u64(g, (prime - 1U) / r, prime);
    if (candidate == 1) {
      continue;
    }
    bool exact = true;
    for (uint32_t i = 0; i < nfactors; i++) {
      if (pow_mod_u64(candidate, r / factors[i], prime) == 1) {
        exact = false;
        break;
      }
    }
    if (exact) {
      *root = candidate;
      return true;
    }
  }
  set_error(error, "rankwidth Fourier mode could not find an order-%" PRIu32 " root", r);
  return false;
}

static bool make_root_powers(uint32_t r, uint64_t root, uint64_t prime, uint64_t **out,
                             qsop_error_t *error) {
  if ((size_t)r > SIZE_MAX / (r == 0 ? 1U : (size_t)r) / sizeof(uint64_t)) {
    set_error(error, "rankwidth Fourier modulus is too large for dense mode tables");
    return false;
  }
  uint64_t *powers = calloc((size_t)r * r, sizeof(*powers));
  if (powers == NULL) {
    set_error(error, "out of memory while allocating rankwidth Fourier powers");
    return false;
  }
  for (uint32_t mode = 0; mode < r; mode++) {
    for (uint32_t residue = 0; residue < r; residue++) {
      powers[(size_t)mode * r + residue] =
          pow_mod_u64(root, ((uint64_t)mode * residue) % r, prime);
    }
  }
  *out = powers;
  return true;
}

static bool fourier_table_signature_index(rw_fourier_table_t *table, uint64_t signature,
                                          uint64_t assignment, uint32_t r, size_t *out,
                                          qsop_error_t *error) {
  for (size_t i = 0; i < table->len; i++) {
    if (table->signatures[i] == signature) {
      *out = i;
      return true;
    }
  }
  if (!reserve_fourier_table(table, table->len + 1U, r, error)) {
    return false;
  }
  const size_t index = table->len++;
  table->signatures[index] = signature;
  table->assignments[index] = assignment;
  memset(&table->values[index * (size_t)r], 0, (size_t)r * sizeof(*table->values));
  *out = index;
  return true;
}

static bool solve_fourier_leaf(const qsop_instance_t *qsop, const uint64_t *adj,
                               const rw_node_t *node, const uint64_t *powers,
                               uint64_t prime, rw_fourier_table_t *table,
                               qsop_error_t *error) {
  const uint64_t var_bit = bit_for_var(node->var);
  const uint64_t signature = adj[node->var] & ~var_bit;
  size_t zero = 0;
  size_t one = 0;
  if (!fourier_table_signature_index(table, 0, 0, qsop->r, &zero, error) ||
      !fourier_table_signature_index(table, signature, var_bit, qsop->r, &one, error)) {
    return false;
  }
  for (uint32_t mode = 0; mode < qsop->r; mode++) {
    table->values[zero * (size_t)qsop->r + mode] =
        add_mod_u64(table->values[zero * (size_t)qsop->r + mode], 1, prime);
    table->values[one * (size_t)qsop->r + mode] = add_mod_u64(
        table->values[one * (size_t)qsop->r + mode],
        powers[(size_t)mode * qsop->r + (qsop->unary[node->var] % qsop->r)], prime);
  }
  return true;
}

static bool build_fourier_join_map(const qsop_instance_t *qsop, const uint64_t *adj,
                                   const rw_node_t *node, const rw_fourier_table_t *left,
                                   const rw_fourier_table_t *right, rw_join_map_t *map,
                                   qsop_error_t *error) {
  const uint32_t sign = qsop->r / 2U;
  const uint64_t outside = all_vars_mask(qsop->nvars) & ~node->vars;
  if (left->len > 0 && right->len > SIZE_MAX / left->len) {
    set_error(error, "rankwidth Fourier join map is too large");
    return false;
  }
  if (!reserve_join_map(map, left->len * right->len, error)) {
    return false;
  }
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      const uint64_t left_rep = left->assignments[i];
      const uint64_t right_rep = right->assignments[j];
      const uint32_t parity = cross_parity_masks(qsop->nvars, adj, left_rep, right_rep);
      map->entries[map->len++] = (rw_join_map_entry_t){
          .left_signature = left->signatures[i],
          .right_signature = right->signatures[j],
          .parent_signature = (left->signatures[i] ^ right->signatures[j]) & outside,
          .assignment = left_rep | right_rep,
          .residue_shift = (uint32_t)(((uint64_t)sign * parity) % qsop->r),
      };
    }
  }
  return true;
}

static bool solve_fourier_join(const qsop_instance_t *qsop, const rw_join_map_t *map,
                               const rw_fourier_table_t *left,
                               const rw_fourier_table_t *right, const uint64_t *powers,
                               uint64_t prime, rw_fourier_table_t *out,
                               uint64_t *join_signature_pairs, qsop_error_t *error) {
  for (size_t i = 0; i < left->len; i++) {
    for (size_t j = 0; j < right->len; j++) {
      const rw_join_map_entry_t *mapped =
          join_map_get(map, left->signatures[i], right->signatures[j]);
      if (mapped == NULL) {
        set_error(error, "internal error: missing rankwidth Fourier join-map entry");
        return false;
      }
      size_t out_index = 0;
      if (!fourier_table_signature_index(out, mapped->parent_signature, mapped->assignment,
                                         qsop->r, &out_index, error)) {
        return false;
      }
      for (uint32_t mode = 0; mode < qsop->r; mode++) {
        const uint64_t left_value = left->values[i * (size_t)qsop->r + mode];
        const uint64_t right_value = right->values[j * (size_t)qsop->r + mode];
        uint64_t value = mul_mod_u64(left_value, right_value, prime);
        value = mul_mod_u64(value, powers[(size_t)mode * qsop->r + mapped->residue_shift], prime);
        out->values[out_index * (size_t)qsop->r + mode] =
            add_mod_u64(out->values[out_index * (size_t)qsop->r + mode], value, prime);
      }
      (*join_signature_pairs)++;
    }
  }
  return true;
}

static bool fourier_table_find_signature(const rw_fourier_table_t *table, uint64_t signature,
                                         size_t *out) {
  for (size_t i = 0; i < table->len; i++) {
    if (table->signatures[i] == signature) {
      *out = i;
      return true;
    }
  }
  return false;
}


static bool solve_rankwidth_count_table(const qsop_instance_t *qsop,
                                        const qsop_rankwidth_decomposition_t *decomposition,
                                        const uint64_t *adj, qsop_result_t **out,
                                        qsop_solve_stats_t *stats,
                                        qsop_solve_trace_t *trace, qsop_error_t *error) {
  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  uint64_t join_pairs = 0;
  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_leaf(qsop, adj, node, &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.leaf", 0, tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_join_map(qsop, adj, node, &tables[node->left], &tables[node->right], &map, error);
      if (ok) {
        join_signature_pairs += map.len;
        qsop_trace_emit_elapsed(trace, "rankwidth.join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_join(qsop, &map, &tables[node->left], &tables[node->right], &tables[node_id],
                        &join_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.join", 0, tables[node_id].len, join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      qsop_result_free(result);
      return false;
    }
    table_entries += tables[node_id].len;
    signature_entries += tables[node_id].reps_len;
    if (tables[node_id].len > max_table_entries) {
      max_table_entries = tables[node_id].len;
    }
    if (tables[node_id].reps_len > max_signature_entries) {
      max_signature_entries = tables[node_id].reps_len;
    }
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
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_pairs;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        table_free(&tables[t]);
      }
      free(tables);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    table_free(&tables[t]);
  }
  free(tables);
  *out = result;
  return true;
}

static bool solve_rankwidth_fourier(const qsop_instance_t *qsop,
                                    const qsop_rankwidth_decomposition_t *decomposition,
                                    const uint64_t *adj, qsop_result_t **out,
                                    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                    qsop_error_t *error) {
  uint64_t prime = 0;
  uint64_t root = 0;
  uint64_t inv_root = 0;
  uint64_t *powers = NULL;
  uint64_t *inv_powers = NULL;
  if (!find_ntt_prime(qsop->r, qsop->nvars, &prime, error) ||
      !find_order_root(prime, qsop->r, &root, error)) {
    return false;
  }
  inv_root = pow_mod_u64(root, prime - 2U, prime);
  if (!make_root_powers(qsop->r, root, prime, &powers, error) ||
      !make_root_powers(qsop->r, inv_root, prime, &inv_powers, error)) {
    free(powers);
    free(inv_powers);
    return false;
  }

  qsop_result_t *result = calloc(1, sizeof(*result));
  rw_fourier_table_t *tables =
      calloc(decomposition->nnodes == 0 ? 1U : decomposition->nnodes, sizeof(*tables));
  if (result == NULL || tables == NULL || !qsop_counts_alloc(qsop->r, &result->counts, error)) {
    free(powers);
    free(inv_powers);
    free(tables);
    qsop_result_free(result);
    set_error(error, "out of memory while allocating rankwidth Fourier solve state");
    return false;
  }
  result->r = qsop->r;
  result->norm_h = qsop->norm_h;

  uint64_t join_signature_pairs = 0;
  uint64_t table_entries = 0;
  uint64_t signature_entries = 0;
  uint64_t max_table_entries = 0;
  uint64_t max_signature_entries = 0;
  for (uint32_t i = 0; i < decomposition->postorder_len; i++) {
    const uint32_t node_id = decomposition->postorder[i];
    const rw_node_t *node = &decomposition->nodes[node_id];
    const uint64_t start = qsop_trace_begin(trace);
    bool ok = false;
    if (node->kind == RW_NODE_LEAF) {
      ok = solve_fourier_leaf(qsop, adj, node, powers, prime, &tables[node_id], error);
      qsop_trace_emit_elapsed(trace, "rankwidth.fourier_leaf", 0, tables[node_id].len, start);
    } else {
      rw_join_map_t map = {0};
      ok = build_fourier_join_map(qsop, adj, node, &tables[node->left], &tables[node->right],
                                  &map, error);
      if (ok) {
        qsop_trace_emit_elapsed(trace, "rankwidth.fourier_join_map", 0, map.len, start);
        const uint64_t join_start = qsop_trace_begin(trace);
        ok = solve_fourier_join(qsop, &map, &tables[node->left], &tables[node->right], powers,
                                prime, &tables[node_id], &join_signature_pairs, error);
        qsop_trace_emit_elapsed(trace, "rankwidth.fourier_join", 0, tables[node_id].len,
                                join_start);
      }
      join_map_free(&map);
    }
    if (!ok) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        fourier_table_free(&tables[t]);
      }
      free(tables);
      free(powers);
      free(inv_powers);
      qsop_result_free(result);
      return false;
    }
    const uint64_t node_entries = (uint64_t)tables[node_id].len * qsop->r;
    table_entries += node_entries;
    signature_entries += tables[node_id].len;
    if (node_entries > max_table_entries) {
      max_table_entries = node_entries;
    }
    if (tables[node_id].len > max_signature_entries) {
      max_signature_entries = tables[node_id].len;
    }
  }

  const rw_fourier_table_t *root_table = &tables[decomposition->root];
  size_t root_index = 0;
  if (fourier_table_find_signature(root_table, 0, &root_index)) {
    const uint64_t inv_r = pow_mod_u64(qsop->r, prime - 2U, prime);
    for (uint32_t residue = 0; residue < qsop->r; residue++) {
      uint64_t sum = 0;
      for (uint32_t mode = 0; mode < qsop->r; mode++) {
        uint64_t value = root_table->values[root_index * (size_t)qsop->r + mode];
        value = mul_mod_u64(value, powers[(size_t)mode * qsop->r + (qsop->constant % qsop->r)],
                            prime);
        value = mul_mod_u64(value, inv_powers[(size_t)mode * qsop->r + residue], prime);
        sum = add_mod_u64(sum, value, prime);
      }
      result->counts[residue] = mul_mod_u64(sum, inv_r, prime);
    }
  }

  if (stats != NULL) {
    stats->table_entries = table_entries;
    stats->max_table_entries = max_table_entries;
    stats->signature_entries = signature_entries;
    stats->max_signature_entries = max_signature_entries;
    stats->join_pairs = join_signature_pairs * qsop->r;
    stats->join_signature_pairs = join_signature_pairs;
    stats->decomposition_width = decomposition_width(decomposition, adj, error);
    if (stats->decomposition_width == UINT32_MAX) {
      for (uint32_t t = 0; t < decomposition->nnodes; t++) {
        fourier_table_free(&tables[t]);
      }
      free(tables);
      free(powers);
      free(inv_powers);
      qsop_result_free(result);
      return false;
    }
  }

  for (uint32_t t = 0; t < decomposition->nnodes; t++) {
    fourier_table_free(&tables[t]);
  }
  free(tables);
  free(powers);
  free(inv_powers);
  *out = result;
  return true;
}

bool qsop_solve_rankwidth_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode, qsop_result_t **out,
    qsop_solve_stats_t *stats, qsop_solve_trace_t *trace, qsop_error_t *error) {
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
  const bool ok =
      mode == QSOP_RANKWIDTH_SOLVE_FOURIER
          ? solve_rankwidth_fourier(qsop, decomposition, adj, out, stats, trace, error)
          : solve_rankwidth_count_table(qsop, decomposition, adj, out, stats, trace, error);
  free(adj);
  return ok;
}

bool qsop_solve_rankwidth_trace_stats(const qsop_instance_t *qsop,
                                      const qsop_rankwidth_decomposition_t *decomposition,
                                      uint32_t max_vars, qsop_result_t **out,
                                      qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                      qsop_error_t *error) {
  return qsop_solve_rankwidth_mode_trace_stats(qsop, decomposition, max_vars,
                                               QSOP_RANKWIDTH_SOLVE_COUNT_TABLE, out, stats,
                                               trace, error);
}
