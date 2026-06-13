#define _GNU_SOURCE

#include "dlx4sop/qsop.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct qasm_reg {
  char *name;
  uint32_t size;
  uint32_t offset;
} qasm_reg_t;

typedef struct qasm_unary {
  uint32_t v;
  uint32_t q;
} qasm_unary_t;

typedef struct qasm_edge {
  uint32_t u;
  uint32_t v;
  uint32_t q;
} qasm_edge_t;

typedef enum qasm_one_qubit_op {
  QASM_ONE_ID,
  QASM_ONE_PHASE,
  QASM_ONE_H,
  QASM_ONE_X,
  QASM_ONE_Y,
} qasm_one_qubit_op_t;

typedef enum qasm_two_qubit_op {
  QASM_TWO_CZ,
  QASM_TWO_CPHASE,
  QASM_TWO_CX,
  QASM_TWO_CY,
  QASM_TWO_SWAP,
} qasm_two_qubit_op_t;

typedef struct qasm_operand {
  bool is_reg;
  uint32_t qubit;
  qasm_reg_t *reg;
} qasm_operand_t;

typedef struct qasm_importer {
  const char *path;
  size_t line_no;
  char error[256];

  qasm_reg_t *regs;
  size_t regs_len;
  size_t regs_cap;

  uint32_t *current;
  uint32_t nqubits;
  uint32_t qubits_cap;

  qasm_unary_t *unary;
  uint32_t unary_len;
  uint32_t unary_cap;

  qasm_edge_t *edges;
  uint32_t edges_len;
  uint32_t edges_cap;

  uint32_t constant;
  uint32_t nvars;
  uint64_t norm_h;
  const char *input_bits;
  const char *output_bits;
  bool have_openqasm;
  bool saw_gate;
} qasm_importer_t;

static void set_error(qasm_importer_t *importer, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(importer->error, sizeof(importer->error), fmt, args);
  va_end(args);
}

static void print_usage(FILE *file) {
  fputs("usage: qasm2sop [--input BITS] [--output BITS] [PATH|-]\n", file);
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text)) {
    text++;
  }

  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  return text;
}

static bool starts_with_keyword(const char *text, const char *keyword) {
  const size_t len = strlen(keyword);
  return strncmp(text, keyword, len) == 0 &&
         (text[len] == '\0' || isspace((unsigned char)text[len]));
}

static bool valid_identifier(const char *name) {
  if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
    return false;
  }
  for (const char *p = name + 1; *p != '\0'; p++) {
    if (!(isalnum((unsigned char)*p) || *p == '_')) {
      return false;
    }
  }
  return true;
}

static bool parse_u32_text(const char *text, uint32_t *out) {
  if (text[0] == '-' || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *trim(end) != '\0' || value > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)value;
  return true;
}

static uint32_t mod16_i64(int64_t value) {
  int64_t residue = value % 16;
  if (residue < 0) {
    residue += 16;
  }
  return (uint32_t)residue;
}

static bool validate_boundary_bits(const char *option, const char *bits, uint32_t nqubits,
                                   char *error, size_t error_size) {
  if (bits == NULL) {
    return true;
  }

  for (const char *p = bits; *p != '\0'; p++) {
    if (*p != '0' && *p != '1') {
      snprintf(error, error_size, "%s bitstring must contain only 0 or 1", option);
      return false;
    }
  }

  const size_t len = strlen(bits);
  if (len != (size_t)nqubits) {
    snprintf(error, error_size, "%s bitstring length %zu does not match %" PRIu32
                                " OpenQASM qubits",
             option, len, nqubits);
    return false;
  }
  return true;
}

static uint32_t boundary_bit(const char *bits, uint32_t index) {
  return bits == NULL ? 0U : (uint32_t)(bits[index] - '0');
}

static bool reserve_regs(qasm_importer_t *importer, size_t needed) {
  if (needed <= importer->regs_cap) {
    return true;
  }
  size_t new_cap = importer->regs_cap == 0 ? 4U : importer->regs_cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(importer, "too many qregs");
      return false;
    }
    new_cap *= 2U;
  }
  qasm_reg_t *regs = realloc(importer->regs, new_cap * sizeof(*regs));
  if (regs == NULL) {
    set_error(importer, "out of memory while storing qregs");
    return false;
  }
  importer->regs = regs;
  importer->regs_cap = new_cap;
  return true;
}

static bool reserve_current(qasm_importer_t *importer, uint32_t needed) {
  if (needed <= importer->qubits_cap) {
    return true;
  }
  uint32_t new_cap = importer->qubits_cap == 0 ? 8U : importer->qubits_cap;
  while (new_cap < needed) {
    if (new_cap > UINT32_MAX / 2U) {
      set_error(importer, "too many qubits");
      return false;
    }
    new_cap *= 2U;
  }
  uint32_t *current = realloc(importer->current, (size_t)new_cap * sizeof(*current));
  if (current == NULL) {
    set_error(importer, "out of memory while storing qubits");
    return false;
  }
  importer->current = current;
  importer->qubits_cap = new_cap;
  return true;
}

