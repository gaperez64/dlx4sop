#define _GNU_SOURCE

#include "dlx4sop/min_fill.h"
#include "dlx4sop/qsop.h"
#include "cli_common.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QASM_PI 3.141592653589793238462643383279502884
#define QASM_PI_L 3.141592653589793238462643383279502884L
#define QASM_TWO_PI_L (2.0L * QASM_PI_L)

typedef struct qasm_reg {
  char *name;
  uint32_t size;
  uint32_t offset;
} qasm_reg_t;

typedef struct qasm_unary {
  uint32_t v;
  uint64_t q;
} qasm_unary_t;

typedef struct qasm_edge {
  uint32_t u;
  uint32_t v;
  uint64_t q;
} qasm_edge_t;

/* The computational-basis value carried by a wire. CNOT networks transform basis values by
 * affine GF(2) maps, so keeping the XOR explicitly avoids lowering every CNOT through H-CZ-H.
 * `vars` is sorted and duplicate-free; `constant` is the affine offset. */
typedef struct qasm_affine {
  uint32_t *vars;
  uint32_t len;
  uint32_t cap;
  bool constant;
  bool has_observed_value;
  uint32_t observed_value;
} qasm_affine_t;

typedef enum qasm_one_qubit_op {
  QASM_ONE_ID,
  QASM_ONE_PHASE,
  QASM_ONE_H,
  QASM_ONE_X,
  QASM_ONE_Y,
  QASM_ONE_SX,
  QASM_ONE_SXDG,
} qasm_one_qubit_op_t;

typedef enum qasm_two_qubit_op {
  QASM_TWO_CZ,
  QASM_TWO_CPHASE,
  QASM_TWO_CH,
  QASM_TWO_CX,
  QASM_TWO_CY,
  QASM_TWO_CSX,
  QASM_TWO_CSXDG,
  QASM_TWO_SWAP,
  QASM_TWO_DCX,
  QASM_TWO_ISWAP,
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

  qasm_affine_t *current;
  uint32_t nqubits;
  uint32_t qubits_cap;

  qasm_unary_t *unary;
  uint32_t unary_len;
  uint32_t unary_cap;

  qasm_edge_t *edges;
  uint32_t edges_len;
  uint32_t edges_cap;

  /* Variables introduced by `reset` that must pin to |0>: the wire's fresh post-reset
   * variable. Applied as `f <var> 0` alongside the boundary pins in collect_boundary_pins. */
  uint32_t *zero_pins;
  uint32_t zero_pins_len;
  uint32_t zero_pins_cap;

  uint64_t constant;
  uint32_t nvars;
  uint64_t norm_h;
  const char *input_bits;
  const char *output_bits;
  uint64_t modulus;
  bool approx_enabled;
  double approx_epsilon;
  double approx_delta;
  uint64_t approx_phase_count;
  bool have_openqasm;
  bool saw_gate;
  /* Set whenever exact mode refuses a gate because its angle needs a finer tick than the
   * current modulus provides (as opposed to an arity/syntax error). qasm2sop_main uses this to
   * decide whether a bigger-modulus retry is worth attempting; a false positive here just costs
   * one wasted re-parse. */
  bool angle_refusal;
  /* Build the former H-CZ-H/X-HZH graph as a comparison candidate. */
  bool permutation_lowering;
} qasm_importer_t;

static void set_error(qasm_importer_t *importer, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(importer->error, sizeof(importer->error), fmt, args);
  va_end(args);
}

static void print_usage(FILE *file) {
  static const char *const core[] = {
      "--input BITS",
      "--output BITS",
      "--approx EPS",
      "--no-optimize",
      "--version",
      "--help",
      "PATH|-",
  };
  static const dlx4sop_cli_usage_section_t sections[] = {
      {.title = "Options", .items = core, .nitems = sizeof(core) / sizeof(core[0])},
  };
  dlx4sop_cli_print_usage(file,
                          "usage: qasm2sop [--input BITS] [--output BITS] [--approx EPS] "
                          "[--no-optimize] [PATH|-]",
                          sections, sizeof(sections) / sizeof(sections[0]));
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

static void lowercase_ascii(char *text) {
  for (char *p = text; *p != '\0'; p++) {
    *p = (char)tolower((unsigned char)*p);
  }
}

/* Widened moduli (up to ~2^60 from choose_approx_modulus) times a QASM-source angle multiple
 * can overflow int64_t well before it overflows the modulus itself; do the reduction in 128
 * bits so the caller's product never needs its own overflow check. */
__extension__ typedef __int128 qasm_int128_t;

static uint64_t mod_i64(qasm_int128_t value, uint64_t modulus) {
  qasm_int128_t residue = value % (qasm_int128_t)modulus;
  if (residue < 0) {
    residue += (qasm_int128_t)modulus;
  }
  return (uint64_t)residue;
}

/* For FIXED structural angles that are always an exact multiple of pi/8 (H-sandwich twists,
 * sign flips, CX-sandwich building blocks) regardless of the target modulus -- these never
 * change with the modulus-dependent parsing below, since they aren't user-parsed values. */
static uint64_t coeff_from_pi_over_eight_units(const qasm_importer_t *importer, int64_t units) {
  const uint64_t scale = importer->modulus / 16U;
  return mod_i64((qasm_int128_t)units * (qasm_int128_t)scale, importer->modulus);
}

/* For USER-PARSED angles, already expressed directly as modulus ticks (angle = 2*pi*units/M) by
 * the dynamic-resolution parser below -- unlike coeff_from_pi_over_eight_units, no further
 * scaling is needed, just modular reduction. */
static uint64_t coeff_from_units(const qasm_importer_t *importer, int64_t units) {
  return mod_i64((qasm_int128_t)units, importer->modulus);
}

/* As coeff_from_units, but for the handful of call sites that must scale units by a small
 * constant first (e.g. -2x for an rzz edge coefficient); the multiply happens in 128-bit
 * space so a units value near INT64_MAX (as parse_pi_units's own overflow checks allow) can't
 * overflow before the modular reduction. */
static uint64_t coeff_from_units_scaled(const qasm_importer_t *importer, int64_t units,
                                        int64_t multiplier) {
  return mod_i64((qasm_int128_t)units * (qasm_int128_t)multiplier, importer->modulus);
}

static uint64_t sign_coeff(const qasm_importer_t *importer) {
  return importer->modulus / 2U;
}

static bool parse_numeric_pi_units(const char *expr, uint64_t unit_denominator,
                                   int64_t *out_units) {
  errno = 0;
  char *end = NULL;
  const double value = strtod(expr, &end);
  if (errno != 0 || end == expr || *trim(end) != '\0') {
    return false;
  }

  const long double units =
      (long double)value / ((long double)QASM_PI / (long double)unit_denominator);
  const int64_t rounded = units >= 0.0L ? (int64_t)(units + 0.5L) : (int64_t)(units - 0.5L);
  long double diff = units - (long double)rounded;
  if (diff < 0.0L) {
    diff = -diff;
  }
  /* Absolute tolerance is the historical (modulus-16) bound, dominant for small unit counts.
   * The relative term additionally covers large unit counts (e.g. a QFT chain's accumulated
   * pi/2^k tick multiplier at a wide modulus), where a decimal literal's own double-precision
   * round-trip error grows with |units| faster than a fixed absolute epsilon -- decimal-printed
   * dyadics round-trip at <= ~4*2^-53 relative error, so 2^-45 leaves ~250x headroom. A false
   * snap here perturbs the angle by at most pi/2^31, well below the 1e-8 amplitude epsilon
   * --approx would certify anyway. */
  const long double relative_bound =
      (units < 0.0L ? -units : units) * 0x1p-45L;
  if (diff > 1e-9L && diff > relative_bound) {
    return false;
  }

  *out_units = rounded;
  return true;
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
    snprintf(error, error_size,
             "%s bitstring length %zu does not match %" PRIu32 " OpenQASM qubits", option, len,
             nqubits);
    return false;
  }
  return true;
}

static uint32_t boundary_bit(const char *bits, uint32_t index) {
  return bits == NULL ? 0U : (uint32_t)(bits[index] - '0');
}

static bool affine_reserve(qasm_importer_t *importer, qasm_affine_t *affine, uint32_t needed) {
  if (needed <= affine->cap) {
    return true;
  }
  uint32_t new_cap = affine->cap == 0U ? 4U : affine->cap;
  while (new_cap < needed) {
    if (new_cap > UINT32_MAX / 2U) {
      set_error(importer, "affine wire expression is too large");
      return false;
    }
    new_cap *= 2U;
  }
  uint32_t *vars = realloc(affine->vars, (size_t)new_cap * sizeof(*vars));
  if (vars == NULL) {
    set_error(importer, "out of memory while storing affine wire expression");
    return false;
  }
  affine->vars = vars;
  affine->cap = new_cap;
  return true;
}

static bool affine_set_singleton(qasm_importer_t *importer, qasm_affine_t *affine,
                                 uint32_t var) {
  if (!affine_reserve(importer, affine, 1U)) {
    return false;
  }
  affine->vars[0] = var;
  affine->len = 1U;
  affine->constant = false;
  affine->has_observed_value = false;
  return true;
}

/* target ^= source, preserving sorted unique supports by symmetric difference. */
static bool affine_xor(qasm_importer_t *importer, qasm_affine_t *target,
                       const qasm_affine_t *source) {
  if (UINT32_MAX - target->len < source->len) {
    set_error(importer, "affine wire expression is too large");
    return false;
  }
  const uint32_t cap = target->len + source->len;
  uint32_t *merged = cap == 0U ? NULL : malloc((size_t)cap * sizeof(*merged));
  if (cap != 0U && merged == NULL) {
    set_error(importer, "out of memory while combining affine wire expressions");
    return false;
  }

  uint32_t i = 0;
  uint32_t j = 0;
  uint32_t len = 0;
  while (i < target->len || j < source->len) {
    if (j == source->len || (i < target->len && target->vars[i] < source->vars[j])) {
      merged[len++] = target->vars[i++];
    } else if (i == target->len || source->vars[j] < target->vars[i]) {
      merged[len++] = source->vars[j++];
    } else {
      i++;
      j++;
    }
  }

  free(target->vars);
  target->vars = merged;
  target->len = len;
  target->cap = cap;
  target->constant ^= source->constant;
  target->has_observed_value = false;
  return true;
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
  const uint32_t old_cap = importer->qubits_cap;
  qasm_affine_t *current = realloc(importer->current, (size_t)new_cap * sizeof(*current));
  if (current == NULL) {
    set_error(importer, "out of memory while storing qubits");
    return false;
  }
  memset(current + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*current));
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

static bool reserve_zero_pins(qasm_importer_t *importer, uint32_t needed) {
  if (needed <= importer->zero_pins_cap) {
    return true;
  }
  uint32_t new_cap = importer->zero_pins_cap == 0 ? 16U : importer->zero_pins_cap;
  while (new_cap < needed) {
    if (new_cap > UINT32_MAX / 2U) {
      set_error(importer, "too many reset pins");
      return false;
    }
    new_cap *= 2U;
  }
  uint32_t *zero_pins = realloc(importer->zero_pins, (size_t)new_cap * sizeof(*zero_pins));
  if (zero_pins == NULL) {
    set_error(importer, "out of memory while storing reset pins");
    return false;
  }
  importer->zero_pins = zero_pins;
  importer->zero_pins_cap = new_cap;
  return true;
}

static bool record_zero_pin(qasm_importer_t *importer, uint32_t var) {
  if (!reserve_zero_pins(importer, importer->zero_pins_len + 1U)) {
    return false;
  }
  importer->zero_pins[importer->zero_pins_len++] = var;
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
    if (!affine_set_singleton(importer, &importer->current[offset + i], importer->nvars++)) {
      return false;
    }
  }
  importer->nqubits += size;
  return true;
}

static bool add_unary(qasm_importer_t *importer, uint32_t v, uint64_t q) {
  if (!reserve_unary(importer, importer->unary_len + 1U)) {
    return false;
  }
  importer->unary[importer->unary_len++] = (qasm_unary_t){.v = v, .q = q};
  return true;
}

static bool add_edge(qasm_importer_t *importer, uint32_t u, uint32_t v, uint64_t q) {
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
    set_error(importer, "qreg index %" PRIu32 " is outside '%s[%" PRIu32 "]'", index, reg->name,
              reg->size);
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

static uint64_t named_phase_coeff_for_gate(const qasm_importer_t *importer, const char *gate) {
  if (strcmp(gate, "t") == 0) {
    return coeff_from_pi_over_eight_units(importer, 2);
  }
  if (strcmp(gate, "s") == 0) {
    return coeff_from_pi_over_eight_units(importer, 4);
  }
  if (strcmp(gate, "z") == 0) {
    return coeff_from_pi_over_eight_units(importer, 8);
  }
  if (strcmp(gate, "sdg") == 0) {
    return coeff_from_pi_over_eight_units(importer, 12);
  }
  if (strcmp(gate, "tdg") == 0) {
    return coeff_from_pi_over_eight_units(importer, 14);
  }
  return UINT64_MAX;
}

static bool named_phase_angle_for_gate(const char *gate, double *out_angle) {
  if (strcmp(gate, "t") == 0) {
    *out_angle = QASM_PI / 4.0;
    return true;
  }
  if (strcmp(gate, "s") == 0) {
    *out_angle = QASM_PI / 2.0;
    return true;
  }
  if (strcmp(gate, "z") == 0) {
    *out_angle = QASM_PI;
    return true;
  }
  if (strcmp(gate, "sdg") == 0) {
    *out_angle = -QASM_PI / 2.0;
    return true;
  }
  if (strcmp(gate, "tdg") == 0) {
    *out_angle = -QASM_PI / 4.0;
    return true;
  }
  return false;
}

static bool parse_pi_units(const char *expr, uint64_t unit_denominator, int64_t *out_units) {
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
      return parse_numeric_pi_units(expr, unit_denominator, out_units);
    }
    p++;
  }

  if (strncmp(p, "pi", 2) != 0) {
    return parse_numeric_pi_units(expr, unit_denominator, out_units);
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

  if (denominator == 0U || unit_denominator % denominator != 0U) {
    return false;
  }

  const uint64_t scale = unit_denominator / denominator;
  if (multiplier > (uint64_t)INT64_MAX / scale) {
    return false;
  }
  *out_units = sign * (int64_t)(multiplier * scale);
  return true;
}

