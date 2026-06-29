#define _POSIX_C_SOURCE 200809L

#include "dlx4sop/qsop.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct raw_edge {
  uint32_t u;
  uint32_t v;
} raw_edge_t;

typedef struct edge_vec {
  raw_edge_t *items;
  uint32_t len;
  uint32_t cap;
} edge_vec_t;

typedef struct parser {
  FILE *file;
  const char *path;
  qsop_error_t *error;
  size_t line_no;

  bool have_header;
  uint32_t r;
  uint32_t nvars;
  uint32_t expected_terms;
  uint32_t seen_terms;
  uint64_t norm_h;
  uint32_t constant;

  uint32_t *unary;
  int8_t *pins;
  edge_vec_t edges;
} parser_t;

static void set_error(parser_t *parser, size_t column, const char *fmt, ...) {
  if (parser->error == NULL) {
    return;
  }

  parser->error->path = parser->path;
  parser->error->line = parser->line_no;
  parser->error->column = column;

  va_list args;
  va_start(args, fmt);
  vsnprintf(parser->error->message, sizeof(parser->error->message), fmt, args);
  va_end(args);
}

static void set_global_error(qsop_error_t *error, const char *path, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }

  error->path = path;
  error->line = 0;
  error->column = 0;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
}

static uint32_t add_mod_u32(uint32_t a, uint32_t b, uint32_t r) {
  return (uint32_t)(((uint64_t)a + (uint64_t)b) % (uint64_t)r);
}

static bool reserve_edges(edge_vec_t *vec, uint32_t needed) {
  if (needed <= vec->cap) {
    return true;
  }

  uint32_t new_cap = vec->cap == 0 ? 16U : vec->cap;
  while (new_cap < needed) {
    if (new_cap > UINT32_MAX / 2U) {
      return false;
    }
    new_cap *= 2U;
  }

  raw_edge_t *new_items = realloc(vec->items, (size_t)new_cap * sizeof(*new_items));
  if (new_items == NULL) {
    return false;
  }

  vec->items = new_items;
  vec->cap = new_cap;
  return true;
}

static bool push_edge(parser_t *parser, uint32_t u, uint32_t v, size_t column) {
  if (!reserve_edges(&parser->edges, parser->edges.len + 1U)) {
    set_error(parser, column, "out of memory while storing quadratic terms");
    return false;
  }

  parser->edges.items[parser->edges.len] = (raw_edge_t){.u = u, .v = v};
  parser->edges.len++;
  return true;
}

static char *skip_spaces(char *text) {
  while (*text != '\0' && isspace((unsigned char)*text)) {
    text++;
  }
  return text;
}

static size_t column_of(const char *line, const char *ptr) {
  return (size_t)(ptr - line) + 1U;
}

static char *next_token(char **cursor) {
  char *start = skip_spaces(*cursor);
  if (*start == '\0') {
    *cursor = start;
    return NULL;
  }

  char *end = start;
  while (*end != '\0' && !isspace((unsigned char)*end)) {
    end++;
  }
  if (*end != '\0') {
    *end = '\0';
    end++;
  }
  *cursor = end;
  return start;
}

static bool no_more_tokens(parser_t *parser, const char *line, char **cursor) {
  char *extra = next_token(cursor);
  if (extra == NULL) {
    return true;
  }
  set_error(parser, column_of(line, extra), "unexpected extra token '%s'", extra);
  return false;
}