static bool reserve_unary(qasm_importer_t *importer, uint32_t needed) {
  if (needed <= importer->unary_cap) {
    return true;
  }
  uint32_t new_cap = importer->unary_cap == 0 ? 16U : importer->unary_cap;
  while (new_cap < needed) {
    if (new_cap > UINT32_MAX / 2U) {
      set_error(importer, "too many unary terms");
      return false;
    }
    new_cap *= 2U;
  }
  qasm_unary_t *unary = realloc(importer->unary, (size_t)new_cap * sizeof(*unary));
  if (unary == NULL) {
    set_error(importer, "out of memory while storing unary terms");
    return false;
  }
  importer->unary = unary;
  importer->unary_cap = new_cap;
  return true;
}

static bool reserve_edges(qasm_importer_t *importer, uint32_t needed) {
  if (needed <= importer->edges_cap) {
    return true;
  }
  uint32_t new_cap = importer->edges_cap == 0 ? 16U : importer->edges_cap;
  while (new_cap < needed) {
    if (new_cap > UINT32_MAX / 2U) {
      set_error(importer, "too many quadratic terms");
      return false;
    }
    new_cap *= 2U;
  }
  qasm_edge_t *edges = realloc(importer->edges, (size_t)new_cap * sizeof(*edges));
  if (edges == NULL) {
    set_error(importer, "out of memory while storing quadratic terms");
    return false;
  }
  importer->edges = edges;
  importer->edges_cap = new_cap;
  return true;
}

static qasm_reg_t *find_reg(qasm_importer_t *importer, const char *name) {
  for (size_t i = 0; i < importer->regs_len; i++) {
    if (strcmp(importer->regs[i].name, name) == 0) {
      return &importer->regs[i];
    }
  }
  return NULL;
}

static bool add_qreg(qasm_importer_t *importer, const char *name, uint32_t size) {
  if (importer->saw_gate) {
    set_error(importer, "qreg declarations must appear before gates");
    return false;
  }
  if (size == 0) {
    set_error(importer, "qreg '%s' must have positive size", name);
    return false;
  }
  if (find_reg(importer, name) != NULL) {
    set_error(importer, "duplicate qreg '%s'", name);
    return false;
  }
  if (UINT32_MAX - importer->nqubits < size || UINT32_MAX - importer->nvars < size) {
    set_error(importer, "too many qubits");
    return false;
  }
  if (!reserve_regs(importer, importer->regs_len + 1U) ||
      !reserve_current(importer, importer->nqubits + size)) {
    return false;
  }

  char *owned_name = strdup(name);
  if (owned_name == NULL) {
    set_error(importer, "out of memory while storing qreg name");
    return false;
  }

  const uint32_t offset = importer->nqubits;
  importer->regs[importer->regs_len++] = (qasm_reg_t){
      .name = owned_name,
      .size = size,
      .offset = offset,
  };
  for (uint32_t i = 0; i < size; i++) {
    importer->current[offset + i] = importer->nvars++;
  }
  importer->nqubits += size;
  return true;
}

static bool add_unary(qasm_importer_t *importer, uint32_t v, uint32_t q) {
  if (!reserve_unary(importer, importer->unary_len + 1U)) {
    return false;
  }
  importer->unary[importer->unary_len++] = (qasm_unary_t){.v = v, .q = q};
  return true;
}

static bool add_edge(qasm_importer_t *importer, uint32_t u, uint32_t v, uint32_t q) {
  if (!reserve_edges(importer, importer->edges_len + 1U)) {
    return false;
  }
  importer->edges[importer->edges_len++] = (qasm_edge_t){.u = u, .v = v, .q = q};
  return true;
}

static bool parse_qref(qasm_importer_t *importer, char *text, uint32_t *out_qubit) {
  text = trim(text);
  char *open = strchr(text, '[');
  char *close = open == NULL ? NULL : strchr(open + 1, ']');
  if (open == NULL || close == NULL || *trim(close + 1) != '\0') {
    set_error(importer, "qubit operand must look like qreg[index]");
    return false;
  }

  *open = '\0';
  *close = '\0';
  char *name = trim(text);
  char *index_text = trim(open + 1);
  if (!valid_identifier(name)) {
    set_error(importer, "invalid qreg name '%s'", name);
    return false;
  }

  uint32_t index = 0;
  if (!parse_u32_text(index_text, &index)) {
    set_error(importer, "invalid qreg index '%s'", index_text);
    return false;
  }

  qasm_reg_t *reg = find_reg(importer, name);
  if (reg == NULL) {
    set_error(importer, "unknown qreg '%s'", name);
    return false;
  }
  if (index >= reg->size) {
    set_error(importer, "qreg index %" PRIu32 " is outside '%s[%" PRIu32 "]'", index,
              reg->name, reg->size);
    return false;
  }

  *out_qubit = reg->offset + index;
  return true;
}