static bool parse_param_angle_units(qasm_importer_t *importer, const char *gate,
                                    const char *prefix, const char *name,
                                    int64_t *out_units, bool *out_matches) {
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
    importer->angle_refusal = true;
    set_error(importer, "unsupported %s phase angle '%s'", name, gate);
    return false;
  }
  memcpy(expr, gate + prefix_len, expr_len);
  expr[expr_len] = '\0';

  int64_t units = 0;
  if (!parse_pi_units(expr, sign_coeff(importer), &units)) {
    importer->angle_refusal = true;
    set_error(importer, "unsupported %s phase angle '%s'", name, gate);
    return false;
  }
  *out_units = units;
  return true;
}

static bool parse_param_phase_units(qasm_importer_t *importer, const char *gate, const char *prefix,
                                    const char *name, int64_t *out_units, bool *out_matches) {
  return parse_param_angle_units(importer, gate, prefix, name, out_units, out_matches);
}

static bool parse_param_phase_coeff(qasm_importer_t *importer, const char *gate, const char *prefix,
                                    const char *name, uint64_t *out_coeff, bool *out_matches) {
  int64_t units = 0;
  if (!parse_param_angle_units(importer, gate, prefix, name, &units, out_matches)) {
    return false;
  }
  if (!*out_matches) {
    return true;
  }
  *out_coeff = coeff_from_units(importer, units);
  return true;
}

static bool parse_param_unit_list(qasm_importer_t *importer, const char *gate, const char *prefix,
                                  const char *name, int64_t *out_units, size_t expected,
                                  bool *out_matches) {
  const size_t prefix_len = strlen(prefix);
  const size_t gate_len = strlen(gate);
  if (strncmp(gate, prefix, prefix_len) != 0) {
    *out_matches = false;
    return true;
  }
  *out_matches = true;

  if (gate_len <= prefix_len || gate[gate_len - 1U] != ')') {
    set_error(importer, "%s gate must be written with %" PRIu64 " angle parameters", name,
              (uint64_t)expected);
    return false;
  }

  const size_t params_len = gate_len - prefix_len - 1U;
  char params[192];
  if (params_len == 0 || params_len >= sizeof(params)) {
    set_error(importer, "unsupported %s angle list '%s'", name, gate);
    return false;
  }
  memcpy(params, gate + prefix_len, params_len);
  params[params_len] = '\0';

  char *cursor = params;
  for (size_t i = 0; i < expected; i++) {
    char *next = NULL;
    if (i + 1U < expected) {
      next = strchr(cursor, ',');
      if (next == NULL) {
        set_error(importer, "unsupported %s angle list '%s'", name, gate);
        return false;
      }
      *next = '\0';
      next++;
    } else if (strchr(cursor, ',') != NULL) {
      set_error(importer, "unsupported %s angle list '%s'", name, gate);
      return false;
    }

    char *expr = trim(cursor);
    if (!parse_pi_units(expr, sign_coeff(importer), &out_units[i])) {
      importer->angle_refusal = true;
      set_error(importer, "unsupported %s angle '%s'", name, gate);
      return false;
    }
    cursor = next;
  }

  return true;
}

static bool parse_double_prefix(const char *text, double *out_value, char **out_end) {
  errno = 0;
  char *end = NULL;
  const double value = strtod(text, &end);
  if (errno != 0 || end == text || !isfinite(value)) {
    return false;
  }
  *out_value = value;
  *out_end = end;
  return true;
}

/* Recursive-descent parser for angle expressions, supporting compound arithmetic such as
 * "-pi/2 + pi/2" or "pi/4 + pi/8" (not just a single "[mult*]pi[/div]" term). This exists mainly
 * as a safety net: macro-expanded gate calls (see scripts/build_external_qasm_manifest.py's
 * inline_simple_gates) already fold such expressions down to a single term or plain decimal
 * before qasm2sop ever sees them, but this keeps the importer honest if that folding is ever
 * bypassed or incomplete.
 *
 * expr   := term (('+' | '-') term)*
 * term   := factor (('*' | '/') factor)*
 * factor := number | 'pi' | '(' expr ')' | ('+' | '-') factor
 */
typedef struct {
  const char *p;
} angle_parser_t;

static void angle_skip_space(angle_parser_t *parser) {
  while (isspace((unsigned char)*parser->p)) {
    parser->p++;
  }
}

static char angle_peek(angle_parser_t *parser) {
  angle_skip_space(parser);
  return *parser->p;
}

static bool angle_parse_expr(angle_parser_t *parser, double *out_value);

static bool angle_parse_factor(angle_parser_t *parser, double *out_value) {
  const char ch = angle_peek(parser);
  if (ch == '+') {
    parser->p++;
    return angle_parse_factor(parser, out_value);
  }
  if (ch == '-') {
    parser->p++;
    double value = 0.0;
    if (!angle_parse_factor(parser, &value)) {
      return false;
    }
    *out_value = -value;
    return true;
  }
  if (ch == '(') {
    parser->p++;
    double value = 0.0;
    if (!angle_parse_expr(parser, &value)) {
      return false;
    }
    if (angle_peek(parser) != ')') {
      return false;
    }
    parser->p++;
    *out_value = value;
    return true;
  }
  if (strncmp(parser->p, "pi", 2) == 0 && !isalnum((unsigned char)parser->p[2]) &&
      parser->p[2] != '_') {
    parser->p += 2;
    *out_value = QASM_PI;
    return true;
  }
  if (isdigit((unsigned char)ch) || ch == '.') {
    char *end = NULL;
    if (!parse_double_prefix(parser->p, out_value, &end)) {
      return false;
    }
    parser->p = end;
    return true;
  }
  return false;
}

static bool angle_parse_term(angle_parser_t *parser, double *out_value) {
  double value = 0.0;
  if (!angle_parse_factor(parser, &value)) {
    return false;
  }
  for (;;) {
    const char op = angle_peek(parser);
    if (op == '*') {
      parser->p++;
      double rhs = 0.0;
      if (!angle_parse_factor(parser, &rhs)) {
        return false;
      }
      value *= rhs;
    } else if (op == '/') {
      parser->p++;
      double rhs = 0.0;
      if (!angle_parse_factor(parser, &rhs) || rhs == 0.0) {
        return false;
      }
      value /= rhs;
    } else {
      break;
    }
  }
  *out_value = value;
  return true;
}

static bool angle_parse_expr(angle_parser_t *parser, double *out_value) {
  double value = 0.0;
  if (!angle_parse_term(parser, &value)) {
    return false;
  }
  for (;;) {
    const char op = angle_peek(parser);
    if (op == '+') {
      parser->p++;
      double rhs = 0.0;
      if (!angle_parse_term(parser, &rhs)) {
        return false;
      }
      value += rhs;
    } else if (op == '-') {
      parser->p++;
      double rhs = 0.0;
      if (!angle_parse_term(parser, &rhs)) {
        return false;
      }
      value -= rhs;
    } else {
      break;
    }
  }
  *out_value = value;
  return true;
}

static bool parse_angle_radians(const char *expr, double *out_angle) {
  angle_parser_t parser = {expr};
  double value = 0.0;
  if (!angle_parse_expr(&parser, &value)) {
    return false;
  }
  if (angle_peek(&parser) != '\0') {
    return false;
  }
  if (!isfinite(value)) {
    return false;
  }
  *out_angle = value;
  return true;
}

static bool parse_param_angle_radians(qasm_importer_t *importer, const char *gate,
                                      const char *prefix, const char *name,
                                      double *out_angle, bool *out_matches) {
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
  char expr[128];
  if (expr_len == 0 || expr_len >= sizeof(expr)) {
    set_error(importer, "unsupported %s phase angle '%s'", name, gate);
    return false;
  }
  memcpy(expr, gate + prefix_len, expr_len);
  expr[expr_len] = '\0';

  if (!parse_angle_radians(expr, out_angle)) {
    set_error(importer, "unsupported %s phase angle '%s'", name, gate);
    return false;
  }
  return true;
}

/* Finds the next comma at parenthesis depth zero, so a parenthesized sub-expression such as
 * "(-pi/2+pi/2)" is never mistaken for containing an argument separator. */
static char *find_top_level_comma(char *text) {
  int depth = 0;
  for (char *p = text; *p != '\0'; p++) {
    if (*p == '(') {
      depth++;
    } else if (*p == ')') {
      depth--;
    } else if (*p == ',' && depth == 0) {
      return p;
    }
  }
  return NULL;
}

static bool parse_param_radian_list(qasm_importer_t *importer, const char *gate,
                                    const char *prefix, const char *name, double *out_angles,
                                    size_t expected, bool *out_matches) {
  const size_t prefix_len = strlen(prefix);
  const size_t gate_len = strlen(gate);
  if (strncmp(gate, prefix, prefix_len) != 0) {
    *out_matches = false;
    return true;
  }
  *out_matches = true;

  if (gate_len <= prefix_len || gate[gate_len - 1U] != ')') {
    set_error(importer, "%s gate must be written with %" PRIu64 " angle parameters", name,
              (uint64_t)expected);
    return false;
  }

  const size_t params_len = gate_len - prefix_len - 1U;
  char params[256];
  if (params_len == 0 || params_len >= sizeof(params)) {
    set_error(importer, "unsupported %s angle list '%s'", name, gate);
    return false;
  }
  memcpy(params, gate + prefix_len, params_len);
  params[params_len] = '\0';

  char *cursor = params;
  for (size_t i = 0; i < expected; i++) {
    char *next = NULL;
    if (i + 1U < expected) {
      next = find_top_level_comma(cursor);
      if (next == NULL) {
        set_error(importer, "unsupported %s angle list '%s'", name, gate);
        return false;
      }
      *next = '\0';
      next++;
    } else if (find_top_level_comma(cursor) != NULL) {
      set_error(importer, "unsupported %s angle list '%s'", name, gate);
      return false;
    }

    char *expr = trim(cursor);
    if (!parse_angle_radians(expr, &out_angles[i])) {
      set_error(importer, "unsupported %s angle '%s'", name, gate);
      return false;
    }
    cursor = next;
  }

  return true;
}

static bool parse_u1_phase_coeff(qasm_importer_t *importer, const char *gate, uint64_t *out_coeff,
                                 bool *out_is_phase) {
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
  if (matches) {
    *out_is_phase = true;
    return true;
  }
  if (!parse_param_phase_coeff(importer, gate, "phase(", "phase", out_coeff, &matches)) {
    return false;
  }
  *out_is_phase = matches;
  return true;
}

static bool phase_coeff_for_gate(qasm_importer_t *importer, const char *gate, uint64_t *out_coeff,
                                 bool *out_is_phase) {
  const uint64_t named_coeff = named_phase_coeff_for_gate(importer, gate);
  if (named_coeff != UINT64_MAX) {
    *out_coeff = named_coeff;
    *out_is_phase = true;
    return true;
  }
  return parse_u1_phase_coeff(importer, gate, out_coeff, out_is_phase);
}