static bool parse_u32_token(parser_t *parser, const char *line, const char *token,
                            uint32_t *out) {
  if (token == NULL) {
    set_error(parser, 1, "missing unsigned integer");
    return false;
  }
  if (token[0] == '-') {
    set_error(parser, column_of(line, token), "negative value '%s' is not allowed", token);
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(token, &end, 10);
  if (errno != 0 || end == token || *end != '\0' || value > UINT32_MAX) {
    set_error(parser, column_of(line, token), "invalid uint32 value '%s'", token);
    return false;
  }

  *out = (uint32_t)value;
  return true;
}

static bool parse_u64_token(parser_t *parser, const char *line, const char *token,
                            uint64_t *out) {
  if (token == NULL) {
    set_error(parser, 1, "missing unsigned integer");
    return false;
  }
  if (token[0] == '-') {
    set_error(parser, column_of(line, token), "negative value '%s' is not allowed", token);
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(token, &end, 10);
  if (errno != 0 || end == token || *end != '\0') {
    set_error(parser, column_of(line, token), "invalid uint64 value '%s'", token);
    return false;
  }

  *out = (uint64_t)value;
  return true;
}

static bool parse_coeff_token(parser_t *parser, const char *line, const char *token,
                              uint32_t *out) {
  uint32_t raw = 0;
  if (!parse_u32_token(parser, line, token, &raw)) {
    return false;
  }
  if (!parser->have_header) {
    set_error(parser, column_of(line, token), "coefficient appears before header");
    return false;
  }
  *out = raw % parser->r;
  return true;
}

static bool parse_vertex_token(parser_t *parser, const char *line, const char *token,
                               uint32_t *out) {
  uint32_t value = 0;
  if (!parse_u32_token(parser, line, token, &value)) {
    return false;
  }
  if (!parser->have_header) {
    set_error(parser, column_of(line, token), "vertex appears before header");
    return false;
  }
  if (value >= parser->nvars) {
    set_error(parser, column_of(line, token), "vertex %" PRIu32 " is outside 0..%" PRIu32,
              value, parser->nvars == 0 ? 0 : parser->nvars - 1U);
    return false;
  }

  *out = value;
  return true;
}

static bool ensure_header(parser_t *parser, const char *line, const char *token) {
  if (parser->have_header) {
    return true;
  }
  set_error(parser, column_of(line, token), "line '%s' appears before p header", token);
  return false;
}

static bool parse_header(parser_t *parser, const char *line, char **cursor) {
  if (parser->have_header) {
    set_error(parser, 1, "duplicate p header");
    return false;
  }

  char *kind = next_token(cursor);
  char *r_tok = next_token(cursor);
  char *nvars_tok = next_token(cursor);
  char *terms_tok = next_token(cursor);
  if (kind == NULL || r_tok == NULL || nvars_tok == NULL || terms_tok == NULL) {
    set_error(parser, 1, "p header must be: p qsop-sign <r> <num_vars> <num_terms>");
    return false;
  }

  if (strcmp(kind, "qsop-sign") != 0) {
    set_error(parser, column_of(line, kind),
              "QSOP header must use p qsop-sign");
    return false;
  }

  uint32_t r = 0;
  uint32_t nvars = 0;
  uint32_t expected_terms = 0;
  if (!parse_u32_token(parser, line, r_tok, &r) ||
      !parse_u32_token(parser, line, nvars_tok, &nvars) ||
      !parse_u32_token(parser, line, terms_tok, &expected_terms)) {
    return false;
  }

  if (r == 0 || (r % 2U) != 0) {
    set_error(parser, column_of(line, r_tok), "modulus r must be a positive even integer");
    return false;
  }

  parser->unary = calloc(nvars == 0 ? 1U : nvars, sizeof(*parser->unary));
  parser->pins = malloc((nvars == 0 ? 1U : nvars) * sizeof(*parser->pins));
  if (parser->unary == NULL || parser->pins == NULL) {
    set_error(parser, 1, "out of memory while allocating instance");
    return false;
  }
  for (uint32_t i = 0; i < nvars; i++) {
    parser->pins[i] = -1;
  }

  parser->r = r;
  parser->nvars = nvars;
  parser->expected_terms = expected_terms;
  parser->have_header = true;
  return no_more_tokens(parser, line, cursor);
}

static bool parse_norm(parser_t *parser, const char *line, char **cursor) {
  char *norm_tok = next_token(cursor);
  if (norm_tok == NULL) {
    set_error(parser, 1, "n line must be: n <normalization_h>");
    return false;
  }
  if (!parse_u64_token(parser, line, norm_tok, &parser->norm_h)) {
    return false;
  }
  return no_more_tokens(parser, line, cursor);
}

static bool parse_constant(parser_t *parser, const char *line, char **cursor) {
  char *constant_tok = next_token(cursor);
  if (constant_tok == NULL) {
    set_error(parser, 1, "cst line must be: cst <constant_mod_r>");
    return false;
  }
  if (!parse_coeff_token(parser, line, constant_tok, &parser->constant)) {
    return false;
  }
  return no_more_tokens(parser, line, cursor);
}

static bool parse_unary(parser_t *parser, const char *line, char **cursor) {
  char *v_tok = next_token(cursor);
  char *b_tok = next_token(cursor);
  if (v_tok == NULL || b_tok == NULL) {
    set_error(parser, 1, "u line must be: u <vertex> <coefficient_mod_r>");
    return false;
  }

  uint32_t v = 0;
  uint32_t b = 0;
  if (!parse_vertex_token(parser, line, v_tok, &v) ||
      !parse_coeff_token(parser, line, b_tok, &b)) {
    return false;
  }

  parser->unary[v] = add_mod_u32(parser->unary[v], b, parser->r);
  return no_more_tokens(parser, line, cursor);
}

static bool parse_pin(parser_t *parser, const char *line, char **cursor) {
  char *v_tok = next_token(cursor);
  char *value_tok = next_token(cursor);
  if (v_tok == NULL || value_tok == NULL) {
    set_error(parser, 1, "f line must be: f <vertex> <0-or-1>");
    return false;
  }

  uint32_t v = 0;
  uint32_t value = 0;
  if (!parse_vertex_token(parser, line, v_tok, &v) ||
      !parse_u32_token(parser, line, value_tok, &value)) {
    return false;
  }
  if (value > 1U) {
    set_error(parser, column_of(line, value_tok), "pin value must be 0 or 1");
    return false;
  }
  if (parser->pins[v] != -1 && parser->pins[v] != (int8_t)value) {
    set_error(parser, column_of(line, value_tok), "conflicting pin for vertex %" PRIu32, v);
    return false;
  }

  parser->pins[v] = (int8_t)value;
  return no_more_tokens(parser, line, cursor);
}

static bool parse_edge(parser_t *parser, const char *line, char **cursor) {
  char *u_tok = next_token(cursor);
  char *v_tok = next_token(cursor);
  if (u_tok == NULL || v_tok == NULL) {
    set_error(parser, 1, "e line must be: e <u> <v>");
    return false;
  }

  uint32_t u = 0;
  uint32_t v = 0;
  if (!parse_vertex_token(parser, line, u_tok, &u) ||
      !parse_vertex_token(parser, line, v_tok, &v)) {
    return false;
  }

  parser->seen_terms++;
  if (!push_edge(parser, u, v, column_of(line, u_tok))) {
    return false;
  }
  return no_more_tokens(parser, line, cursor);
}

static int compare_edges(const void *left, const void *right) {
  const raw_edge_t *a = left;
  const raw_edge_t *b = right;
  if (a->u != b->u) {
    return a->u < b->u ? -1 : 1;
  }
  if (a->v != b->v) {
    return a->v < b->v ? -1 : 1;
  }
  return 0;
}

static bool add_normalized_edge(edge_vec_t *vec, uint32_t u, uint32_t v, qsop_error_t *error,
                                const char *path) {
  if (!reserve_edges(vec, vec->len + 1U)) {
    set_global_error(error, path, "out of memory while normalizing quadratic terms");
    return false;
  }
  vec->items[vec->len] = (raw_edge_t){.u = u, .v = v};
  vec->len++;
  return true;
}

static bool normalize(parser_t *parser, qsop_instance_t **out) {
  if (parser->seen_terms != parser->expected_terms) {
    set_global_error(parser->error, parser->path,
                     "header declares %" PRIu32 " quadratic terms but parsed %" PRIu32,
                     parser->expected_terms, parser->seen_terms);
    return false;
  }

  uint32_t *renumber = malloc((parser->nvars == 0 ? 1U : parser->nvars) * sizeof(*renumber));
  uint32_t *unary = calloc(parser->nvars == 0 ? 1U : parser->nvars, sizeof(*unary));
  if (renumber == NULL || unary == NULL) {
    free(renumber);
    free(unary);
    set_global_error(parser->error, parser->path, "out of memory while normalizing instance");
    return false;
  }

  uint32_t nfree = 0;
  uint32_t constant = parser->constant;
  const uint32_t sign_coeff = parser->r / 2U;
  for (uint32_t v = 0; v < parser->nvars; v++) {
    if (parser->pins[v] == 1) {
      constant = add_mod_u32(constant, parser->unary[v], parser->r);
      renumber[v] = UINT32_MAX;
    } else if (parser->pins[v] == 0) {
      renumber[v] = UINT32_MAX;
    } else {
      renumber[v] = nfree;
      unary[nfree] = parser->unary[v];
      nfree++;
    }
  }

  edge_vec_t kept = {0};
  for (uint32_t i = 0; i < parser->edges.len; i++) {
    raw_edge_t edge = parser->edges.items[i];
    const int8_t pin_u = parser->pins[edge.u];
    const int8_t pin_v = parser->pins[edge.v];
    if (pin_u != -1 || pin_v != -1) {
      if (edge.u == edge.v) {
        if (pin_u == 1) {
          constant = add_mod_u32(constant, sign_coeff, parser->r);
        }
        continue;
      }
      if (pin_u != -1 && pin_v != -1) {
        if (pin_u == 1 && pin_v == 1) {
          constant = add_mod_u32(constant, sign_coeff, parser->r);
        }
        continue;
      }
      if (pin_u == 1 && pin_v == -1) {
        const uint32_t v = renumber[edge.v];
        unary[v] = add_mod_u32(unary[v], sign_coeff, parser->r);
      } else if (pin_v == 1 && pin_u == -1) {
        const uint32_t u = renumber[edge.u];
        unary[u] = add_mod_u32(unary[u], sign_coeff, parser->r);
      }
      continue;
    }

    uint32_t u = renumber[edge.u];
    uint32_t v = renumber[edge.v];
    if (u == v) {
      unary[u] = add_mod_u32(unary[u], sign_coeff, parser->r);
      continue;
    }
    if (u > v) {
      uint32_t tmp = u;
      u = v;
      v = tmp;
    }
    if (!add_normalized_edge(&kept, u, v, parser->error, parser->path)) {
      free(renumber);
      free(unary);
      free(kept.items);
      return false;
    }
  }

  if (kept.len > 1) {
    qsort(kept.items, kept.len, sizeof(*kept.items), compare_edges);
  }

  uint32_t out_edges = 0;
  for (uint32_t i = 0; i < kept.len;) {
    raw_edge_t edge = kept.items[i];
    uint32_t parity = 0;
    do {
      parity ^= 1U;
      i++;
    } while (i < kept.len && kept.items[i].u == edge.u && kept.items[i].v == edge.v);

    if (parity != 0U) {
      kept.items[out_edges] = (raw_edge_t){.u = edge.u, .v = edge.v};
      out_edges++;
    }
  }
  kept.len = out_edges;

  qsop_instance_t *qsop = calloc(1, sizeof(*qsop));
  if (qsop == NULL) {
    free(renumber);
    free(unary);
    free(kept.items);
    set_global_error(parser->error, parser->path, "out of memory while allocating instance");
    return false;
  }

  qsop->r = parser->r;
  qsop->nvars = nfree;
  qsop->norm_h = parser->norm_h;
  qsop->constant = constant;
  qsop->unary = unary;
  qsop->nedges = kept.len;
  qsop->edge_u = calloc(kept.len == 0 ? 1U : kept.len, sizeof(*qsop->edge_u));
  qsop->edge_v = calloc(kept.len == 0 ? 1U : kept.len, sizeof(*qsop->edge_v));
  if (qsop->edge_u == NULL || qsop->edge_v == NULL) {
    free(renumber);
    free(kept.items);
    qsop_free(qsop);
    set_global_error(parser->error, parser->path, "out of memory while allocating edges");
    return false;
  }

  for (uint32_t i = 0; i < kept.len; i++) {
    qsop->edge_u[i] = kept.items[i].u;
    qsop->edge_v[i] = kept.items[i].v;
  }

  free(renumber);
  free(kept.items);
  *out = qsop;
  return true;
}

static bool parse_line(parser_t *parser, char *line) {
  char *cursor = line;
  char *token = next_token(&cursor);
  if (token == NULL) {
    return true;
  }
  if (strcmp(token, "c") == 0) {
    return true;
  }
  if (strcmp(token, "p") == 0) {
    return parse_header(parser, line, &cursor);
  }

  if (!ensure_header(parser, line, token)) {
    return false;
  }

  if (strcmp(token, "n") == 0) {
    return parse_norm(parser, line, &cursor);
  }
  if (strcmp(token, "cst") == 0) {
    return parse_constant(parser, line, &cursor);
  }
  if (strcmp(token, "u") == 0) {
    return parse_unary(parser, line, &cursor);
  }
  if (strcmp(token, "q") == 0) {
    set_error(parser, column_of(line, token),
              "quadratic coefficients are not supported; use sign edge e <u> <v>");
    return false;
  }
  if (strcmp(token, "e") == 0) {
    return parse_edge(parser, line, &cursor);
  }
  if (strcmp(token, "f") == 0) {
    return parse_pin(parser, line, &cursor);
  }

  set_error(parser, column_of(line, token), "unknown directive '%s'", token);
  return false;
}

bool qsop_parse_file(FILE *file, const char *path, qsop_instance_t **out, qsop_error_t *error) {
  if (out == NULL) {
    set_global_error(error, path, "internal error: null output pointer");
    return false;
  }
  *out = NULL;

  if (file == NULL) {
    set_global_error(error, path, "internal error: null file pointer");
    return false;
  }

  parser_t parser = {
      .file = file,
      .path = path,
      .error = error,
  };

  char *line = NULL;
  size_t cap = 0;
  ssize_t len = 0;
  bool ok = true;
  while ((len = getline(&line, &cap, parser.file)) >= 0) {
    parser.line_no++;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[len - 1] = '\0';
      len--;
    }
    if (!parse_line(&parser, line)) {
      ok = false;
      break;
    }
  }

  if (ok && ferror(file)) {
    set_global_error(error, path, "read failed: %s", strerror(errno));
    ok = false;
  }
  if (ok && !parser.have_header) {
    set_global_error(error, path, "missing p header");
    ok = false;
  }

  if (ok) {
    ok = normalize(&parser, out);
  }

  free(line);
  free(parser.unary);
  free(parser.pins);
  free(parser.edges.items);
  return ok;
}