static bool parse_qreg(qasm_importer_t *importer, char *rest) {
  char *open = strchr(rest, '[');
  char *close = open == NULL ? NULL : strchr(open + 1, ']');
  if (open == NULL || close == NULL || *trim(close + 1) != '\0') {
    set_error(importer, "qreg declaration must be: qreg name[size]");
    return false;
  }

  *open = '\0';
  *close = '\0';
  char *name = trim(rest);
  char *size_text = trim(open + 1);
  if (!valid_identifier(name)) {
    set_error(importer, "invalid qreg name '%s'", name);
    return false;
  }

  uint32_t size = 0;
  if (!parse_u32_text(size_text, &size)) {
    set_error(importer, "invalid qreg size '%s'", size_text);
    return false;
  }
  return add_qreg(importer, name, size);
}

static uint32_t named_phase_coeff_for_gate(const char *gate) {
  if (strcmp(gate, "t") == 0) {
    return 2;
  }
  if (strcmp(gate, "s") == 0) {
    return 4;
  }
  if (strcmp(gate, "z") == 0) {
    return 8;
  }
  if (strcmp(gate, "sdg") == 0) {
    return 12;
  }
  if (strcmp(gate, "tdg") == 0) {
    return 14;
  }
  return UINT32_MAX;
}

static bool parse_pi_over_four_units(const char *expr, int64_t *out_units) {
  const char *p = expr;
  int64_t sign = 1;
  if (*p == '-') {
    sign = -1;
    p++;
  } else if (*p == '+') {
    p++;
  }

  if (strcmp(p, "0") == 0) {
    *out_units = 0;
    return true;
  }

  uint64_t multiplier = 1;
  if (isdigit((unsigned char)*p)) {
    multiplier = 0;
    do {
      const uint32_t digit = (uint32_t)(*p - '0');
      if (multiplier > (UINT64_MAX - digit) / 10U) {
        return false;
      }
      multiplier = multiplier * 10U + digit;
      p++;
    } while (isdigit((unsigned char)*p));
    if (*p != '*') {
      return false;
    }
    p++;
  }

  if (strncmp(p, "pi", 2) != 0) {
    return false;
  }
  p += 2;

  uint64_t denominator = 1;
  if (*p == '/') {
    p++;
    if (!isdigit((unsigned char)*p)) {
      return false;
    }
    denominator = 0;
    do {
      const uint32_t digit = (uint32_t)(*p - '0');
      if (denominator > (UINT64_MAX - digit) / 10U) {
        return false;
      }
      denominator = denominator * 10U + digit;
      p++;
    } while (isdigit((unsigned char)*p));
  }
  if (*p != '\0') {
    return false;
  }

  if (denominator != 1U && denominator != 2U && denominator != 4U) {
    return false;
  }

  const uint64_t scale = 4U / denominator;
  if (multiplier > (uint64_t)INT64_MAX / scale) {
    return false;
  }
  *out_units = sign * (int64_t)(multiplier * scale);
  return true;
}

static bool parse_param_phase_units(qasm_importer_t *importer, const char *gate,
                                    const char *prefix, const char *name, int64_t *out_units,
                                    bool *out_matches) {
  const size_t prefix_len = strlen(prefix);
  const size_t gate_len = strlen(gate);
  if (strncmp(gate, prefix, prefix_len) != 0) {
    *out_matches = false;
    return true;
  }
  *out_matches = true;

  if (gate_len <= prefix_len || gate[gate_len - 1U] != ')') {
    set_error(importer, "%s phase gate must be written as %s(<angle>)", name, name);
    return false;
  }

  const size_t expr_len = gate_len - prefix_len - 1U;
  char expr[64];
  if (expr_len == 0 || expr_len >= sizeof(expr)) {
    set_error(importer, "unsupported %s phase angle '%s'", name, gate);
    return false;
  }
  memcpy(expr, gate + prefix_len, expr_len);
  expr[expr_len] = '\0';

  int64_t units = 0;
  if (!parse_pi_over_four_units(expr, &units)) {
    set_error(importer, "unsupported %s phase angle '%s'", name, gate);
    return false;
  }
  *out_units = units;
  return true;
}

static bool parse_param_phase_coeff(qasm_importer_t *importer, const char *gate,
                                    const char *prefix, const char *name,
                                    uint32_t *out_coeff, bool *out_matches) {
  int64_t units = 0;
  if (!parse_param_phase_units(importer, gate, prefix, name, &units, out_matches)) {
    return false;
  }
  if (!*out_matches) {
    return true;
  }
  *out_coeff = mod16_i64(2 * units);
  return true;
}

static bool parse_u1_phase_coeff(qasm_importer_t *importer, const char *gate,
                                 uint32_t *out_coeff, bool *out_is_phase) {
  bool matches = false;
  if (!parse_param_phase_coeff(importer, gate, "u1(", "u1", out_coeff, &matches)) {
    return false;
  }
  if (matches) {
    *out_is_phase = true;
    return true;
  }
  if (!parse_param_phase_coeff(importer, gate, "p(", "p", out_coeff, &matches)) {
    return false;
  }
  *out_is_phase = matches;
  return true;
}