static bool controlled_phase_coeff_for_gate(qasm_importer_t *importer, const char *gate,
                                            uint64_t *out_coeff, bool *out_is_controlled_phase) {
  const uint64_t named_coeff = named_phase_coeff_for_gate(importer, gate + 1);
  if (gate[0] == 'c' && named_coeff != UINT64_MAX) {
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
  if (matches) {
    *out_is_controlled_phase = true;
    return true;
  }
  if (!parse_param_phase_coeff(importer, gate, "cphase(", "cphase", out_coeff, &matches)) {
    return false;
  }
  *out_is_controlled_phase = matches;
  return true;
}

static bool phase_angle_for_gate(qasm_importer_t *importer, const char *gate, double *out_angle,
                                 bool *out_is_phase) {
  bool matches = false;
  if (!parse_param_angle_radians(importer, gate, "u1(", "u1", out_angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_is_phase = true;
    return true;
  }
  if (!parse_param_angle_radians(importer, gate, "p(", "p", out_angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_is_phase = true;
    return true;
  }
  if (!parse_param_angle_radians(importer, gate, "phase(", "phase", out_angle, &matches)) {
    return false;
  }
  *out_is_phase = matches;
  return true;
}

static bool controlled_phase_angle_for_gate(qasm_importer_t *importer, const char *gate,
                                            double *out_angle, bool *out_is_controlled_phase) {
  bool matches = false;
  if (!parse_param_angle_radians(importer, gate, "cu1(", "cu1", out_angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_is_controlled_phase = true;
    return true;
  }
  if (!parse_param_angle_radians(importer, gate, "cp(", "cp", out_angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_is_controlled_phase = true;
    return true;
  }
  if (!parse_param_angle_radians(importer, gate, "cphase(", "cphase", out_angle, &matches)) {
    return false;
  }
  *out_is_controlled_phase = matches;
  return true;
}

static bool rz_units_for_gate(qasm_importer_t *importer, const char *gate, const char *prefix,
                              const char *name, int64_t *out_units, bool *out_matches) {
  return parse_param_phase_units(importer, gate, prefix, name, out_units, out_matches);
}

static bool add_constant(qasm_importer_t *importer, uint64_t coeff) {
  importer->constant = (importer->constant + coeff) % importer->modulus;
  return true;
}

/* Materialize y = constant XOR vars as a sign-only equality gadget:
 *
 *   delta(y = p) = 1/2 Sum_z (-1)^(z(y + p)).
 *
 * The factor 1/2 is represented by two normalization half-powers. This is needed only when a
 * non-Clifford phase observes a multi-variable parity; CNOT and Hadamard themselves stay affine. */
static bool affine_materialize(qasm_importer_t *importer, qasm_affine_t *affine,
                               uint32_t *out_value) {
  if (affine->has_observed_value) {
    *out_value = affine->observed_value;
    return true;
  }
  if (affine->len <= 1U) {
    set_error(importer, "internal error: tried to materialize a scalar affine wire");
    return false;
  }
  if (importer->nvars > UINT32_MAX - 2U || importer->norm_h > UINT64_MAX - 2U) {
    set_error(importer, "too many variables while materializing affine wire expression");
    return false;
  }

  const uint32_t value = importer->nvars++;
  const uint32_t check = importer->nvars++;
  const uint64_t sign = sign_coeff(importer);
  if (!add_edge(importer, check, value, sign)) {
    return false;
  }
  for (uint32_t i = 0; i < affine->len; i++) {
    if (!add_edge(importer, check, affine->vars[i], sign)) {
      return false;
    }
  }
  if (affine->constant && !add_unary(importer, check, sign)) {
    return false;
  }
  importer->norm_h += 2U;
  affine->has_observed_value = true;
  affine->observed_value = value;
  *out_value = value;
  return true;
}

/* For q a multiple of r/4, q*(x0 XOR ... XOR xn) has no degree >= 3 terms modulo r:
 * q*parity = q*Sum(xi) - 2q*Sum(xi*xj). This keeps Clifford parity phases in qsop-sign without
 * introducing an equality gadget. */
static bool apply_clifford_phase_affine(qasm_importer_t *importer, const qasm_affine_t *affine,
                                        uint64_t coeff) {
  uint64_t effective = coeff % importer->modulus;
  if (affine->constant) {
    if (!add_constant(importer, effective)) {
      return false;
    }
    effective = effective == 0U ? 0U : importer->modulus - effective;
  }
  for (uint32_t i = 0; i < affine->len; i++) {
    if (effective != 0U && !add_unary(importer, affine->vars[i], effective)) {
      return false;
    }
  }
  const uint64_t pair_coeff =
      mod_i64((qasm_int128_t)-2 * (qasm_int128_t)effective, importer->modulus);
  if (pair_coeff == 0U) {
    return true;
  }
  if (pair_coeff != sign_coeff(importer)) {
    set_error(importer, "internal error: Clifford parity phase produced a non-sign edge");
    return false;
  }
  for (uint32_t i = 0; i < affine->len; i++) {
    for (uint32_t j = i + 1U; j < affine->len; j++) {
      if (!add_edge(importer, affine->vars[i], affine->vars[j], pair_coeff)) {
        return false;
      }
    }
  }
  return true;
}

static bool apply_phase(qasm_importer_t *importer, uint32_t qubit, uint64_t coeff) {
  coeff %= importer->modulus;
  if (coeff == 0U) {
    return true;
  }
  qasm_affine_t *affine = &importer->current[qubit];
  if (affine->len == 0U) {
    return !affine->constant || add_constant(importer, coeff);
  }
  if (importer->modulus % 4U == 0U && coeff % (importer->modulus / 4U) == 0U) {
    return apply_clifford_phase_affine(importer, affine, coeff);
  }
  if (affine->len == 1U) {
    if (!affine->constant) {
      return add_unary(importer, affine->vars[0], coeff);
    }
    return add_constant(importer, coeff) &&
           add_unary(importer, affine->vars[0], importer->modulus - coeff);
  }
  uint32_t value = 0;
  return affine_materialize(importer, affine, &value) && add_unary(importer, value, coeff);
}

static bool apply_h(qasm_importer_t *importer, uint32_t qubit) {
  if (importer->nvars == UINT32_MAX || importer->norm_h == UINT64_MAX) {
    set_error(importer, "too many Hadamard gates");
    return false;
  }
  const uint32_t next_var = importer->nvars++;
  qasm_affine_t *affine = &importer->current[qubit];
  const uint64_t sign = sign_coeff(importer);
  for (uint32_t i = 0; i < affine->len; i++) {
    if (!add_edge(importer, affine->vars[i], next_var, sign)) {
      return false;
    }
  }
  if (affine->constant && !add_unary(importer, next_var, sign)) {
    return false;
  }
  importer->norm_h++;
  return affine_set_singleton(importer, affine, next_var);
}

static bool apply_cz(qasm_importer_t *importer, uint32_t left, uint32_t right) {
  const qasm_affine_t *a = &importer->current[left];
  const qasm_affine_t *b = &importer->current[right];
  const uint64_t sign = sign_coeff(importer);
  if (a->constant && b->constant && !add_constant(importer, sign)) {
    return false;
  }
  if (b->constant) {
    for (uint32_t i = 0; i < a->len; i++) {
      if (!add_unary(importer, a->vars[i], sign)) {
        return false;
      }
    }
  }
  if (a->constant) {
    for (uint32_t j = 0; j < b->len; j++) {
      if (!add_unary(importer, b->vars[j], sign)) {
        return false;
      }
    }
  }
  for (uint32_t i = 0; i < a->len; i++) {
    for (uint32_t j = 0; j < b->len; j++) {
      if (a->vars[i] == b->vars[j]) {
        if (!add_unary(importer, a->vars[i], sign)) {
          return false;
        }
      } else if (!add_edge(importer, a->vars[i], b->vars[j], sign)) {
        return false;
      }
    }
  }
  return true;
}

/* apply_cx_decomposition is defined further below (it needs apply_h/apply_cz, defined in
 * between) -- forward-declared here so the two functions below can lower through it instead
 * of encoding a direct weighted edge. */
static bool apply_cx_decomposition(qasm_importer_t *importer, uint32_t control, uint32_t target);

/* phase(coeff) conditioned on left XOR right (applied iff exactly one of the two qubits is 1):
 * cx(left,right) computes right ^= left, a plain phase on right then applies iff that XOR was
 * 1, and the second cx uncomputes it back to right's original value. Sign-only (2 cx + 1
 * single-qubit phase), no weighted edge and no halving needed at all -- this is what
 * apply_ccz_decomposition (ccx/ccz/cswap) and apply_controlled_phase use, and was previously
 * the actual source of the "non-sign quadratic" export failures on circuits using Toffoli
 * gates. */
static bool apply_phase_on_xor2(qasm_importer_t *importer, uint32_t left, uint32_t right,
                                uint64_t coeff) {
  if (coeff % importer->modulus == 0) {
    return true;
  }
  return apply_cx_decomposition(importer, left, right) &&
         apply_phase(importer, right, coeff) &&
         apply_cx_decomposition(importer, left, right);
}

/* CP(theta) = diag(1,1,1,e^{i*theta}): p(theta/2) c; p(theta/2) t; cx c,t; p(-theta/2) t; cx c,t
 * gives exactly this matrix for ANY theta (verified by tracing all 4 basis states) -- every
 * 2-qubit interaction is a cx (sign-only via apply_cx_decomposition), theta only ever lands on
 * single-qubit phases. `coeff` here is theta's own tick value (not theta/2's), so this needs
 * coeff to be even to halve exactly; refuse cleanly otherwise (matches how the rest of exact
 * mode already narrows) rather than falling back to a non-sign edge. Every current caller
 * (crz, rzz, csx) already passes an even coeff by construction; only a literal cp(theta)/cu1
 * (theta) with an odd pi/8-unit count could hit the refusal, and --approx always works there.
 *
 * coeff == sign_coeff (exactly pi) is already a pure sign edge with no decomposition needed --
 * short-circuit to the direct edge apply_cz itself uses. This also covers "cz" specifically:
 * controlled_phase_coeff_for_gate treats it as the named phase gate "c"+"z" (named_phase_coeff_
 * for_gate("z") happens to equal sign_coeff exactly), routing it through this function rather
 * than the dedicated QASM_TWO_CZ path -- harmless before this rewrite since the old
 * implementation was already just this same direct edge, but would otherwise now explode a
 * plain cz into an unnecessary cx-sandwich. */
static bool apply_controlled_phase(qasm_importer_t *importer, uint32_t left, uint32_t right,
                                   uint64_t coeff) {
  if (coeff % importer->modulus == 0) {
    return true;
  }
  if (coeff == sign_coeff(importer)) {
    return apply_cz(importer, left, right);
  }
  if (coeff % 2 != 0) {
    importer->angle_refusal = true;
    set_error(importer,
              "unsupported cp/cu1 angle in exact mode (must be an even multiple of the "
              "finest representable tick); use --approx");
    return false;
  }
  const uint64_t half_coeff = coeff / 2;
  return apply_phase(importer, left, half_coeff) && apply_phase(importer, right, half_coeff) &&
         apply_phase_on_xor2(
             importer, left, right,
             mod_i64((qasm_int128_t)-1 * (qasm_int128_t)half_coeff, importer->modulus));
}

/* units is now the gate's full angle expressed directly in modulus ticks (angle =
 * 2*pi*units/modulus), produced by the dynamic-resolution parser in parse_param_angle_units.
 * RZ(theta)'s global phase term -theta/2 needs units/2 to land on an exact tick -- refuse
 * cleanly when it doesn't (one more halving than the current modulus provides); the retry at
 * QASM_EXACT_MODULUS_MAX resolves this for any angle that's an honest dyadic fraction of pi. */
static bool apply_rz(qasm_importer_t *importer, uint32_t qubit, int64_t units) {
  if (units % 2 != 0) {
    importer->angle_refusal = true;
    set_error(importer,
              "unsupported rz/rx/ry angle in exact mode (needs one more halving than the "
              "current modulus provides); use --approx");
    return false;
  }
  return add_constant(importer, coeff_from_units(importer, -(units / 2))) &&
         apply_phase(importer, qubit, coeff_from_units(importer, units));
}

static bool apply_crz(qasm_importer_t *importer, uint32_t control, uint32_t target, int64_t units) {
  if (units % 2 != 0) {
    importer->angle_refusal = true;
    set_error(importer,
              "unsupported crz/cry angle in exact mode (needs one more halving than the "
              "current modulus provides); use --approx");
    return false;
  }
  return apply_phase(importer, control, coeff_from_units(importer, -(units / 2))) &&
         apply_controlled_phase(importer, control, target, coeff_from_units(importer, units));
}

/* CRY(theta) = (I (x) P(pi/2).H) . CRZ(theta) . (I (x) H.P(-pi/2)): the same H/phase sandwich
 * apply_ry already applies to apply_rz (coefficients 12 and 4 in pi/8 units), conjugating
 * apply_crz instead and applied only to the target wire -- control is untouched. */
static bool apply_cry(qasm_importer_t *importer, uint32_t control, uint32_t target,
                      int64_t units) {
  return apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, target) && apply_crz(importer, control, target, units) &&
         apply_h(importer, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 4));
}

/* units is the full RZZ(theta) angle in modulus ticks (see apply_rz); RZZ's global -theta/2 term
 * needs the same one-tick-of-headroom as RZ's. */
static bool apply_rzz(qasm_importer_t *importer, uint32_t left, uint32_t right, int64_t units) {
  if (units % 2 != 0) {
    importer->angle_refusal = true;
    set_error(importer,
              "unsupported rzz/rxx/ryy angle in exact mode (needs one more halving than the "
              "current modulus provides); use --approx");
    return false;
  }
  return add_constant(importer, coeff_from_units(importer, -(units / 2))) &&
         apply_phase(importer, left, coeff_from_units(importer, units)) &&
         apply_phase(importer, right, coeff_from_units(importer, units)) &&
         apply_controlled_phase(importer, left, right,
                                coeff_from_units_scaled(importer, units, -2));
}

static bool apply_rxx(qasm_importer_t *importer, uint32_t left, uint32_t right, int64_t units) {
  return apply_h(importer, left) && apply_h(importer, right) &&
         apply_rzz(importer, left, right, units) && apply_h(importer, left) &&
         apply_h(importer, right);
}

static bool apply_ryy(qasm_importer_t *importer, uint32_t left, uint32_t right, int64_t units) {
  return apply_phase(importer, left, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, left) &&
         apply_phase(importer, right, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, right) &&
         apply_rzz(importer, left, right, units) && apply_h(importer, left) &&
         apply_phase(importer, left, coeff_from_pi_over_eight_units(importer, 4)) &&
         apply_h(importer, right) &&
         apply_phase(importer, right, coeff_from_pi_over_eight_units(importer, 4));
}

static bool apply_x_decomposition(qasm_importer_t *importer, uint32_t qubit) {
  if (importer->permutation_lowering) {
    return apply_h(importer, qubit) &&
           apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 8)) &&
           apply_h(importer, qubit);
  }
  importer->current[qubit].constant = !importer->current[qubit].constant;
  importer->current[qubit].has_observed_value = false;
  return true;
}

static bool apply_y_decomposition(qasm_importer_t *importer, uint32_t qubit) {
  return apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_x_decomposition(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 4));
}

static bool apply_sx_decomposition(qasm_importer_t *importer, uint32_t qubit) {
  return apply_h(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 4)) &&
         apply_h(importer, qubit);
}

static bool apply_sxdg_decomposition(qasm_importer_t *importer, uint32_t qubit) {
  return apply_h(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, qubit);
}

static bool apply_rx(qasm_importer_t *importer, uint32_t qubit, int64_t units) {
  return apply_h(importer, qubit) && apply_rz(importer, qubit, units) && apply_h(importer, qubit);
}

static bool apply_ry(qasm_importer_t *importer, uint32_t qubit, int64_t units) {
  return apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, qubit) &&
         apply_rz(importer, qubit, units) && apply_h(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 4));
}

/* theta/phi/lambda_units are the gate's full angles in modulus ticks (see apply_rz). Unlike the
 * old fixed pi/4-unit convention, lambda and phi land as bare qubit-conditional phases here (no
 * per-term global-phase side effect to correct away): only theta goes through apply_ry (hence
 * apply_rz), which is where U3's own net global phase of -theta/2 comes from -- verified against
 * the old formula (which routed lambda/phi through apply_rz too and then cancelled their global
 * contributions with an explicit +(phi+lambda)/2 correction) to produce an identical qsop output
 * with one fewer refusal case, since lambda/phi no longer need their own halving. */
static bool apply_u3(qasm_importer_t *importer, uint32_t qubit, int64_t theta_units,
                     int64_t phi_units, int64_t lambda_units) {
  return apply_phase(importer, qubit, coeff_from_units(importer, lambda_units)) &&
         apply_ry(importer, qubit, theta_units) &&
         apply_phase(importer, qubit, coeff_from_units(importer, phi_units));
}

static bool apply_cx_decomposition(qasm_importer_t *importer, uint32_t control, uint32_t target) {
  if (importer->permutation_lowering) {
    return apply_h(importer, target) && apply_cz(importer, control, target) &&
           apply_h(importer, target);
  }
  return affine_xor(importer, &importer->current[target], &importer->current[control]);
}

/* CU(theta,phi,lambda,gamma): the standard 2-CNOT Barenco/ABC decomposition (same one
 * OpenQASM 3's stdgates.inc and Qiskit use). Every 2-qubit interaction is one of the two `cx`
 * gates below (sign-only via apply_cx_decomposition); theta/phi/lambda/gamma only ever land on
 * single-qubit phase terms, which have no sign-only restriction -- so this needs no format
 * change to support arbitrary angles via --approx.
 *
 * theta/phi/lambda/gamma_units are the gate's full angles in modulus ticks (see apply_rz). The
 * two u(...) sub-calls below need theta/2 and (phi+lambda)/2 as apply_u3 parameters, exact only
 * when theta_units and (phi_units+lambda_units) are each even -- refuse cleanly otherwise
 * (matching how the rest of exact mode already narrows) rather than silently rounding; lambda
 * and phi always have the same parity (they differ by 2*phi), so checking the sum covers both
 * of the p(...) terms below that use their difference. */
static bool apply_cu(qasm_importer_t *importer, uint32_t control, uint32_t target,
                     int64_t theta_units, int64_t phi_units, int64_t lambda_units,
                     int64_t gamma_units) {
  if (theta_units % 2 != 0 || (phi_units + lambda_units) % 2 != 0) {
    importer->angle_refusal = true;
    set_error(importer,
              "unsupported cu angle combination in exact mode (theta and phi+lambda need one "
              "more halving than the current modulus provides); use --approx");
    return false;
  }
  const int64_t half_theta = theta_units / 2;
  const int64_t half_phi_plus_lambda = (phi_units + lambda_units) / 2;
  const int64_t half_lambda_minus_phi = (lambda_units - phi_units) / 2;
  return apply_phase(importer, control, coeff_from_units(importer, gamma_units)) &&
         apply_phase(importer, control, coeff_from_units(importer, half_phi_plus_lambda)) &&
         apply_phase(importer, target, coeff_from_units(importer, half_lambda_minus_phi)) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_u3(importer, target, -half_theta, 0, -half_phi_plus_lambda) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_u3(importer, target, half_theta, phi_units, 0);
}

static uint64_t rounded_coeff_for_angle(qasm_importer_t *importer, double angle) {
  /* Scale only after reducing to the unit circle. Multiplying an AE/QPE-scale angle (which can
   * be around 2^100 after an unrolled power) by a roughly 2^59 approximation modulus first loses
   * every fractional tick even in long double. libm's sin/cos perform the required wide argument
   * reduction; atan2 then gives a small principal angle that can be scaled without cancellation. */
  const double actual_re = cos(angle);
  const double actual_im = sin(angle);
  const long double principal = atan2l((long double)actual_im, (long double)actual_re);
  const long double scaled =
      principal * (long double)importer->modulus / QASM_TWO_PI_L;
  const long double nearest = nearbyintl(scaled);
  long double residue = fmodl(nearest, (long double)importer->modulus);
  if (residue < 0.0L) {
    residue += (long double)importer->modulus;
  }

  const long double rounded_angle = nearest * QASM_TWO_PI_L / (long double)importer->modulus;
  const long double rounded_re = cosl(rounded_angle);
  const long double rounded_im = sinl(rounded_angle);
  long double chord = hypotl((long double)actual_re - rounded_re,
                             (long double)actual_im - rounded_im);
  /* Account conservatively for the stored double unit-circle value and the long-double root.
   * This is tiny compared with the requested budgets but prevents the certificate from claiming
   * literal zero error merely because two rounded transcendental results happened to coincide. */
  chord += 8.0L * (long double)DBL_EPSILON + 16.0L * LDBL_EPSILON;
  if (chord > 2.0L) {
    chord = 2.0L;
  }
  importer->approx_delta += (double)chord;
  importer->approx_phase_count++;

  return (uint64_t)residue;
}

static bool add_constant_angle(qasm_importer_t *importer, double angle) {
  return add_constant(importer, rounded_coeff_for_angle(importer, angle));
}

static bool apply_phase_angle(qasm_importer_t *importer, uint32_t qubit, double angle) {
  return apply_phase(importer, qubit, rounded_coeff_for_angle(importer, angle));
}

static bool apply_rz_angle(qasm_importer_t *importer, uint32_t qubit, double angle) {
  return add_constant_angle(importer, -0.5 * angle) && apply_phase_angle(importer, qubit, angle);
}

static bool apply_rx_angle(qasm_importer_t *importer, uint32_t qubit, double angle) {
  return apply_h(importer, qubit) && apply_rz_angle(importer, qubit, angle) &&
         apply_h(importer, qubit);
}

static bool apply_ry_angle(qasm_importer_t *importer, uint32_t qubit, double angle) {
  return apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, qubit) && apply_rz_angle(importer, qubit, angle) &&
         apply_h(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_pi_over_eight_units(importer, 4));
}

static bool apply_controlled_phase_angle(qasm_importer_t *importer, uint32_t control,
                                         uint32_t target, double angle) {
  return apply_phase_angle(importer, control, 0.5 * angle) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_phase_angle(importer, target, -0.5 * angle) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_phase_angle(importer, target, 0.5 * angle);
}

static bool apply_csx_angle_decomposition(qasm_importer_t *importer, uint32_t control,
                                          uint32_t target, double angle) {
  return apply_h(importer, target) &&
         apply_controlled_phase_angle(importer, control, target, angle) &&
         apply_h(importer, target);
}

static bool apply_crz_angle(qasm_importer_t *importer, uint32_t control, uint32_t target,
                            double angle) {
  return apply_phase_angle(importer, control, -0.5 * angle) &&
         apply_controlled_phase_angle(importer, control, target, angle);
}

/* Same H/phase sandwich as apply_ry_angle, conjugating apply_crz_angle instead, target only. */
static bool apply_cry_angle(qasm_importer_t *importer, uint32_t control, uint32_t target,
                            double angle) {
  return apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, target) && apply_crz_angle(importer, control, target, angle) &&
         apply_h(importer, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 4));
}

static bool apply_rzz_angle(qasm_importer_t *importer, uint32_t left, uint32_t right,
                            double angle) {
  return apply_cx_decomposition(importer, left, right) && apply_rz_angle(importer, right, angle) &&
         apply_cx_decomposition(importer, left, right);
}

static bool apply_rxx_angle(qasm_importer_t *importer, uint32_t left, uint32_t right,
                            double angle) {
  return apply_h(importer, left) && apply_h(importer, right) &&
         apply_rzz_angle(importer, left, right, angle) && apply_h(importer, left) &&
         apply_h(importer, right);
}

static bool apply_ryy_angle(qasm_importer_t *importer, uint32_t left, uint32_t right,
                            double angle) {
  return apply_phase(importer, left, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, left) &&
         apply_phase(importer, right, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_h(importer, right) &&
         apply_rzz_angle(importer, left, right, angle) && apply_h(importer, left) &&
         apply_phase(importer, left, coeff_from_pi_over_eight_units(importer, 4)) &&
         apply_h(importer, right) &&
         apply_phase(importer, right, coeff_from_pi_over_eight_units(importer, 4));
}

static bool apply_u3_angle(qasm_importer_t *importer, uint32_t qubit, double theta, double phi,
                           double lambda) {
  return add_constant_angle(importer, 0.5 * (phi + lambda)) &&
         apply_rz_angle(importer, qubit, lambda) && apply_ry_angle(importer, qubit, theta) &&
         apply_rz_angle(importer, qubit, phi);
}

static bool apply_u2_angle(qasm_importer_t *importer, uint32_t qubit, double phi, double lambda) {
  return apply_u3_angle(importer, qubit, QASM_PI / 2.0, phi, lambda);
}

/* Continuous-angle counterpart of apply_cu: no exactness concerns at all, since
 * rounded_coeff_for_angle (via apply_phase_angle/apply_u3_angle) already handles arbitrary
 * real angles. This is the path real cu(...) gates actually take (theta continuous, phi=
 * lambda=gamma=0 in every observed case, which makes this mathematically identical to
 * apply_cry_angle -- but implemented generally here, not special-cased). */
static bool apply_cu_angle(qasm_importer_t *importer, uint32_t control, uint32_t target,
                          double theta, double phi, double lambda, double gamma) {
  return apply_phase_angle(importer, control, gamma) &&
         apply_phase_angle(importer, control, 0.5 * (lambda + phi)) &&
         apply_phase_angle(importer, target, 0.5 * (lambda - phi)) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_u3_angle(importer, target, -0.5 * theta, 0.0, -0.5 * (phi + lambda)) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_u3_angle(importer, target, 0.5 * theta, phi, 0.0);
}

static bool apply_cy_decomposition(qasm_importer_t *importer, uint32_t control, uint32_t target) {
  return apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 12)) &&
         apply_cx_decomposition(importer, control, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 4));
}

static bool apply_csx_decomposition(qasm_importer_t *importer, uint32_t control,
                                    uint32_t target, uint64_t phase_coeff) {
  return apply_h(importer, target) &&
         apply_controlled_phase(importer, control, target, phase_coeff) &&
         apply_h(importer, target);
}

static bool apply_ccz_decomposition(qasm_importer_t *importer, uint32_t first, uint32_t second,
                                    uint32_t third) {
  return apply_phase(importer, first, coeff_from_pi_over_eight_units(importer, 2)) &&
         apply_phase(importer, second, coeff_from_pi_over_eight_units(importer, 2)) &&
         apply_phase(importer, third, coeff_from_pi_over_eight_units(importer, 2)) &&
         apply_phase_on_xor2(importer, first, second,
                             coeff_from_pi_over_eight_units(importer, 14)) &&
         apply_phase_on_xor2(importer, first, third,
                             coeff_from_pi_over_eight_units(importer, 14)) &&
         apply_phase_on_xor2(importer, second, third,
                             coeff_from_pi_over_eight_units(importer, 14)) &&
         apply_cx_decomposition(importer, first, third) &&
         apply_phase_on_xor2(importer, second, third,
                             coeff_from_pi_over_eight_units(importer, 2)) &&
         apply_cx_decomposition(importer, first, third);
}

static bool apply_ccx_decomposition(qasm_importer_t *importer, uint32_t first, uint32_t second,
                                    uint32_t target) {
  return apply_h(importer, target) && apply_ccz_decomposition(importer, first, second, target) &&
         apply_h(importer, target);
}

static bool apply_rccx_decomposition(qasm_importer_t *importer, uint32_t first, uint32_t second,
                                     uint32_t target) {
  return apply_h(importer, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 2)) &&
         apply_cx_decomposition(importer, second, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 14)) &&
         apply_cx_decomposition(importer, first, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 2)) &&
         apply_cx_decomposition(importer, second, target) &&
         apply_phase(importer, target, coeff_from_pi_over_eight_units(importer, 14)) &&
         apply_h(importer, target);
}

static bool apply_cswap_decomposition(qasm_importer_t *importer, uint32_t control, uint32_t left,
                                      uint32_t right) {
  return apply_cx_decomposition(importer, right, left) &&
         apply_ccx_decomposition(importer, control, left, right) &&
         apply_cx_decomposition(importer, right, left);
}

static bool apply_iswap_decomposition(qasm_importer_t *importer, uint32_t left, uint32_t right) {
  if (!apply_cz(importer, left, right)) {
    return false;
  }

  const qasm_affine_t tmp = importer->current[left];
  importer->current[left] = importer->current[right];
  importer->current[right] = tmp;
  return apply_phase(importer, left, coeff_from_pi_over_eight_units(importer, 4)) &&
         apply_phase(importer, right, coeff_from_pi_over_eight_units(importer, 4));
}