static bool phase_coeff_for_gate(qasm_importer_t *importer, const char *gate,
                                 uint32_t *out_coeff, bool *out_is_phase) {
  const uint32_t named_coeff = named_phase_coeff_for_gate(gate);
  if (named_coeff != UINT32_MAX) {
    *out_coeff = named_coeff;
    *out_is_phase = true;
    return true;
  }
  return parse_u1_phase_coeff(importer, gate, out_coeff, out_is_phase);
}

static bool controlled_phase_coeff_for_gate(qasm_importer_t *importer, const char *gate,
                                            uint32_t *out_coeff,
                                            bool *out_is_controlled_phase) {
  const uint32_t named_coeff = named_phase_coeff_for_gate(gate + 1);
  if (gate[0] == 'c' && named_coeff != UINT32_MAX) {
    *out_coeff = named_coeff;
    *out_is_controlled_phase = true;
    return true;
  }

  bool matches = false;
  if (!parse_param_phase_coeff(importer, gate, "cu1(", "cu1", out_coeff, &matches)) {
    return false;
  }
  if (matches) {
    *out_is_controlled_phase = true;
    return true;
  }
  if (!parse_param_phase_coeff(importer, gate, "cp(", "cp", out_coeff, &matches)) {
    return false;
  }
  *out_is_controlled_phase = matches;
  return true;
}

static bool rz_units_for_gate(qasm_importer_t *importer, const char *gate, const char *prefix,
                              const char *name, int64_t *out_units, bool *out_matches) {
  return parse_param_phase_units(importer, gate, prefix, name, out_units, out_matches);
}

static bool add_constant(qasm_importer_t *importer, uint32_t coeff) {
  importer->constant = (importer->constant + coeff) % 16U;
  return true;
}

static bool apply_phase(qasm_importer_t *importer, uint32_t qubit, uint32_t coeff) {
  if (coeff % 16U == 0) {
    return true;
  }
  return add_unary(importer, importer->current[qubit], coeff);
}

static bool apply_h(qasm_importer_t *importer, uint32_t qubit) {
  if (importer->nvars == UINT32_MAX || importer->norm_h == UINT64_MAX) {
    set_error(importer, "too many Hadamard gates");
    return false;
  }
  const uint32_t next_var = importer->nvars++;
  if (!add_edge(importer, importer->current[qubit], next_var, 8)) {
    return false;
  }
  importer->current[qubit] = next_var;
  importer->norm_h++;
  return true;
}

static bool apply_cz(qasm_importer_t *importer, uint32_t left, uint32_t right) {
  return add_edge(importer, importer->current[left], importer->current[right], 8);
}

static bool apply_controlled_phase(qasm_importer_t *importer, uint32_t left, uint32_t right,
                                   uint32_t coeff) {
  if (coeff % 16U == 0) {
    return true;
  }
  return add_edge(importer, importer->current[left], importer->current[right], coeff);
}

static bool apply_rz(qasm_importer_t *importer, uint32_t qubit, int64_t units) {
  return add_constant(importer, mod16_i64(-units)) &&
         apply_phase(importer, qubit, mod16_i64(2 * units));
}

static bool apply_crz(qasm_importer_t *importer, uint32_t control, uint32_t target,
                      int64_t units) {
  return apply_phase(importer, control, mod16_i64(-units)) &&
         apply_controlled_phase(importer, control, target, mod16_i64(2 * units));
}

static bool apply_x_decomposition(qasm_importer_t *importer, uint32_t qubit) {
  return apply_h(importer, qubit) && apply_phase(importer, qubit, 8) &&
         apply_h(importer, qubit);
}

static bool apply_y_decomposition(qasm_importer_t *importer, uint32_t qubit) {
  return apply_phase(importer, qubit, 12) && apply_x_decomposition(importer, qubit) &&
         apply_phase(importer, qubit, 4);
}

static bool apply_cx_decomposition(qasm_importer_t *importer, uint32_t control,
                                   uint32_t target) {
  return apply_h(importer, target) && apply_cz(importer, control, target) &&
         apply_h(importer, target);
}

static bool apply_cy_decomposition(qasm_importer_t *importer, uint32_t control,
                                   uint32_t target) {
  return apply_phase(importer, target, 12) &&
         apply_cx_decomposition(importer, control, target) && apply_phase(importer, target, 4);
}

static bool split_two_operands(qasm_importer_t *importer, char *rest, char **left,
                               char **right) {
  char *comma = strchr(rest, ',');
  if (comma == NULL) {
    set_error(importer, "two-qubit gate operands must be separated by comma");
    return false;
  }
  *comma = '\0';
  *left = rest;
  *right = comma + 1;
  return true;
}