static bool split_two_operands(qasm_importer_t *importer, char *rest, char **left, char **right) {
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

static bool split_three_operands(qasm_importer_t *importer, char *rest, char **first,
                                 char **second, char **third) {
  char *first_comma = strchr(rest, ',');
  if (first_comma == NULL) {
    set_error(importer, "three-qubit gate operands must be separated by commas");
    return false;
  }
  *first_comma = '\0';

  char *second_comma = strchr(first_comma + 1, ',');
  if (second_comma == NULL) {
    set_error(importer, "three-qubit gate operands must be separated by commas");
    return false;
  }
  *second_comma = '\0';

  if (strchr(second_comma + 1, ',') != NULL) {
    set_error(importer, "three-qubit gate has too many operands");
    return false;
  }

  *first = rest;
  *second = first_comma + 1;
  *third = second_comma + 1;
  return true;
}

static bool parse_qubit_or_reg_operand(qasm_importer_t *importer, char *text, qasm_operand_t *out) {
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

/* `reset q`: the wire is discarded and re-prepared in |0>. In the single-amplitude SOP this is the
 * coherent object Sum_m |0><m| = |0>(<0| + <1|): the pre-reset variable is left unpinned (so the
 * evaluation sums it), and a fresh variable pinned to |0> carries the wire onward. The fresh-var
 * idiom mirrors apply_h / add_qreg.
 *
 * Precondition (shared with apply_measure): this coherent lowering equals a physical measure/reset
 * exactly when the qubit is in a definite computational-basis state at that point -- the regime of
 * uncomputed/recycled ancillas and stabilizer syndromes (the entire alg85 corpus; cross-checked
 * against qiskit-aer in tests/test_qasm_aer.py). It does not reproduce incoherent statistics for a
 * qubit left in superposition and used coherently afterward -- a regime the corpus excludes. */
static bool apply_reset(qasm_importer_t *importer, uint32_t qubit) {
  if (importer->nvars == UINT32_MAX) {
    set_error(importer, "too many variables");
    return false;
  }
  const uint32_t fresh = importer->nvars++;
  if (!record_zero_pin(importer, fresh)) {
    return false;
  }
  return affine_set_singleton(importer, &importer->current[qubit], fresh);
}

static bool apply_reset_operand(qasm_importer_t *importer, char *rest) {
  qasm_operand_t operand;
  if (!parse_qubit_or_reg_operand(importer, rest, &operand)) {
    return false;
  }
  if (!operand.is_reg) {
    return apply_reset(importer, operand.qubit);
  }
  for (uint32_t i = 0; i < operand.reg->size; i++) {
    if (!apply_reset(importer, operand.reg->offset + i)) {
      return false;
    }
  }
  return true;
}

/* `measure q -> c` with no classical feed-forward (an `if` reading c is still rejected): deferred to
 * the end of the circuit and dropped like the stripped final measurements, which coherently is
 * Sum_m |m><m| = I -- a no-op on the amplitude. The wire keeps its current variable. We still
 * validate the quantum operand so a malformed measure is rejected; the classical target is inert
 * (cregs are declared-and-ignored), so it is only checked to be present. */
static bool apply_measure(qasm_importer_t *importer, char *rest) {
  char *arrow = strstr(rest, "->");
  if (arrow == NULL) {
    set_error(importer, "measure must look like 'measure qreg -> creg'");
    return false;
  }
  *arrow = '\0';
  char *classical = trim(arrow + 2);
  if (*classical == '\0') {
    set_error(importer, "measure is missing its classical target");
    return false;
  }
  qasm_operand_t operand;
  if (!parse_qubit_or_reg_operand(importer, trim(rest), &operand)) {
    return false;
  }
  (void)operand;
  return true;
}

/* Matches qasm_param_two_qubit_fn (a single qubit, two int64 values) so a one-qubit fixed-enum
 * dispatch can broadcast over a qreg through the same generic apply_param_two_qubit_operand the
 * continuously-parameterized gates already use, instead of its own copy of that loop. */
static bool apply_one_qubit_op_values(qasm_importer_t *importer, uint32_t qubit, int64_t op,
                                      int64_t phase_coeff) {
  switch ((qasm_one_qubit_op_t)op) {
  case QASM_ONE_ID:
    return true;
  case QASM_ONE_PHASE:
    return apply_phase(importer, qubit, (uint64_t)phase_coeff);
  case QASM_ONE_H:
    return apply_h(importer, qubit);
  case QASM_ONE_X:
    return apply_x_decomposition(importer, qubit);
  case QASM_ONE_Y:
    return apply_y_decomposition(importer, qubit);
  case QASM_ONE_SX:
    return apply_sx_decomposition(importer, qubit);
  case QASM_ONE_SXDG:
    return apply_sxdg_decomposition(importer, qubit);
  }
  set_error(importer, "internal error: unknown one-qubit operation");
  return false;
}

typedef bool (*qasm_param_one_qubit_fn)(qasm_importer_t *importer, uint32_t qubit, int64_t units);

typedef bool (*qasm_param_two_qubit_fn)(qasm_importer_t *importer, uint32_t qubit, int64_t first,
                                        int64_t second);

typedef bool (*qasm_param_three_qubit_fn)(qasm_importer_t *importer, uint32_t qubit, int64_t first,
                                          int64_t second, int64_t third);

static bool apply_param_one_qubit_operand(qasm_importer_t *importer, char *rest, int64_t units,
                                          qasm_param_one_qubit_fn apply) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply(importer, qubit, units);
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
    if (!apply(importer, reg->offset + i, units)) {
      return false;
    }
  }
  return true;
}

static bool apply_param_two_qubit_operand(qasm_importer_t *importer, char *rest, int64_t first,
                                          int64_t second, qasm_param_two_qubit_fn apply) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply(importer, qubit, first, second);
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
    if (!apply(importer, reg->offset + i, first, second)) {
      return false;
    }
  }
  return true;
}

static bool apply_param_three_qubit_operand(qasm_importer_t *importer, char *rest, int64_t first,
                                            int64_t second, int64_t third,
                                            qasm_param_three_qubit_fn apply) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply(importer, qubit, first, second, third);
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
    if (!apply(importer, reg->offset + i, first, second, third)) {
      return false;
    }
  }
  return true;
}

typedef bool (*qasm_radian_one_qubit_fn)(qasm_importer_t *importer, uint32_t qubit,
                                         double angle);

typedef bool (*qasm_radian_two_param_one_qubit_fn)(qasm_importer_t *importer, uint32_t qubit,
                                                  double first, double second);

typedef bool (*qasm_radian_three_param_one_qubit_fn)(qasm_importer_t *importer, uint32_t qubit,
                                                    double first, double second, double third);

static bool apply_radian_one_qubit_operand(qasm_importer_t *importer, char *rest, double angle,
                                           qasm_radian_one_qubit_fn apply) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply(importer, qubit, angle);
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
    if (!apply(importer, reg->offset + i, angle)) {
      return false;
    }
  }
  return true;
}

static bool apply_radian_two_param_one_qubit_operand(
    qasm_importer_t *importer, char *rest, double first, double second,
    qasm_radian_two_param_one_qubit_fn apply) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply(importer, qubit, first, second);
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
    if (!apply(importer, reg->offset + i, first, second)) {
      return false;
    }
  }
  return true;
}

static bool apply_radian_three_param_one_qubit_operand(
    qasm_importer_t *importer, char *rest, double first, double second, double third,
    qasm_radian_three_param_one_qubit_fn apply) {
  rest = trim(rest);
  if (strchr(rest, '[') != NULL) {
    uint32_t qubit = 0;
    if (!parse_qref(importer, rest, &qubit)) {
      return false;
    }
    return apply(importer, qubit, first, second, third);
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
    if (!apply(importer, reg->offset + i, first, second, third)) {
      return false;
    }
  }
  return true;
}

typedef bool (*qasm_angle_two_qubit_fn)(qasm_importer_t *importer, uint32_t left,
                                        uint32_t right, int64_t units);

/* Matches qasm_two_value_two_qubit_fn (two int64 values) so the two-qubit fixed-enum dispatch can
 * broadcast over qreg operands through the same generic apply_two_value_two_qubit_operand the
 * continuously-parameterized two-qubit gates already use, instead of its own copy of that loop. */
static bool apply_two_qubit_op_values(qasm_importer_t *importer, uint32_t left, uint32_t right,
                                      int64_t op, int64_t phase_coeff) {
  switch ((qasm_two_qubit_op_t)op) {
  case QASM_TWO_CZ:
    return apply_cz(importer, left, right);
  case QASM_TWO_CPHASE:
    return apply_controlled_phase(importer, left, right, (uint64_t)phase_coeff);
  case QASM_TWO_CH:
    return apply_h(importer, right) && apply_cz(importer, left, right) &&
           apply_h(importer, right);
  case QASM_TWO_CX:
    return apply_cx_decomposition(importer, left, right);
  case QASM_TWO_CY:
    return apply_cy_decomposition(importer, left, right);
  case QASM_TWO_CSX:
    return apply_csx_decomposition(importer, left, right,
                                   coeff_from_pi_over_eight_units(importer, 4));
  case QASM_TWO_CSXDG:
    return apply_csx_decomposition(importer, left, right,
                                   coeff_from_pi_over_eight_units(importer, 12));
  case QASM_TWO_SWAP: {
    const qasm_affine_t tmp = importer->current[left];
    importer->current[left] = importer->current[right];
    importer->current[right] = tmp;
    return true;
  }
  case QASM_TWO_DCX:
    return apply_cx_decomposition(importer, left, right) &&
           apply_cx_decomposition(importer, right, left);
  case QASM_TWO_ISWAP:
    return apply_iswap_decomposition(importer, left, right);
  }
  set_error(importer, "internal error: unknown two-qubit operation");
  return false;
}

static bool apply_angle_two_qubit_operands(qasm_importer_t *importer, char *rest, int64_t units,
                                           qasm_angle_two_qubit_fn apply) {
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
    return apply(importer, left.qubit, right.qubit, units);
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
    if (!apply(importer, left_qubit, right_qubit, units)) {
      return false;
    }
  }
  return true;
}

typedef bool (*qasm_two_value_two_qubit_fn)(qasm_importer_t *importer, uint32_t left,
                                            uint32_t right, int64_t first, int64_t second);

static bool apply_two_value_two_qubit_operand(qasm_importer_t *importer, char *rest,
                                              int64_t first, int64_t second,
                                              qasm_two_value_two_qubit_fn apply) {
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
    return apply(importer, left.qubit, right.qubit, first, second);
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
    if (!apply(importer, left_qubit, right_qubit, first, second)) {
      return false;
    }
  }
  return true;
}

typedef bool (*qasm_radian_two_qubit_fn)(qasm_importer_t *importer, uint32_t left,
                                         uint32_t right, double angle);

static bool apply_radian_two_qubit_operands(qasm_importer_t *importer, char *rest, double angle,
                                            qasm_radian_two_qubit_fn apply) {
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
    return apply(importer, left.qubit, right.qubit, angle);
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
    if (!apply(importer, left_qubit, right_qubit, angle)) {
      return false;
    }
  }
  return true;
}

/* cu(theta,phi,lambda,gamma) needs 4 angle values + 2 qubit operands -- a shape the two-qubit
 * wiring above doesn't cover (they only forward a single value). Same split/register-expansion
 * logic, just forwarding 4 values instead of 1. */
typedef bool (*qasm_param_cu_two_qubit_fn)(qasm_importer_t *importer, uint32_t left,
                                           uint32_t right, int64_t theta, int64_t phi,
                                           int64_t lambda, int64_t gamma);

static bool apply_param_cu_two_qubit_operand(qasm_importer_t *importer, char *rest, int64_t theta,
                                             int64_t phi, int64_t lambda, int64_t gamma,
                                             qasm_param_cu_two_qubit_fn apply) {
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
    return apply(importer, left.qubit, right.qubit, theta, phi, lambda, gamma);
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
    if (!apply(importer, left_qubit, right_qubit, theta, phi, lambda, gamma)) {
      return false;
    }
  }
  return true;
}

typedef bool (*qasm_radian_param_cu_two_qubit_fn)(qasm_importer_t *importer, uint32_t left,
                                                   uint32_t right, double theta, double phi,
                                                   double lambda, double gamma);

static bool apply_radian_param_cu_two_qubit_operand(qasm_importer_t *importer, char *rest,
                                                    double theta, double phi, double lambda,
                                                    double gamma,
                                                    qasm_radian_param_cu_two_qubit_fn apply) {
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
    return apply(importer, left.qubit, right.qubit, theta, phi, lambda, gamma);
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
    if (!apply(importer, left_qubit, right_qubit, theta, phi, lambda, gamma)) {
      return false;
    }
  }
  return true;
}

typedef bool (*qasm_three_qubit_fn)(qasm_importer_t *importer, uint32_t first, uint32_t second,
                                    uint32_t third);

static bool apply_three_qubit_operands(qasm_importer_t *importer, char *rest,
                                       qasm_three_qubit_fn apply) {
  char *first_text = NULL;
  char *second_text = NULL;
  char *third_text = NULL;
  if (!split_three_operands(importer, rest, &first_text, &second_text, &third_text)) {
    return false;
  }

  qasm_operand_t first = {0};
  qasm_operand_t second = {0};
  qasm_operand_t third = {0};
  if (!parse_qubit_or_reg_operand(importer, first_text, &first) ||
      !parse_qubit_or_reg_operand(importer, second_text, &second) ||
      !parse_qubit_or_reg_operand(importer, third_text, &third)) {
    return false;
  }

  if (!first.is_reg && !second.is_reg && !third.is_reg) {
    return apply(importer, first.qubit, second.qubit, third.qubit);
  }
  if (!first.is_reg || !second.is_reg || !third.is_reg) {
    set_error(importer, "three-qubit gates cannot mix qreg and indexed qubit operands");
    return false;
  }
  if (first.reg->size != second.reg->size || first.reg->size != third.reg->size) {
    set_error(importer, "three-qubit qreg operands must have matching sizes");
    return false;
  }

  for (uint32_t i = 0; i < first.reg->size; i++) {
    const uint32_t first_qubit = first.reg->offset + i;
    const uint32_t second_qubit = second.reg->offset + i;
    const uint32_t third_qubit = third.reg->offset + i;
    if (!apply(importer, first_qubit, second_qubit, third_qubit)) {
      return false;
    }
  }
  return true;
}

static bool apply_approx_gate(qasm_importer_t *importer, char *gate, char *rest,
                              bool *out_handled) {
  *out_handled = false;

  double u3_angles[3] = {0.0};
  bool is_u3 = false;
  if (!parse_param_radian_list(importer, gate, "u3(", "u3", u3_angles, 3, &is_u3)) {
    return false;
  }
  if (is_u3) {
    *out_handled = true;
    return apply_radian_three_param_one_qubit_operand(importer, rest, u3_angles[0], u3_angles[1],
                                                      u3_angles[2], apply_u3_angle);
  }

  if (!parse_param_radian_list(importer, gate, "u(", "u", u3_angles, 3, &is_u3)) {
    return false;
  }
  if (is_u3) {
    *out_handled = true;
    return apply_radian_three_param_one_qubit_operand(importer, rest, u3_angles[0], u3_angles[1],
                                                      u3_angles[2], apply_u3_angle);
  }

  double u2_angles[2] = {0.0};
  bool is_u2 = false;
  if (!parse_param_radian_list(importer, gate, "u2(", "u2", u2_angles, 2, &is_u2)) {
    return false;
  }
  if (is_u2) {
    *out_handled = true;
    return apply_radian_two_param_one_qubit_operand(importer, rest, u2_angles[0], u2_angles[1],
                                                    apply_u2_angle);
  }

  double angle = 0.0;
  bool matches = false;
  if (!parse_param_angle_radians(importer, gate, "gphase(", "gphase", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    if (*trim(rest) != '\0') {
      set_error(importer, "gphase gate does not take qubit operands");
      return false;
    }
    return add_constant_angle(importer, angle);
  }

  if (!parse_param_angle_radians(importer, gate, "rz(", "rz", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_one_qubit_operand(importer, rest, angle, apply_rz_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "rx(", "rx", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_one_qubit_operand(importer, rest, angle, apply_rx_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "ry(", "ry", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_one_qubit_operand(importer, rest, angle, apply_ry_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "crz(", "crz", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_crz_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "cry(", "cry", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_cry_angle);
  }

  double cu_angles[4] = {0.0};
  bool is_cu = false;
  if (!parse_param_radian_list(importer, gate, "cu(", "cu", cu_angles, 4, &is_cu)) {
    return false;
  }
  if (is_cu) {
    *out_handled = true;
    return apply_radian_param_cu_two_qubit_operand(importer, rest, cu_angles[0], cu_angles[1],
                                                   cu_angles[2], cu_angles[3], apply_cu_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "rxx(", "rxx", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_rxx_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "ryy(", "ryy", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_ryy_angle);
  }

  if (!parse_param_angle_radians(importer, gate, "rzz(", "rzz", &angle, &matches)) {
    return false;
  }
  if (matches) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_rzz_angle);
  }

  bool is_phase = false;
  if (!phase_angle_for_gate(importer, gate, &angle, &is_phase)) {
    return false;
  }
  if (is_phase) {
    *out_handled = true;
    return apply_radian_one_qubit_operand(importer, rest, angle, apply_phase_angle);
  }

  bool is_controlled_phase = false;
  if (!controlled_phase_angle_for_gate(importer, gate, &angle, &is_controlled_phase)) {
    return false;
  }
  if (is_controlled_phase) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_controlled_phase_angle);
  }

  if (strcmp(gate, "csx") == 0 || strcmp(gate, "csxdg") == 0) {
    *out_handled = true;
    const double csx_angle = strcmp(gate, "csx") == 0 ? QASM_PI / 2.0 : -QASM_PI / 2.0;
    return apply_radian_two_qubit_operands(importer, rest, csx_angle,
                                           apply_csx_angle_decomposition);
  }

  if (gate[0] == 'c' && strcmp(gate, "cz") != 0 &&
      named_phase_angle_for_gate(gate + 1, &angle)) {
    *out_handled = true;
    return apply_radian_two_qubit_operands(importer, rest, angle, apply_controlled_phase_angle);
  }

  return true;
}

/* P(units), units already a full modulus-tick coeff; the qasm_param_one_qubit_fn used to
 * broadcast a theta=0 u-gate. */
static bool apply_phase_units(qasm_importer_t *importer, uint32_t qubit, int64_t units) {
  return apply_phase(importer, qubit, coeff_from_units(importer, units));
}

/* U(pi/2,phi,lambda) = P(phi) H P(lambda+pi), and
 * U(-pi/2,phi,lambda) = P(phi+pi) H P(lambda). These direct forms keep odd phase parameters
 * exact without introducing an extra halving from the generic RZ-RY-RZ path. theta/phi/lambda
 * are full modulus-tick coeffs (see apply_rz); the pi/2 threshold is +-sign_coeff/2. */
static bool apply_u3_half_pi(qasm_importer_t *importer, uint32_t qubit, int64_t theta_units,
                             int64_t phi_units, int64_t lambda_units) {
  const int64_t pi_units = (int64_t)sign_coeff(importer);
  if (theta_units == pi_units / 2) {
    return apply_phase(importer, qubit, coeff_from_units(importer, lambda_units + pi_units)) &&
           apply_h(importer, qubit) &&
           apply_phase(importer, qubit, coeff_from_units(importer, phi_units));
  }
  return apply_phase(importer, qubit, coeff_from_units(importer, lambda_units)) &&
         apply_h(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_units(importer, phi_units + pi_units));
}

static bool apply_u2_units(qasm_importer_t *importer, uint32_t qubit, int64_t phi_units,
                           int64_t lambda_units) {
  return apply_u3_half_pi(importer, qubit, (int64_t)sign_coeff(importer) / 2, phi_units,
                          lambda_units);
}

/* U(pi,phi,lambda) = P(phi) X P(lambda+pi); the negative-theta form moves the pi shift
 * to phi. Like the half-pi identities, this avoids artificial half-angle phases. */
static bool apply_u3_pi(qasm_importer_t *importer, uint32_t qubit, int64_t theta_units,
                        int64_t phi_units, int64_t lambda_units) {
  const int64_t pi_units = (int64_t)sign_coeff(importer);
  if (theta_units == pi_units) {
    return apply_phase(importer, qubit, coeff_from_units(importer, lambda_units + pi_units)) &&
           apply_x_decomposition(importer, qubit) &&
           apply_phase(importer, qubit, coeff_from_units(importer, phi_units));
  }
  return apply_phase(importer, qubit, coeff_from_units(importer, lambda_units)) &&
         apply_x_decomposition(importer, qubit) &&
         apply_phase(importer, qubit, coeff_from_units(importer, phi_units + pi_units));
}

/* Exact u3/u dispatch; theta/phi/lambda are full modulus-tick coeffs from the dynamic-resolution
 * parser (see apply_rz). A theta of 0 makes the gate the pure phase U(0, phi, lambda) =
 * P(phi + lambda), exact for any phi + lambda (no rz angle-halving) -- this is what lets e.g.
 * Qiskit's u(0, 5pi/8, -3pi/8) = T import exactly instead of only via --approx. +-pi/2 and +-pi
 * get direct identities avoiding an extra halving. Any other theta takes the general rz-ry-rz
 * path (apply_u3), which now only needs theta itself to halve evenly (enforced by apply_rz,
 * reached via apply_ry) -- unlike the old fixed pi/8-tick scheme, phi and lambda need no parity
 * check of their own here. */
static bool apply_u3_units_operand(qasm_importer_t *importer, char *rest,
                                   const int64_t units[3]) {
  const int64_t pi_units = (int64_t)sign_coeff(importer);
  if (units[0] == 0) {
    return apply_param_one_qubit_operand(importer, rest, units[1] + units[2], apply_phase_units);
  }
  if (units[0] == pi_units / 2 || units[0] == -(pi_units / 2)) {
    return apply_param_three_qubit_operand(importer, rest, units[0], units[1], units[2],
                                           apply_u3_half_pi);
  }
  if (units[0] == pi_units || units[0] == -pi_units) {
    return apply_param_three_qubit_operand(importer, rest, units[0], units[1], units[2],
                                           apply_u3_pi);
  }
  return apply_param_three_qubit_operand(importer, rest, units[0], units[1], units[2], apply_u3);
}

static bool apply_gate(qasm_importer_t *importer, char *gate, char *rest) {
  importer->saw_gate = true;

  if (importer->approx_enabled) {
    bool handled = false;
    if (!apply_approx_gate(importer, gate, rest, &handled)) {
      return false;
    }
    if (handled) {
      return true;
    }
  }

  uint64_t global_phase_coeff = 0;
  bool is_gphase = false;
  if (!parse_param_phase_coeff(importer, gate, "gphase(", "gphase", &global_phase_coeff,
                               &is_gphase)) {
    return false;
  }
  if (is_gphase) {
    if (*trim(rest) != '\0') {
      set_error(importer, "gphase gate does not take qubit operands");
      return false;
    }
    return add_constant(importer, global_phase_coeff);
  }

  int64_t u3_units[3] = {0};
  bool is_u3 = false;
  if (!parse_param_unit_list(importer, gate, "u3(", "u3", u3_units, 3, &is_u3)) {
    return false;
  }
  if (is_u3) {
    return apply_u3_units_operand(importer, rest, u3_units);
  }

  if (!parse_param_unit_list(importer, gate, "u(", "u", u3_units, 3, &is_u3)) {
    return false;
  }
  if (is_u3) {
    return apply_u3_units_operand(importer, rest, u3_units);
  }

  int64_t u2_units[2] = {0};
  bool is_u2 = false;
  if (!parse_param_unit_list(importer, gate, "u2(", "u2", u2_units, 2, &is_u2)) {
    return false;
  }
  if (is_u2) {
    return apply_param_two_qubit_operand(importer, rest, u2_units[0], u2_units[1],
                                         apply_u2_units);
  }

  int64_t rz_units = 0;
  bool is_rz = false;
  if (!rz_units_for_gate(importer, gate, "rz(", "rz", &rz_units, &is_rz)) {
    return false;
  }
  if (is_rz) {
    return apply_param_one_qubit_operand(importer, rest, rz_units, apply_rz);
  }

  int64_t rx_units = 0;
  bool is_rx = false;
  if (!rz_units_for_gate(importer, gate, "rx(", "rx", &rx_units, &is_rx)) {
    return false;
  }
  if (is_rx) {
    return apply_param_one_qubit_operand(importer, rest, rx_units, apply_rx);
  }

  int64_t ry_units = 0;
  bool is_ry = false;
  if (!rz_units_for_gate(importer, gate, "ry(", "ry", &ry_units, &is_ry)) {
    return false;
  }
  if (is_ry) {
    return apply_param_one_qubit_operand(importer, rest, ry_units, apply_ry);
  }

  int64_t crz_units = 0;
  bool is_crz = false;
  if (!rz_units_for_gate(importer, gate, "crz(", "crz", &crz_units, &is_crz)) {
    return false;
  }
  if (is_crz) {
    return apply_angle_two_qubit_operands(importer, rest, crz_units, apply_crz);
  }

  int64_t cry_units = 0;
  bool is_cry = false;
  if (!rz_units_for_gate(importer, gate, "cry(", "cry", &cry_units, &is_cry)) {
    return false;
  }
  if (is_cry) {
    return apply_angle_two_qubit_operands(importer, rest, cry_units, apply_cry);
  }

  int64_t cu_units[4] = {0};
  bool is_cu = false;
  if (!parse_param_unit_list(importer, gate, "cu(", "cu", cu_units, 4, &is_cu)) {
    return false;
  }
  if (is_cu) {
    return apply_param_cu_two_qubit_operand(importer, rest, cu_units[0], cu_units[1],
                                            cu_units[2], cu_units[3], apply_cu);
  }

  int64_t rxx_units = 0;
  bool is_rxx = false;
  if (!rz_units_for_gate(importer, gate, "rxx(", "rxx", &rxx_units, &is_rxx)) {
    return false;
  }
  if (is_rxx) {
    return apply_angle_two_qubit_operands(importer, rest, rxx_units, apply_rxx);
  }

  int64_t ryy_units = 0;
  bool is_ryy = false;
  if (!rz_units_for_gate(importer, gate, "ryy(", "ryy", &ryy_units, &is_ryy)) {
    return false;
  }
  if (is_ryy) {
    return apply_angle_two_qubit_operands(importer, rest, ryy_units, apply_ryy);
  }

  int64_t rzz_units = 0;
  bool is_rzz = false;
  if (!rz_units_for_gate(importer, gate, "rzz(", "rzz", &rzz_units, &is_rzz)) {
    return false;
  }
  if (is_rzz) {
    return apply_angle_two_qubit_operands(importer, rest, rzz_units, apply_rzz);
  }

  uint64_t phase_coeff = 0;
  bool is_phase = false;
  if (!phase_coeff_for_gate(importer, gate, &phase_coeff, &is_phase)) {
    return false;
  }
  if (is_phase) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_PHASE,
                                         (int64_t)phase_coeff, apply_one_qubit_op_values);
  }

  uint64_t controlled_phase_coeff = 0;
  bool is_controlled_phase = false;
  if (!controlled_phase_coeff_for_gate(importer, gate, &controlled_phase_coeff,
                                       &is_controlled_phase)) {
    return false;
  }
  if (is_controlled_phase) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CPHASE,
                                             (int64_t)controlled_phase_coeff,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "h") == 0) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_H, 0,
                                         apply_one_qubit_op_values);
  }

  if (strcmp(gate, "id") == 0) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_ID, 0,
                                         apply_one_qubit_op_values);
  }

  if (strcmp(gate, "x") == 0) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_X, 0,
                                         apply_one_qubit_op_values);
  }

  if (strcmp(gate, "y") == 0) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_Y, 0,
                                         apply_one_qubit_op_values);
  }

  if (strcmp(gate, "sx") == 0) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_SX, 0,
                                         apply_one_qubit_op_values);
  }

  if (strcmp(gate, "sxdg") == 0) {
    return apply_param_two_qubit_operand(importer, rest, (int64_t)QASM_ONE_SXDG, 0,
                                         apply_one_qubit_op_values);
  }

  if (strcmp(gate, "cz") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CZ, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "ch") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CH, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "cx") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CX, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "cy") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CY, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "csx") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CSX, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "csxdg") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_CSXDG, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "swap") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_SWAP, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "dcx") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_DCX, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "iswap") == 0) {
    return apply_two_value_two_qubit_operand(importer, rest, (int64_t)QASM_TWO_ISWAP, 0,
                                             apply_two_qubit_op_values);
  }

  if (strcmp(gate, "ccz") == 0) {
    return apply_three_qubit_operands(importer, rest, apply_ccz_decomposition);
  }

  if (strcmp(gate, "ccx") == 0) {
    return apply_three_qubit_operands(importer, rest, apply_ccx_decomposition);
  }

  if (strcmp(gate, "rccx") == 0) {
    return apply_three_qubit_operands(importer, rest, apply_rccx_decomposition);
  }

  if (strcmp(gate, "cswap") == 0) {
    return apply_three_qubit_operands(importer, rest, apply_cswap_decomposition);
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
  if (starts_with_keyword(text, "opaque")) {
    return true;
  }
  if (starts_with_keyword(text, "qreg")) {
    return parse_qreg(importer, trim(text + strlen("qreg")));
  }
  if (starts_with_keyword(text, "barrier")) {
    return true;
  }
  if (starts_with_keyword(text, "creg")) {
    /* An inert classical register -- declared but never written by a measure or read by
     * an `if` -- has no effect on the amplitude, so ignore it like a barrier. A measure
     * that targets it, or an `if` that reads it, is still rejected just below, so nothing
     * dynamic or non-unitary slips through on the strength of this. */
    return true;
  }
  if (starts_with_keyword(text, "measure")) {
    importer->saw_gate = true;
    return apply_measure(importer, trim(text + strlen("measure")));
  }
  if (starts_with_keyword(text, "reset")) {
    importer->saw_gate = true;
    return apply_reset_operand(importer, trim(text + strlen("reset")));
  }
  if (starts_with_keyword(text, "if")) {
    set_error(importer, "dynamic or classical OpenQASM features are not supported");
    return false;
  }

  /* Find the end of the bare gate/operator name: the first whitespace or '(', whichever comes
   * first. A parenthesized parameter list may follow directly ("u(0.1,...)") or after whitespace
   * ("cu1 (pi/4)"); either way, the first '(' anywhere past this point (if any) opens it, since
   * OpenQASM 2 operand syntax never contains a literal '(' (registers use '[' ']'). */
  char *name_end = text;
  while (*name_end != '\0' && !isspace((unsigned char)*name_end) && *name_end != '(') {
    name_end++;
  }
  char *open_paren = strchr(name_end, '(');

  char *rest = NULL;
  if (open_paren != NULL) {
    /* Depth-aware scan for the matching ')', so whitespace or nested parentheses inside the
     * parameter list (e.g. from macro-substituted angle arithmetic like
     * "u(0.1, -pi/2 + pi/2)") don't truncate the gate token early. */
    int depth = 0;
    char *scan = open_paren;
    do {
      if (*scan == '(') {
        depth++;
      } else if (*scan == ')') {
        depth--;
      } else if (*scan == '\0') {
        set_error(importer, "unbalanced gate parameter list");
        return false;
      }
      scan++;
    } while (depth > 0);
    char *close = scan - 1;
    rest = trim(close + 1);

    const size_t name_len = (size_t)(name_end - text);
    const size_t params_len = (size_t)(close - open_paren + 1);
    memmove(text + name_len, open_paren, params_len);
    text[name_len + params_len] = '\0';
  } else {
    if (*name_end == '\0') {
      set_error(importer, "gate statement is missing operands");
      return false;
    }
    *name_end = '\0';
    rest = trim(name_end + 1);
  }

  lowercase_ascii(text);
  if (*rest == '\0' && strncmp(text, "gphase(", strlen("gphase(")) != 0) {
    set_error(importer, "gate statement is missing operands");
    return false;
  }
  return apply_gate(importer, text, rest);
}