static bool parse_qubit_or_reg_operand(qasm_importer_t *importer, char *text,
                                       qasm_operand_t *out) {
  text = trim(text);
  if (strchr(text, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, text, &qubit)) {
      return false;
    }
    *out = (qasm_operand_t){.is_reg = false, .qubit = qubit, .reg = NULL};
    return true;
  }

  if (!valid_identifier(text)) {
    set_error(importer, "invalid qreg name '%s'", text);
    return false;
  }
  qasm_reg_t *reg = find_reg(importer, text);
  if (reg == NULL) {
    set_error(importer, "unknown qreg '%s'", text);
    return false;
  }
  *out = (qasm_operand_t){.is_reg = true, .qubit = 0, .reg = reg};
  return true;
}

static bool apply_one_qubit_op(qasm_importer_t *importer, qasm_one_qubit_op_t op,
                               uint32_t qubit, uint32_t phase_coeff) {
  switch (op) {
  case QASM_ONE_ID:
    return true;
  case QASM_ONE_PHASE:
    return apply_phase(importer, qubit, phase_coeff);
  case QASM_ONE_H:
    return apply_h(importer, qubit);
  case QASM_ONE_X:
    return apply_x_decomposition(importer, qubit);
  case QASM_ONE_Y:
    return apply_y_decomposition(importer, qubit);
  }
  set_error(importer, "internal error: unknown one-qubit operation");
  return false;
}

static bool apply_one_qubit_operand(qasm_importer_t *importer, char *rest,
                                    qasm_one_qubit_op_t op, uint32_t phase_coeff) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply_one_qubit_op(importer, op, qubit, phase_coeff);
  }

  if (!valid_identifier(rest)) {
    set_error(importer, "invalid qreg name '%s'", rest);
    return false;
  }

  qasm_reg_t *reg = find_reg(importer, rest);
  if (reg == NULL) {
    set_error(importer, "unknown qreg '%s'", rest);
    return false;
  }
  for (uint32_t i = 0; i < reg->size; i++) {
    if (!apply_one_qubit_op(importer, op, reg->offset + i, phase_coeff)) {
      return false;
    }
  }
  return true;
}

static bool apply_rz_operand(qasm_importer_t *importer, char *rest, int64_t units) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply_rz(importer, qubit, units);
  }

  if (!valid_identifier(rest)) {
    set_error(importer, "invalid qreg name '%s'", rest);
    return false;
  }

  qasm_reg_t *reg = find_reg(importer, rest);
  if (reg == NULL) {
    set_error(importer, "unknown qreg '%s'", rest);
    return false;
  }
  for (uint32_t i = 0; i < reg->size; i++) {
    if (!apply_rz(importer, reg->offset + i, units)) {
      return false;
    }
  }
  return true;
}

static bool apply_two_qubit_op(qasm_importer_t *importer, qasm_two_qubit_op_t op,
                               uint32_t left, uint32_t right, uint32_t phase_coeff) {
  switch (op) {
  case QASM_TWO_CZ:
    return apply_cz(importer, left, right);
  case QASM_TWO_CPHASE:
    return apply_controlled_phase(importer, left, right, phase_coeff);
  case QASM_TWO_CX:
    return apply_cx_decomposition(importer, left, right);
  case QASM_TWO_CY:
    return apply_cy_decomposition(importer, left, right);
  case QASM_TWO_SWAP: {
    const uint32_t tmp = importer->current[left];
    importer->current[left] = importer->current[right];
    importer->current[right] = tmp;
    return true;
  }
  }
  set_error(importer, "internal error: unknown two-qubit operation");
  return false;
}

static bool apply_two_qubit_operands(qasm_importer_t *importer, char *rest,
                                     qasm_two_qubit_op_t op, uint32_t phase_coeff) {
  char *left_text = NULL;
  char *right_text = NULL;
  if (!split_two_operands(importer, rest, &left_text, &right_text)) {
    return false;
  }

  qasm_operand_t left = {0};
  qasm_operand_t right = {0};
  if (!parse_qubit_or_reg_operand(importer, left_text, &left) ||
      !parse_qubit_or_reg_operand(importer, right_text, &right)) {
    return false;
  }

  if (!left.is_reg && !right.is_reg) {
    return apply_two_qubit_op(importer, op, left.qubit, right.qubit, phase_coeff);
  }
  if (left.is_reg != right.is_reg) {
    set_error(importer, "two-qubit gates cannot mix qreg and indexed qubit operands");
    return false;
  }
  if (left.reg->size != right.reg->size) {
    set_error(importer, "two-qubit qreg operands must have matching sizes");
    return false;
  }

  for (uint32_t i = 0; i < left.reg->size; i++) {
    const uint32_t left_qubit = left.reg->offset + i;
    const uint32_t right_qubit = right.reg->offset + i;
    if (!apply_two_qubit_op(importer, op, left_qubit, right_qubit, phase_coeff)) {
      return false;
    }
  }
  return true;
}