static void free_importer(qasm_importer_t *importer) {
  for (size_t i = 0; i < importer->regs_len; i++) {
    free(importer->regs[i].name);
  }
  free(importer->regs);
  for (uint32_t q = 0; q < importer->qubits_cap; q++) {
    free(importer->current[q].vars);
  }
  free(importer->current);
  free(importer->unary);
  free(importer->edges);
  free(importer->zero_pins);
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

static bool write_zero_qsop(FILE *file, uint64_t norm_h, uint64_t modulus) {
  fprintf(file, "p qsop-sign %" PRIu64 " 1 0\n", modulus);
  fprintf(file, "n %" PRIu64 "\n", norm_h);
  fputs("cst 0\n\n", file);
  fprintf(file, "u 0 %" PRIu64 "\n", modulus / 2U);
  return !ferror(file);
}

static bool pin_boundary_variable(int8_t *pins, uint32_t var, uint32_t value, bool *conflict) {
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
    const qasm_affine_t *affine = &importer->current[q];
    if (affine->len == 0U) {
      if ((uint32_t)affine->constant != value) {
        conflict = true;
      }
    } else {
      /* write_raw_qsop materializes multi-variable output parities before allocating pins. */
      if (affine->len != 1U) {
        free(pins);
        return false;
      }
      pin_boundary_variable(pins, affine->vars[0], value ^ (uint32_t)affine->constant, &conflict);
    }
  }
  /* Mid-circuit reset pins: each fresh post-reset variable is |0>. A conflict here (e.g. an
   * --output that demands 1 on a wire whose final op was a reset to 0) correctly yields amplitude
   * zero, same as any other boundary conflict. */
  for (uint32_t i = 0; i < importer->zero_pins_len && !conflict; i++) {
    pin_boundary_variable(pins, importer->zero_pins[i], 0U, &conflict);
  }

  *out_pins = pins;
  *out_conflict = conflict;
  return true;
}

/* Finds the coarsest modulus (a divisor of importer->modulus, floored at 8) that still
 * represents every accumulated coefficient exactly, halving one step at a time from the parse
 * modulus. At the historical fixed parse modulus of 16, this is exactly the old single
 * 16-or-8 check; at a wider retry modulus, it walks all the way down if the circuit's angles
 * turn out not to need the extra resolution after all, keeping goldens byte-stable. */
static uint64_t output_modulus(const qasm_importer_t *importer) {
  if (importer->approx_enabled) {
    return importer->modulus;
  }
  uint64_t modulus = importer->modulus;
  while (modulus > 8) {
    const uint64_t scale = importer->modulus / (modulus / 2U);
    bool all_even = importer->constant % scale == 0;
    for (uint32_t i = 0; all_even && i < importer->unary_len; i++) {
      all_even = importer->unary[i].q % scale == 0;
    }
    for (uint32_t i = 0; all_even && i < importer->edges_len; i++) {
      all_even = importer->edges[i].q % scale == 0;
    }
    if (!all_even) {
      break;
    }
    modulus /= 2U;
  }
  return modulus;
}

static uint64_t output_coeff(const qasm_importer_t *importer, uint64_t coeff, uint64_t modulus) {
  if (modulus == importer->modulus) {
    return coeff % modulus;
  }
  const uint64_t scale = importer->modulus / modulus;
  return coeff / scale;
}

static bool write_raw_qsop(FILE *file, qasm_importer_t *importer) {
  /* A fixed output bit is a projector on the final affine wire value. Materializing only the
   * multi-variable expressions here lets the ordinary pin reducer eliminate the value side of
   * each equality gadget again, leaving one sign-check variable in canonical output. */
  for (uint32_t q = 0; q < importer->nqubits; q++) {
    if (importer->current[q].len > 1U) {
      uint32_t value = 0;
      if (!affine_materialize(importer, &importer->current[q], &value) ||
          !affine_set_singleton(importer, &importer->current[q], value)) {
        return false;
      }
    }
  }
  int8_t *pins = NULL;
  bool boundary_conflict = false;
  if (!collect_boundary_pins(importer, &pins, &boundary_conflict)) {
    return false;
  }
  if (boundary_conflict) {
    free(pins);
    return write_zero_qsop(file, importer->norm_h,
                           importer->approx_enabled ? importer->modulus : 8U);
  }

  const uint64_t modulus = output_modulus(importer);
  const uint64_t sign_coeff = modulus / 2U;
  for (uint32_t i = 0; i < importer->edges_len; i++) {
    const uint64_t coeff = output_coeff(importer, importer->edges[i].q, modulus);
    if (coeff != sign_coeff) {
      set_error(importer,
                "unsupported non-sign quadratic phase coefficient %" PRIu64
                " for modulus %" PRIu64 " between variables %" PRIu32 " and %" PRIu32,
                coeff, modulus, importer->edges[i].u, importer->edges[i].v);
      free(pins);
      return false;
    }
  }

  fprintf(file, "p qsop-sign %" PRIu64 " %" PRIu32 " %" PRIu32 "\n", modulus, importer->nvars,
          importer->edges_len);
  fprintf(file, "n %" PRIu64 "\n", importer->norm_h);
  fprintf(file, "cst %" PRIu64 "\n", output_coeff(importer, importer->constant, modulus));

  for (uint32_t i = 0; i < importer->unary_len; i++) {
    fprintf(file, "u %" PRIu32 " %" PRIu64 "\n", importer->unary[i].v,
            output_coeff(importer, importer->unary[i].q, modulus));
  }
  for (uint32_t i = 0; i < importer->edges_len; i++) {
    fprintf(file, "e %" PRIu32 " %" PRIu32 "\n", importer->edges[i].u, importer->edges[i].v);
  }
  for (uint32_t v = 0; v < importer->nvars; v++) {
    if (pins[v] != -1) {
      fprintf(file, "f %" PRIu32 " %d\n", v, pins[v]);
    }
  }

  free(pins);
  return !ferror(file);
}

static void write_approx_certificate(FILE *file, const qasm_importer_t *importer) {
  if (!importer->approx_enabled) {
    return;
  }
  fprintf(file, "c qasm2sop_approx epsilon %.17g\n", importer->approx_epsilon);
  fprintf(file, "c qasm2sop_approx modulus %" PRIu64 "\n", importer->modulus);
  fprintf(file, "c qasm2sop_approx rounded_phase_ops %" PRIu64 "\n",
          importer->approx_phase_count);
  fprintf(file, "c qasm2sop_approx additive_amplitude_error_bound %.17g\n",
          importer->approx_delta);
}

static bool canonicalize_importer(qasm_importer_t *importer, qsop_error_t *error, bool optimize,
                                  qsop_instance_t **out_qsop) {
  *out_qsop = NULL;
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
    if (importer->error[0] != '\0') {
      snprintf(error->message, sizeof(error->message), "%s", importer->error);
    } else {
      snprintf(error->message, sizeof(error->message), "failed to write raw QSOP");
    }
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

  /* Collapse the Hadamard-uncompute variables the parser's parity-dedup cannot reach, so the
   * emitted instance is smaller (fewer variables/edges, lower treewidth) for the solver. This
   * is amplitude-exact, so it never invalidates the approx certificate written below. */
  if (optimize && !qsop_simplify_hadamard(qsop)) {
    snprintf(error->message, sizeof(error->message), "out of memory while simplifying QSOP");
    qsop_free(qsop);
    return false;
  }

  *out_qsop = qsop;
  return true;
}

static bool prefer_permutation_candidate(const qsop_instance_t *affine,
                                    const qsop_instance_t *permutation) {
  /* First preserve both basic size measures monotonically. */
  if (affine->nvars > permutation->nvars || affine->nedges > permutation->nedges) {
    return true;
  }
  if ((affine->nvars == permutation->nvars && affine->nedges == permutation->nedges) ||
      affine->nvars > 1000U || permutation->nvars > 1000U || affine->nedges > 5000U ||
      permutation->nedges > 5000U) {
    return false;
  }

  /* A smaller graph can still have a worse elimination topology. On instances where the
   * diagnostic itself is cheap, require the affine candidate to preserve both greedy width and
   * its more timing-relevant DP-work estimate. Giant QFT/qwalk graphs stay on the O(1) size guard
   * above; running min-fill inside an importer must not turn those imports into minute-long jobs. */
  uint32_t affine_width = 0;
  uint32_t permutation_width = 0;
  uint64_t affine_work = 0;
  uint64_t permutation_work = 0;
  qsop_error_t error = {0};
  if (!qsop_min_fill_eliminate(affine->nvars, affine->edge_u, affine->edge_v, affine->nedges,
                               QSOP_TREEWIDTH_ORDER_MIN_FILL, UINT32_MAX, NULL, &affine_width,
                               NULL, &affine_work, NULL, &error) ||
      !qsop_min_fill_eliminate(permutation->nvars, permutation->edge_u, permutation->edge_v, permutation->nedges,
                               QSOP_TREEWIDTH_ORDER_MIN_FILL, UINT32_MAX, NULL, &permutation_width,
                               NULL, &permutation_work, NULL, &error)) {
    return false;
  }
  return affine_width > permutation_width || affine_work > permutation_work;
}