static bool apply_crz_operands(qasm_importer_t *importer, char *rest, int64_t units) {
  char *left_text = NULL;
  char *right_text = NULL;
  if (!split_two_operands(importer, rest, &left_text, &right_text)) {
    return false;
  }

  qasm_operand_t left = {0};
  qasm_operand_t right = {0};
  if (!parse_qubit_or_reg_operand(importer, left_text, &left) ||
      !parse_qubit_or_reg_operand(importer, right_text, &right)) {
    return false;
  }

  if (!left.is_reg && !right.is_reg) {
    return apply_crz(importer, left.qubit, right.qubit, units);
  }
  if (left.is_reg != right.is_reg) {
    set_error(importer, "two-qubit gates cannot mix qreg and indexed qubit operands");
    return false;
  }
  if (left.reg->size != right.reg->size) {
    set_error(importer, "two-qubit qreg operands must have matching sizes");
    return false;
  }

  for (uint32_t i = 0; i < left.reg->size; i++) {
    const uint32_t left_qubit = left.reg->offset + i;
    const uint32_t right_qubit = right.reg->offset + i;
    if (!apply_crz(importer, left_qubit, right_qubit, units)) {
      return false;
    }
  }
  return true;
}

static bool apply_gate(qasm_importer_t *importer, char *gate, char *rest) {
  importer->saw_gate = true;

  int64_t rz_units = 0;
  bool is_rz = false;
  if (!rz_units_for_gate(importer, gate, "rz(", "rz", &rz_units, &is_rz)) {
    return false;
  }
  if (is_rz) {
    return apply_rz_operand(importer, rest, rz_units);
  }

  int64_t crz_units = 0;
  bool is_crz = false;
  if (!rz_units_for_gate(importer, gate, "crz(", "crz", &crz_units, &is_crz)) {
    return false;
  }
  if (is_crz) {
    return apply_crz_operands(importer, rest, crz_units);
  }

  uint32_t phase_coeff = 0;
  bool is_phase = false;
  if (!phase_coeff_for_gate(importer, gate, &phase_coeff, &is_phase)) {
    return false;
  }
  if (is_phase) {
    return apply_one_qubit_operand(importer, rest, QASM_ONE_PHASE, phase_coeff);
  }

  uint32_t controlled_phase_coeff = 0;
  bool is_controlled_phase = false;
  if (!controlled_phase_coeff_for_gate(importer, gate, &controlled_phase_coeff,
                                       &is_controlled_phase)) {
    return false;
  }
  if (is_controlled_phase) {
    return apply_two_qubit_operands(importer, rest, QASM_TWO_CPHASE, controlled_phase_coeff);
  }

  if (strcmp(gate, "h") == 0) {
    return apply_one_qubit_operand(importer, rest, QASM_ONE_H, 0);
  }

  if (strcmp(gate, "id") == 0) {
    return apply_one_qubit_operand(importer, rest, QASM_ONE_ID, 0);
  }

  if (strcmp(gate, "x") == 0) {
    return apply_one_qubit_operand(importer, rest, QASM_ONE_X, 0);
  }

  if (strcmp(gate, "y") == 0) {
    return apply_one_qubit_operand(importer, rest, QASM_ONE_Y, 0);
  }

  if (strcmp(gate, "cz") == 0) {
    return apply_two_qubit_operands(importer, rest, QASM_TWO_CZ, 0);
  }

  if (strcmp(gate, "cx") == 0) {
    return apply_two_qubit_operands(importer, rest, QASM_TWO_CX, 0);
  }

  if (strcmp(gate, "cy") == 0) {
    return apply_two_qubit_operands(importer, rest, QASM_TWO_CY, 0);
  }

  if (strcmp(gate, "swap") == 0) {
    return apply_two_qubit_operands(importer, rest, QASM_TWO_SWAP, 0);
  }

  set_error(importer, "unsupported OpenQASM operation '%s'", gate);
  return false;
}

static bool parse_statement(qasm_importer_t *importer, char *line) {
  char *comment = strstr(line, "//");
  if (comment != NULL) {
    *comment = '\0';
  }

  char *text = trim(line);
  if (*text == '\0') {
    return true;
  }
  const size_t len = strlen(text);
  if (text[len - 1U] != ';') {
    set_error(importer, "OpenQASM statements must end with ';'");
    return false;
  }
  text[len - 1U] = '\0';
  text = trim(text);
  if (*text == '\0') {
    return true;
  }

  if (starts_with_keyword(text, "OPENQASM")) {
    char *version = trim(text + strlen("OPENQASM"));
    if (strcmp(version, "2.0") != 0) {
      set_error(importer, "only OPENQASM 2.0 is supported");
      return false;
    }
    importer->have_openqasm = true;
    return true;
  }

  if (!importer->have_openqasm) {
    set_error(importer, "missing OPENQASM 2.0 header");
    return false;
  }

  if (starts_with_keyword(text, "include")) {
    return true;
  }
  if (starts_with_keyword(text, "qreg")) {
    return parse_qreg(importer, trim(text + strlen("qreg")));
  }
  if (starts_with_keyword(text, "barrier")) {
    return true;
  }
  if (starts_with_keyword(text, "creg") || starts_with_keyword(text, "measure") ||
      starts_with_keyword(text, "reset") || starts_with_keyword(text, "if")) {
    set_error(importer, "dynamic or classical OpenQASM features are not supported");
    return false;
  }

  char *rest = text;
  while (*rest != '\0' && !isspace((unsigned char)*rest)) {
    rest++;
  }
  if (*rest == '\0') {
    set_error(importer, "gate statement is missing operands");
    return false;
  }
  *rest = '\0';
  rest = trim(rest + 1);
  return apply_gate(importer, text, rest);
}

static void free_importer(qasm_importer_t *importer) {
  for (size_t i = 0; i < importer->regs_len; i++) {
    free(importer->regs[i].name);
  }
  free(importer->regs);
  free(importer->current);
  free(importer->unary);
  free(importer->edges);
}

static bool parse_qasm(FILE *input, const char *path, qasm_importer_t *importer) {
  importer->path = path;
  char *line = NULL;
  size_t cap = 0;
  ssize_t len = 0;
  bool ok = true;
  while ((len = getline(&line, &cap, input)) >= 0) {
    (void)len;
    importer->line_no++;
    if (!parse_statement(importer, line)) {
      ok = false;
      break;
    }
  }
  if (ok && ferror(input)) {
    set_error(importer, "read failed: %s", strerror(errno));
    ok = false;
  }
  if (ok && !importer->have_openqasm) {
    set_error(importer, "missing OPENQASM 2.0 header");
    ok = false;
  }
  free(line);
  return ok;
}

static bool write_zero_qsop(FILE *file, uint64_t norm_h) {
  fprintf(file, "p qsop 8 1 0\n");
  fprintf(file, "n %" PRIu64 "\n", norm_h);
  fputs("cst 0\n\n", file);
  fputs("u 0 4\n", file);
  return !ferror(file);
}

static bool pin_boundary_variable(int8_t *pins, uint32_t var, uint32_t value,
                                  bool *conflict) {
  if (pins[var] != -1 && pins[var] != (int8_t)value) {
    *conflict = true;
    return false;
  }
  pins[var] = (int8_t)value;
  return true;
}

static bool collect_boundary_pins(const qasm_importer_t *importer, int8_t **out_pins,
                                  bool *out_conflict) {
  const size_t pin_count = importer->nvars == 0 ? 1U : importer->nvars;
  int8_t *pins = malloc(pin_count * sizeof(*pins));
  if (pins == NULL) {
    return false;
  }
  for (size_t i = 0; i < pin_count; i++) {
    pins[i] = -1;
  }

  bool conflict = false;
  for (uint32_t q = 0; q < importer->nqubits && !conflict; q++) {
    const uint32_t value = boundary_bit(importer->input_bits, q);
    pin_boundary_variable(pins, q, value, &conflict);
  }
  for (uint32_t q = 0; q < importer->nqubits && !conflict; q++) {
    const uint32_t value = boundary_bit(importer->output_bits, q);
    pin_boundary_variable(pins, importer->current[q], value, &conflict);
  }

  *out_pins = pins;
  *out_conflict = conflict;
  return true;
}

static uint32_t output_modulus(const qasm_importer_t *importer) {
  if (importer->constant % 2U != 0) {
    return 16;
  }
  for (uint32_t i = 0; i < importer->unary_len; i++) {
    if (importer->unary[i].q % 2U != 0) {
      return 16;
    }
  }
  for (uint32_t i = 0; i < importer->edges_len; i++) {
    if (importer->edges[i].q % 2U != 0) {
      return 16;
    }
  }
  return 8;
}

static uint32_t output_coeff(uint32_t coeff, uint32_t modulus) {
  return modulus == 8 ? coeff / 2U : coeff;
}

static bool write_raw_qsop(FILE *file, const qasm_importer_t *importer) {
  int8_t *pins = NULL;
  bool boundary_conflict = false;
  if (!collect_boundary_pins(importer, &pins, &boundary_conflict)) {
    return false;
  }
  if (boundary_conflict) {
    free(pins);
    return write_zero_qsop(file, importer->norm_h);
  }

  const uint32_t modulus = output_modulus(importer);
  fprintf(file, "p qsop %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", modulus, importer->nvars,
          importer->edges_len);
  fprintf(file, "n %" PRIu64 "\n", importer->norm_h);
  fprintf(file, "cst %" PRIu32 "\n", output_coeff(importer->constant, modulus));

  for (uint32_t i = 0; i < importer->unary_len; i++) {
    fprintf(file, "u %" PRIu32 " %" PRIu32 "\n", importer->unary[i].v,
            output_coeff(importer->unary[i].q, modulus));
  }
  for (uint32_t i = 0; i < importer->edges_len; i++) {
    fprintf(file, "q %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", importer->edges[i].u,
            importer->edges[i].v, output_coeff(importer->edges[i].q, modulus));
  }
  for (uint32_t v = 0; v < importer->nvars; v++) {
    if (pins[v] != -1) {
      fprintf(file, "f %" PRIu32 " %d\n", v, pins[v]);
    }
  }

  free(pins);
  return !ferror(file);
}