static void print_qsop_error(const qsop_error_t *error) {
  if (error->line > 0) {
    fprintf(stderr, "error: %s:%zu:%zu: %s\n", error->path, error->line, error->column,
            error->message);
  } else {
    fprintf(stderr, "error: %s\n", error->message);
  }
}

static bool parse_approx_epsilon(const char *text, double *out_epsilon, char *error,
                                 size_t error_size) {
  errno = 0;
  char *end = NULL;
  const double epsilon = strtod(text, &end);
  if (errno != 0 || end == text || *trim(end) != '\0' || !isfinite(epsilon) || epsilon <= 0.0) {
    snprintf(error, error_size, "--approx must be a positive finite error budget");
    return false;
  }
  *out_epsilon = epsilon;
  return true;
}

static bool read_stream_to_memory(FILE *input, char **out_data, size_t *out_len, char *error,
                                  size_t error_size) {
  size_t cap = 8192U;
  size_t len = 0;
  char *data = malloc(cap);
  if (data == NULL) {
    snprintf(error, error_size, "out of memory while reading input");
    return false;
  }

  for (;;) {
    if (len == cap) {
      if (cap > SIZE_MAX / 2U) {
        free(data);
        snprintf(error, error_size, "input is too large");
        return false;
      }
      const size_t new_cap = cap * 2U;
      char *new_data = realloc(data, new_cap);
      if (new_data == NULL) {
        free(data);
        snprintf(error, error_size, "out of memory while reading input");
        return false;
      }
      data = new_data;
      cap = new_cap;
    }

    const size_t nread = fread(data + len, 1, cap - len, input);
    len += nread;
    if (nread == 0) {
      if (ferror(input)) {
        free(data);
        snprintf(error, error_size, "read failed: %s", strerror(errno));
        return false;
      }
      break;
    }
  }

  if (len + 1U > cap) {
    char *new_data = realloc(data, len + 1U);
    if (new_data == NULL) {
      free(data);
      snprintf(error, error_size, "out of memory while reading input");
      return false;
    }
    data = new_data;
  }
  data[len] = '\0';
  *out_data = data;
  *out_len = len;
  return true;
}

static bool estimate_approx_phase_ops(const char *source, uint64_t *out_phase_ops, char *error,
                                      size_t error_size) {
  char *copy = strdup(source);
  if (copy == NULL) {
    snprintf(error, error_size, "out of memory while estimating approximate phase count");
    return false;
  }

  uint64_t nqubits = 0;
  uint64_t phase_ops = 0;
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line != NULL;
       line = strtok_r(NULL, "\n", &save)) {
    char *comment = strstr(line, "//");
    if (comment != NULL) {
      *comment = '\0';
    }
    char *text = trim(line);
    if (*text == '\0') {
      continue;
    }
    const size_t len = strlen(text);
    if (len > 0 && text[len - 1U] == ';') {
      text[len - 1U] = '\0';
      text = trim(text);
    }
    if (*text == '\0' || starts_with_keyword(text, "OPENQASM") ||
        starts_with_keyword(text, "include") || starts_with_keyword(text, "opaque") ||
        starts_with_keyword(text, "barrier")) {
      continue;
    }
    if (starts_with_keyword(text, "qreg")) {
      char *rest = trim(text + strlen("qreg"));
      char *open = strchr(rest, '[');
      char *close = open == NULL ? NULL : strchr(open + 1, ']');
      if (open != NULL && close != NULL) {
        *close = '\0';
        uint32_t size = 0;
        if (parse_u32_text(trim(open + 1), &size)) {
          if (UINT64_MAX - nqubits < size) {
            free(copy);
            snprintf(error, error_size, "too many qubits while estimating approximation modulus");
            return false;
          }
          nqubits += size;
        }
      }
      continue;
    }
    if (starts_with_keyword(text, "creg") || starts_with_keyword(text, "measure") ||
        starts_with_keyword(text, "reset") || starts_with_keyword(text, "if")) {
      continue;
    }

    const uint64_t width = nqubits == 0 ? 1U : nqubits;
    if (width > UINT64_MAX / 16U || phase_ops > UINT64_MAX - width * 16U) {
      free(copy);
      snprintf(error, error_size, "too many approximate phase operations");
      return false;
    }
    phase_ops += width * 16U;
  }

  free(copy);
  *out_phase_ops = phase_ops;
  return true;
}

/* Ceiling for the chosen modulus, well clear of UINT64_MAX so that 16 * value here, and the
 * caller's doubling retry loop (qasm2sop_main), can never overflow uint64_t. */
#define QASM_APPROX_MODULUS_MAX (UINT64_MAX / 32U)

/* Single retry cap for exact mode's dynamic modulus (see qasm2sop_main): matches the solver's
 * own `qsop->r <= UINT32_MAX` gate on the count-table/all-modes backends, so anything exact
 * mode can produce here stays solvable downstream. Covers a QFT-30-scale cp(pi/2^k) chain
 * (needs modulus 2^(k+2) after the CX-sandwich halving, i.e. 2^32 at k=30). */
#define QASM_EXACT_MODULUS_MAX ((uint64_t)UINT32_MAX + 1U)

static bool choose_approx_modulus(uint64_t estimated_phase_ops, double epsilon,
                                  uint64_t *out_modulus, char *error, size_t error_size) {
  if (estimated_phase_ops == 0) {
    *out_modulus = 16;
    return true;
  }

  const long double multiple =
      ceill((long double)QASM_PI * (long double)estimated_phase_ops / (16.0L * epsilon));
  if (!isfinite((double)multiple) || multiple > (long double)QASM_APPROX_MODULUS_MAX / 16.0L) {
    snprintf(error, error_size, "--approx %.17g requires a modulus larger than %" PRIu64, epsilon,
             (uint64_t)QASM_APPROX_MODULUS_MAX);
    return false;
  }

  uint64_t value = (uint64_t)multiple;
  if (value == 0) {
    value = 1;
  }
  *out_modulus = 16U * value;
  return true;
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *input_bits = NULL;
  const char *output_bits = NULL;
  const char *approx_text = NULL;
  bool optimize = true;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (dlx4sop_cli_is_version_arg(argv[i])) {
      dlx4sop_cli_print_version(stdout, "qasm2sop");
      return 0;
    }
    if (strcmp(argv[i], "--no-optimize") == 0) {
      optimize = false;
      continue;
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
    if (strcmp(argv[i], "--approx") == 0) {
      if (approx_text != NULL) {
        fputs("error: duplicate --approx option\n", stderr);
        print_usage(stderr);
        return 2;
      }
      if (i + 1 >= argc) {
        fputs("error: missing value for --approx\n", stderr);
        print_usage(stderr);
        return 2;
      }
      approx_text = argv[++i];
      continue;
    }
    if (strncmp(argv[i], "--approx=", strlen("--approx=")) == 0) {
      if (approx_text != NULL) {
        fputs("error: duplicate --approx option\n", stderr);
        print_usage(stderr);
        return 2;
      }
      approx_text = argv[i] + strlen("--approx=");
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

  const bool approx_enabled = approx_text != NULL;
  double approx_epsilon = 0.0;
  char option_error[256] = {0};
  if (approx_enabled &&
      !parse_approx_epsilon(approx_text, &approx_epsilon, option_error, sizeof(option_error))) {
    fprintf(stderr, "error: %s\n", option_error);
    return 2;
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
  bool ok = false;
  char *source = NULL;
  size_t source_len = 0;

  if (!approx_enabled) {
    char read_error[256] = {0};
    if (!read_stream_to_memory(input, &source, &source_len, read_error, sizeof(read_error))) {
      if (input != stdin) {
        fclose(input);
      }
      fprintf(stderr, "error: %s\n", read_error);
      return 1;
    }
    if (input != stdin) {
      fclose(input);
    }

    importer.input_bits = input_bits;
    importer.output_bits = output_bits;
    importer.modulus = 16;
    FILE *memory_input = fmemopen(source, source_len, "rb");
    if (memory_input == NULL) {
      fprintf(stderr, "error: failed to read buffered input\n");
      free(source);
      return 1;
    }
    ok = parse_qasm(memory_input, diagnostic_path, &importer);
    fclose(memory_input);

    /* A dyadic angle that needs a finer tick than modulus 16 provides (e.g. a QFT cp(pi/2^k)
     * chain, or a plain rz(pi/8)) fails the first parse with angle_refusal set; retry once at
     * the cap and let output_modulus narrow the result back down, rather than doubling ~28
     * times to find the minimal modulus. */
    if (!ok && importer.angle_refusal) {
      free_importer(&importer);
      importer = (qasm_importer_t){0};
      importer.input_bits = input_bits;
      importer.output_bits = output_bits;
      importer.modulus = QASM_EXACT_MODULUS_MAX;
      memory_input = fmemopen(source, source_len, "rb");
      if (memory_input == NULL) {
        fprintf(stderr, "error: failed to read buffered input\n");
        free(source);
        return 1;
      }
      ok = parse_qasm(memory_input, diagnostic_path, &importer);
      fclose(memory_input);
    }
  } else {
    char read_error[256] = {0};
    if (!read_stream_to_memory(input, &source, &source_len, read_error, sizeof(read_error))) {
      if (input != stdin) {
        fclose(input);
      }
      fprintf(stderr, "error: %s\n", read_error);
      return 1;
    }
    if (input != stdin) {
      fclose(input);
    }

    uint64_t estimated_phase_ops = 0;
    if (!estimate_approx_phase_ops(source, &estimated_phase_ops, read_error, sizeof(read_error))) {
      fprintf(stderr, "error: %s\n", read_error);
      free(source);
      return 1;
    }

    uint64_t approx_modulus = 16;
    if (!choose_approx_modulus(estimated_phase_ops, approx_epsilon, &approx_modulus, read_error,
                               sizeof(read_error))) {
      fprintf(stderr, "error: %s\n", read_error);
      free(source);
      return 2;
    }

    for (;;) {
      importer = (qasm_importer_t){0};
      importer.input_bits = input_bits;
      importer.output_bits = output_bits;
      importer.modulus = approx_modulus;
      importer.approx_enabled = true;
      importer.approx_epsilon = approx_epsilon;

      FILE *memory_input = fmemopen(source, source_len, "rb");
      if (memory_input == NULL) {
        fprintf(stderr, "error: failed to read buffered input\n");
        free(source);
        return 1;
      }
      ok = parse_qasm(memory_input, diagnostic_path, &importer);
      fclose(memory_input);
      if (!ok) {
        break;
      }

      const double tolerance = approx_epsilon * 1e-12 + 1e-15;
      if (importer.approx_delta <= approx_epsilon + tolerance) {
        break;
      }

      const double failed_bound = importer.approx_delta;
      free_importer(&importer);
      if (approx_modulus > QASM_APPROX_MODULUS_MAX / 2U) {
        fprintf(stderr,
                "error: could not certify --approx %.17g within modulus limit; bound is %.17g\n",
                approx_epsilon, failed_bound);
        free(source);
        return 1;
      }
      approx_modulus *= 2U;
    }
  }
  if (!ok) {
    fprintf(stderr, "error: %s:%zu: %s\n", diagnostic_path, importer.line_no, importer.error);
    free_importer(&importer);
    free(source);
    return 1;
  }

  char boundary_error[256] = {0};
  if (!validate_boundary_bits("--input", input_bits, importer.nqubits, boundary_error,
                              sizeof(boundary_error)) ||
      !validate_boundary_bits("--output", output_bits, importer.nqubits, boundary_error,
                              sizeof(boundary_error))) {
    fprintf(stderr, "error: %s\n", boundary_error);
    free_importer(&importer);
    free(source);
    return 2;
  }

  /* The affine representation is usually much smaller, but a few circuits happen to expose
   * more degree-2 eliminations in the former H-CZ-H topology. Build that candidate at the final
   * modulus too, then retain it whenever the affine result regresses canonical size or (for
   * bounded graphs) min-fill cost. This makes the optimization monotone relative to the old
   * importer without running expensive diagnostics on giant instances. */
  qasm_importer_t permutation = {
      .input_bits = input_bits,
      .output_bits = output_bits,
      .modulus = importer.modulus,
      .approx_enabled = importer.approx_enabled,
      .approx_epsilon = importer.approx_epsilon,
      .permutation_lowering = true,
  };
  bool have_permutation = false;
  FILE *permutation_input = fmemopen(source, source_len, "rb");
  if (permutation_input != NULL) {
    have_permutation = parse_qasm(permutation_input, diagnostic_path, &permutation);
    fclose(permutation_input);
    if (have_permutation && permutation.nqubits != importer.nqubits) {
      have_permutation = false;
    }
  }

  qsop_error_t error = {0};
  qsop_instance_t *affine_qsop = NULL;
  qsop_instance_t *permutation_qsop = NULL;
  ok = canonicalize_importer(&importer, &error, optimize, &affine_qsop);
  if (ok && have_permutation) {
    qsop_error_t permutation_error = {0};
    if (!canonicalize_importer(&permutation, &permutation_error, optimize, &permutation_qsop)) {
      qsop_free(permutation_qsop);
      permutation_qsop = NULL;
    }
  }

  if (ok) {
    const bool use_permutation =
        permutation_qsop != NULL && prefer_permutation_candidate(affine_qsop, permutation_qsop);
    const qasm_importer_t *certificate = use_permutation ? &permutation : &importer;
    const qsop_instance_t *chosen = use_permutation ? permutation_qsop : affine_qsop;
    write_approx_certificate(stdout, certificate);
    ok = qsop_write_file(stdout, chosen, &error);
  }

  qsop_free(affine_qsop);
  qsop_free(permutation_qsop);
  free_importer(&permutation);
  free_importer(&importer);
  free(source);
  if (!ok) {
    print_qsop_error(&error);
    return 1;
  }
  return 0;
}