static bool canonicalize_to_stdout(const qasm_importer_t *importer, qsop_error_t *error) {
  char *raw = NULL;
  size_t raw_len = 0;
  FILE *raw_file = open_memstream(&raw, &raw_len);
  if (raw_file == NULL) {
    snprintf(error->message, sizeof(error->message), "out of memory while writing raw QSOP");
    return false;
  }
  const bool write_ok = write_raw_qsop(raw_file, importer);
  const bool close_ok = fclose(raw_file) == 0;
  if (!write_ok || !close_ok) {
    free(raw);
    snprintf(error->message, sizeof(error->message), "failed to write raw QSOP");
    return false;
  }

  qsop_instance_t *qsop = NULL;
  FILE *input = fmemopen(raw, raw_len, "rb");
  if (input == NULL) {
    free(raw);
    snprintf(error->message, sizeof(error->message), "failed to read generated QSOP");
    return false;
  }
  bool ok = qsop_parse_file(input, "<generated-qsop>", &qsop, error);
  fclose(input);
  free(raw);
  if (!ok) {
    return false;
  }

  ok = qsop_write_file(stdout, qsop, error);
  qsop_free(qsop);
  return ok;
}

static void print_qsop_error(const qsop_error_t *error) {
  if (error->line > 0) {
    fprintf(stderr, "error: %s:%zu:%zu: %s\n", error->path, error->line, error->column,
            error->message);
  } else {
    fprintf(stderr, "error: %s\n", error->message);
  }
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *input_bits = NULL;
  const char *output_bits = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--input") == 0) {
      if (input_bits != NULL) {
        fputs("error: duplicate --input option\n", stderr);
        print_usage(stderr);
        return 2;
      }
      if (i + 1 >= argc) {
        fputs("error: missing value for --input\n", stderr);
        print_usage(stderr);
        return 2;
      }
      input_bits = argv[++i];
      continue;
    }
    if (strncmp(argv[i], "--input=", strlen("--input=")) == 0) {
      if (input_bits != NULL) {
        fputs("error: duplicate --input option\n", stderr);
        print_usage(stderr);
        return 2;
      }
      input_bits = argv[i] + strlen("--input=");
      continue;
    }
    if (strcmp(argv[i], "--output") == 0) {
      if (output_bits != NULL) {
        fputs("error: duplicate --output option\n", stderr);
        print_usage(stderr);
        return 2;
      }
      if (i + 1 >= argc) {
        fputs("error: missing value for --output\n", stderr);
        print_usage(stderr);
        return 2;
      }
      output_bits = argv[++i];
      continue;
    }
    if (strncmp(argv[i], "--output=", strlen("--output=")) == 0) {
      if (output_bits != NULL) {
        fputs("error: duplicate --output option\n", stderr);
        print_usage(stderr);
        return 2;
      }
      output_bits = argv[i] + strlen("--output=");
      continue;
    }
    if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
    if (input_path != NULL) {
      fputs("error: expected at most one input path\n", stderr);
      print_usage(stderr);
      return 2;
    }
    input_path = argv[i];
  }

  FILE *input = stdin;
  const char *diagnostic_path = "<stdin>";
  if (input_path != NULL && strcmp(input_path, "-") != 0) {
    input = fopen(input_path, "r");
    diagnostic_path = input_path;
    if (input == NULL) {
      fprintf(stderr, "error: %s: %s\n", input_path, strerror(errno));
      return 1;
    }
  }

  qasm_importer_t importer = {0};
  importer.input_bits = input_bits;
  importer.output_bits = output_bits;
  bool ok = parse_qasm(input, diagnostic_path, &importer);
  if (input != stdin) {
    fclose(input);
  }
  if (!ok) {
    fprintf(stderr, "error: %s:%zu: %s\n", diagnostic_path, importer.line_no, importer.error);
    free_importer(&importer);
    return 1;
  }

  char boundary_error[256] = {0};
  if (!validate_boundary_bits("--input", input_bits, importer.nqubits, boundary_error,
                              sizeof(boundary_error)) ||
      !validate_boundary_bits("--output", output_bits, importer.nqubits, boundary_error,
                              sizeof(boundary_error))) {
    fprintf(stderr, "error: %s\n", boundary_error);
    free_importer(&importer);
    return 2;
  }

  qsop_error_t error = {0};
  ok = canonicalize_to_stdout(&importer, &error);
  free_importer(&importer);
  if (!ok) {
    print_qsop_error(&error);
    return 1;
  }
  return 0;
}
