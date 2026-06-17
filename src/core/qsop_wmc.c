#include "dlx4sop/qsop_wmc.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Residue-counting CNF export.
 *
 * We build a CNF over the QSOP free variables x_1..x_nvars (DIMACS var v+1 for
 * QSOP variable v). A modular adder computes
 *   S = (constant + sum_v unary[v]*x_v + sum_e edge_q[e]*y_e) mod r,
 * where y_e <-> x_u AND x_v (full Tseitin biconditional, so every auxiliary is
 * functionally determined and does not inflate the model count). The final
 * accumulator bits are then constrained to a target residue k with w unit
 * clauses. A plain #SAT count of the result equals counts[k].
 *
 * The accumulator is kept reduced in [0, r) with w = ceil(log2 r) bits. Each
 * addend conditionally adds a compile-time constant c gated by an activator
 * literal a (raw = IN + (a ? c : 0), raw < 2r), then a single conditional
 * subtraction of r via two's-complement add brings it back into [0, r).
 */

#define WMC_MAX_WIDTH 31U /* r must fit in 31 bits so 1<<(w+1) stays in uint64 */

typedef struct wmc_builder {
  int *lits;         /* base clauses, each terminated by a 0 literal */
  size_t len;        /* used entries in lits */
  size_t cap;        /* capacity of lits */
  uint32_t nvars;    /* highest DIMACS variable allocated */
  uint64_t nclauses; /* number of base clauses */
  bool failed;       /* sticky out-of-memory flag */
} wmc_builder_t;

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

static void builder_free(wmc_builder_t *b) {
  if (b == NULL) {
    return;
  }
  free(b->lits);
  b->lits = NULL;
  b->len = 0;
  b->cap = 0;
}

static void builder_push(wmc_builder_t *b, int lit) {
  if (b->failed) {
    return;
  }
  if (b->len == b->cap) {
    const size_t cap = b->cap < 64 ? 64 : b->cap * 2;
    int *grown = realloc(b->lits, cap * sizeof(*grown));
    if (grown == NULL) {
      b->failed = true;
      return;
    }
    b->lits = grown;
    b->cap = cap;
  }
  b->lits[b->len++] = lit;
}

static void add_clause1(wmc_builder_t *b, int a) {
  builder_push(b, a);
  builder_push(b, 0);
  b->nclauses++;
}

static void add_clause2(wmc_builder_t *b, int a, int c) {
  builder_push(b, a);
  builder_push(b, c);
  builder_push(b, 0);
  b->nclauses++;
}

static void add_clause3(wmc_builder_t *b, int a, int c, int d) {
  builder_push(b, a);
  builder_push(b, c);
  builder_push(b, d);
  builder_push(b, 0);
  b->nclauses++;
}

static int new_var(wmc_builder_t *b) {
  b->nvars += 1U;
  return (int)b->nvars;
}

/* z <-> (p AND q) */
static int gate_and(wmc_builder_t *b, int p, int q) {
  const int z = new_var(b);
  add_clause2(b, -z, p);
  add_clause2(b, -z, q);
  add_clause3(b, z, -p, -q);
  return z;
}

/* z <-> (p OR q) */
static int gate_or(wmc_builder_t *b, int p, int q) {
  const int z = new_var(b);
  add_clause2(b, z, -p);
  add_clause2(b, z, -q);
  add_clause3(b, -z, p, q);
  return z;
}

/* z <-> (p XOR q) */
static int gate_xor(wmc_builder_t *b, int p, int q) {
  const int z = new_var(b);
  add_clause3(b, p, q, -z);
  add_clause3(b, -p, -q, -z);
  add_clause3(b, p, -q, z);
  add_clause3(b, -p, q, z);
  return z;
}

/* z <-> (sel ? hi : lo) */
static int gate_mux(wmc_builder_t *b, int sel, int hi, int lo) {
  const int z = new_var(b);
  add_clause3(b, -sel, -hi, z);
  add_clause3(b, -sel, hi, -z);
  add_clause3(b, sel, -lo, z);
  add_clause3(b, sel, lo, -z);
  return z;
}

/* sum/carry for a + b + cin. */
static void full_adder(wmc_builder_t *b, int a, int bb, int cin, int *sum, int *cout) {
  const int t = gate_xor(b, a, bb);
  *sum = gate_xor(b, t, cin);
  const int aab = gate_and(b, a, bb);
  const int tac = gate_and(b, t, cin);
  *cout = gate_or(b, aab, tac);
}

/* Accumulator <- (accumulator + (activator ? coeff : 0)) mod r, in place. */
static void add_addend(wmc_builder_t *b, int *acc, uint32_t w, uint32_t r, uint64_t coeff,
                       int activator, int lit_true) {
  const int lit_false = -lit_true;
  int raw[WMC_MAX_WIDTH + 2];
  int carry = lit_false;
  for (uint32_t j = 0; j < w; j++) {
    const int bj = ((coeff >> j) & 1U) ? activator : lit_false;
    full_adder(b, acc[j], bj, carry, &raw[j], &carry);
  }
  raw[w] = carry; /* raw in [0, 2r-2] needs w+1 bits */

  /* Power-of-two modulus: the low w bits already hold (IN + addend) mod r, so
     drop the carry and skip the conditional subtraction entirely. This keeps
     the CNF small, which matters a lot for the downstream model counter. */
  if ((r & (r - 1U)) == 0U) {
    for (uint32_t j = 0; j < w; j++) {
      acc[j] = raw[j];
    }
    return;
  }

  /* Conditional subtract r: add (2^(w+1) - r); the carry-out means raw >= r. */
  const uint64_t comp = ((uint64_t)1 << (w + 1U)) - (uint64_t)r;
  int sub[WMC_MAX_WIDTH + 2];
  carry = lit_false;
  for (uint32_t j = 0; j <= w; j++) {
    const int cj = ((comp >> j) & 1U) ? lit_true : lit_false;
    int unused = 0;
    full_adder(b, raw[j], cj, carry, (j < w) ? &sub[j] : &unused, &carry);
  }
  const int ge = carry; /* raw >= r */
  for (uint32_t j = 0; j < w; j++) {
    acc[j] = gate_mux(b, ge, sub[j], raw[j]);
  }
}

static uint32_t width_for_modulus(uint32_t r) {
  uint32_t w = 0;
  while (((uint64_t)1 << w) < (uint64_t)r) {
    w += 1U;
  }
  return w == 0 ? 1U : w;
}

/* Build the shared CNF; fills afinal[0..w-1] (LSB first) and *w_out. */
static bool build_shared(const qsop_instance_t *qsop, wmc_builder_t *b, int *afinal,
                         uint32_t *w_out, qsop_error_t *error) {
  const uint32_t r = qsop->r;
  const uint32_t w = width_for_modulus(r);
  *w_out = w;

  b->nvars = qsop->nvars; /* x_v == DIMACS var v+1 */
  const int lit_true = new_var(b);
  add_clause1(b, lit_true); /* pin the constant-true literal */

  const uint32_t c0 = qsop->constant % r;
  for (uint32_t j = 0; j < w; j++) {
    afinal[j] = ((c0 >> j) & 1U) ? lit_true : -lit_true;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    const uint64_t coeff = qsop->unary[v] % r;
    if (coeff != 0U) {
      add_addend(b, afinal, w, r, coeff, (int)(v + 1U), lit_true);
    }
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint64_t coeff = qsop->edge_q[e] % r;
    if (coeff != 0U) {
      const int y = gate_and(b, (int)(qsop->edge_u[e] + 1U), (int)(qsop->edge_v[e] + 1U));
      add_addend(b, afinal, w, r, coeff, y, lit_true);
    }
  }

  if (b->failed) {
    set_error(error, "out of memory while building WMC CNF");
    return false;
  }
  return true;
}

static bool write_metadata(FILE *file, const qsop_instance_t *qsop, uint32_t w,
                           const int *afinal, uint32_t residue) {
  fprintf(file,
          "c sop2wmc encoding=residue r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
          " mode=%s norm_h=%" PRIu64 "\n",
          qsop->r, qsop->nvars, qsop->nedges, qsop_mode_name(qsop->mode), qsop->norm_h);
  fprintf(file, "c residue %" PRIu32 " width %" PRIu32 "\n", residue, w);
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    fprintf(file, "c xvar %" PRIu32 " %" PRIu32 "\n", v, v + 1U);
  }
  for (uint32_t j = 0; j < w; j++) {
    fprintf(file, "c afinal %" PRIu32 " %d\n", j, afinal[j]);
  }
  fputs("c amplitude = sum_k counts[k] * exp(2*pi*i*k/r)\n", file);
  fputs("c probability = |amplitude|^2 * 2^(-norm_h)\n", file);
  return ferror(file) == 0;
}

static bool write_block(FILE *file, const wmc_builder_t *b, const qsop_instance_t *qsop,
                        uint32_t w, const int *afinal, uint32_t residue, bool metadata,
                        bool separator, qsop_error_t *error) {
  if (separator) {
    fprintf(file, "c --- residue %" PRIu32 " ---\n", residue);
  }
  if (metadata && !write_metadata(file, qsop, w, afinal, residue)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b->nvars, b->nclauses + w);
  for (size_t i = 0; i < b->len; i++) {
    if (b->lits[i] == 0) {
      fputs("0\n", file);
    } else {
      fprintf(file, "%d ", b->lits[i]);
    }
  }
  for (uint32_t j = 0; j < w; j++) {
    const int lit = ((residue >> j) & 1U) ? afinal[j] : -afinal[j];
    fprintf(file, "%d 0\n", lit);
  }
  if (ferror(file)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

qsop_wmc_options_t qsop_wmc_options_default(void) {
  qsop_wmc_options_t options = {0};
  options.encoding = QSOP_WMC_ENCODING_RESIDUE;
  options.all_residues = true;
  options.residue = 0;
  options.emit_metadata = true;
  options.stats_out = NULL;
  return options;
}

/* omega^k = exp(2*pi*i*k/r) */
static void omega_power(uint32_t k, uint32_t r, double *re, double *im) {
  const double angle = 2.0 * M_PI * (double)k / (double)r;
  *re = cos(angle);
  *im = sin(angle);
}

/* Write one complex literal weight line for var and its negation. */
static void write_weight(FILE *file, int var, double re, double im) {
  fprintf(file, "c p weight %d %.17g+%.17gi 0\n", var, re, im);
  fprintf(file, "c p weight %d 1+0i 0\n", -var);
}

/*
 * Export-only factor graph (WMC domain): represents the amplitude integrand
 * Z_t = global * prod_v (w_false[v] + w_true[v]*x_v) * prod_{(u,v)} (1 + (R_uv-1)*x_u*x_v)
 * where R_uv is the active pair multiplier when x_u = x_v = 1.
 *
 * This is strictly internal to qsop_wmc.c and never touches the SOP core.
 */
typedef struct wmc_pair {
  uint32_t u;
  uint32_t v;
  double R_re;  /* active multiplier when x_u = x_v = 1: omega^b */
  double R_im;
} wmc_pair_t;

typedef struct wmc_factor_graph {
  uint32_t nvars;
  double global_re;  /* omega^c0 */
  double global_im;
  double *w_true_re; /* w_true[v] = omega^a_v (length nvars) */
  double *w_true_im;
  wmc_pair_t *pairs; /* pairs with R_uv != 1 (i.e., coeff != 0 mod r) */
  size_t npairs;
} wmc_factor_graph_t;

static void fg_free(wmc_factor_graph_t *fg) {
  if (fg == NULL) {
    return;
  }
  free(fg->w_true_re);
  free(fg->w_true_im);
  free(fg->pairs);
  fg->w_true_re = NULL;
  fg->w_true_im = NULL;
  fg->pairs = NULL;
}

/* Build a factor graph from a labelled quadratic SOP for Fourier exponent t=1. */
static bool fg_from_qsop(const qsop_instance_t *qsop, wmc_factor_graph_t *fg,
                          qsop_error_t *error) {
  const uint32_t r = qsop->r;
  *fg = (wmc_factor_graph_t){0};
  fg->nvars = qsop->nvars;

  /* Global factor: omega^c0. */
  omega_power((uint32_t)(qsop->constant % r), r, &fg->global_re, &fg->global_im);

  /* Per-variable true-weights: omega^a_v. w_false[v] = 1 (implicit). */
  if (qsop->nvars > 0) {
    fg->w_true_re = malloc(qsop->nvars * sizeof(*fg->w_true_re));
    fg->w_true_im = malloc(qsop->nvars * sizeof(*fg->w_true_im));
    if (fg->w_true_re == NULL || fg->w_true_im == NULL) {
      set_error(error, "out of memory building WMC factor graph");
      fg_free(fg);
      return false;
    }
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      omega_power((uint32_t)(qsop->unary[v] % r), r, &fg->w_true_re[v], &fg->w_true_im[v]);
    }
  }

  /* Pair factors: only edges whose label mod r is non-zero (R_uv != 1). */
  if (qsop->nedges > 0) {
    fg->pairs = malloc(qsop->nedges * sizeof(*fg->pairs));
    if (fg->pairs == NULL) {
      set_error(error, "out of memory building WMC factor graph");
      fg_free(fg);
      return false;
    }
    fg->npairs = 0;
    for (uint32_t e = 0; e < qsop->nedges; e++) {
      const uint32_t coeff = (uint32_t)(qsop->edge_q[e] % r);
      if (coeff == 0U) {
        continue;
      }
      wmc_pair_t *p = &fg->pairs[fg->npairs++];
      p->u = qsop->edge_u[e];
      p->v = qsop->edge_v[e];
      omega_power(coeff, r, &p->R_re, &p->R_im);
    }
  }

  return true;
}

/* Shared metadata writer for amplitude encodings. */
static void write_amp_metadata(FILE *file, const qsop_instance_t *qsop,
                                const wmc_factor_graph_t *fg, const char *encoding_tag) {
  const uint32_t r = qsop->r;
  const uint32_t c0 = (uint32_t)(qsop->constant % r);
  fprintf(file,
          "c sop2wmc encoding=%s r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
          " mode=%s norm_h=%" PRIu64 "\n",
          encoding_tag, r, qsop->nvars, qsop->nedges, qsop_mode_name(qsop->mode), qsop->norm_h);
  fprintf(file, "c constant_phase %" PRIu32 "\n", c0);
  fprintf(file, "c amplitude_factor %.17g+%.17gi\n", fg->global_re, fg->global_im);
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    fprintf(file, "c xvar %" PRIu32 " %" PRIu32 "\n", v, v + 1U);
  }
  fputs("c amplitude = ganak_output * amplitude_factor\n", file);
  fputs("c probability = |amplitude|^2 * 2^(-norm_h)\n", file);
  fputs("c invoke: ganak --mode 6 --verb 0 <this-file>\n", file);
}

/* amp-and: Tseitin AND auxiliary per encoded edge.
 * W(y=1) = omega^b, W(y=0) = 1. Three clauses per edge (2 binary + 1 ternary). */
static bool write_amplitude(FILE *file, const qsop_instance_t *qsop,
                            const wmc_factor_graph_t *fg, bool emit_metadata,
                            qsop_wmc_stats_t *stats_out, qsop_error_t *error) {
  /* Build Tseitin AND clauses for each pair in the factor graph. */
  wmc_builder_t b = {0};
  b.nvars = fg->nvars;

  int *pair_var = NULL;
  if (fg->npairs > 0) {
    pair_var = calloc(fg->npairs, sizeof(*pair_var));
    if (pair_var == NULL) {
      set_error(error, "out of memory while building amplitude CNF");
      return false;
    }
    for (size_t p = 0; p < fg->npairs; p++) {
      pair_var[p] = gate_and(&b, (int)(fg->pairs[p].u + 1U), (int)(fg->pairs[p].v + 1U));
    }
  }
  if (b.failed) {
    set_error(error, "out of memory while building amplitude CNF");
    free(pair_var);
    builder_free(&b);
    return false;
  }

  const uint32_t encoded_edges = (uint32_t)fg->npairs;
  const uint32_t skipped_edges = qsop->nedges - encoded_edges;

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b.nvars, b.nclauses);
  if (emit_metadata) {
    write_amp_metadata(file, qsop, fg, "amp-and");
  }

  /* Literal weights for free variables with non-trivial true-weight (omega^a_v != 1). */
  for (uint32_t v = 0; v < fg->nvars; v++) {
    const double re = fg->w_true_re[v];
    const double im = fg->w_true_im[v];
    if (re * re + im * im < (1.0 - 1e-12) * (1.0 - 1e-12) ||
        re * re + im * im > (1.0 + 1e-12) * (1.0 + 1e-12) || fabs(im) > 1e-12 ||
        fabs(re - 1.0) > 1e-12) {
      write_weight(file, (int)(v + 1U), re, im);
    }
  }

  /* Literal weights for Tseitin AND variables: W(y=1) = R_uv, W(y=0) = 1. */
  for (size_t p = 0; p < fg->npairs; p++) {
    write_weight(file, pair_var[p], fg->pairs[p].R_re, fg->pairs[p].R_im);
  }

  /* Buffered Tseitin biconditional clauses. */
  for (size_t i = 0; i < b.len; i++) {
    if (b.lits[i] == 0) {
      fputs("0\n", file);
    } else {
      fprintf(file, "%d ", b.lits[i]);
    }
  }

  if (stats_out != NULL) {
    stats_out->aux_vars = encoded_edges;
    stats_out->clauses_unit = 0;
    stats_out->clauses_binary = 2U * encoded_edges;
    stats_out->clauses_ternary = encoded_edges;
    stats_out->encoded_edges = encoded_edges;
    stats_out->skipped_edges = skipped_edges;
  }

  free(pair_var);
  builder_free(&b);
  if (ferror(file)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

/* amp-soft: y_e -> x_u and y_e -> x_v only (2 binary clauses per encoded edge,
 * no ternary). W(y_e=1) = R_uv - 1, W(y_e=0) = 1.
 * Correctness: when x_u=0 or x_v=0 the implications force y_e=0 (weight 1).
 * When x_u=x_v=1, y_e sums freely: 1 + (R_uv - 1) = R_uv. */
static bool write_amp_soft(FILE *file, const qsop_instance_t *qsop,
                            const wmc_factor_graph_t *fg, bool emit_metadata,
                            qsop_wmc_stats_t *stats_out, qsop_error_t *error) {
  wmc_builder_t b = {0};
  b.nvars = fg->nvars;

  int *pair_var = NULL;
  if (fg->npairs > 0) {
    pair_var = calloc(fg->npairs, sizeof(*pair_var));
    if (pair_var == NULL) {
      set_error(error, "out of memory while building amp-soft CNF");
      return false;
    }
    for (size_t p = 0; p < fg->npairs; p++) {
      const int y = new_var(&b);
      pair_var[p] = y;
      add_clause2(&b, -y, (int)(fg->pairs[p].u + 1U));  /* y -> x_u */
      add_clause2(&b, -y, (int)(fg->pairs[p].v + 1U));  /* y -> x_v */
    }
  }
  if (b.failed) {
    set_error(error, "out of memory while building amp-soft CNF");
    free(pair_var);
    builder_free(&b);
    return false;
  }

  const uint32_t encoded_edges = (uint32_t)fg->npairs;
  const uint32_t skipped_edges = qsop->nedges - encoded_edges;

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b.nvars, b.nclauses);
  if (emit_metadata) {
    write_amp_metadata(file, qsop, fg, "amp-soft");
  }

  /* Literal weights for free variables with non-trivial true-weight. */
  for (uint32_t v = 0; v < fg->nvars; v++) {
    const double re = fg->w_true_re[v];
    const double im = fg->w_true_im[v];
    if (fabs(re - 1.0) > 1e-12 || fabs(im) > 1e-12) {
      write_weight(file, (int)(v + 1U), re, im);
    }
  }

  /* Literal weights for soft auxiliaries: W(y=1) = R_uv - 1, W(y=0) = 1. */
  for (size_t p = 0; p < fg->npairs; p++) {
    write_weight(file, pair_var[p], fg->pairs[p].R_re - 1.0, fg->pairs[p].R_im);
  }

  /* Binary implication clauses — no ternary backward clauses. */
  for (size_t i = 0; i < b.len; i++) {
    if (b.lits[i] == 0) {
      fputs("0\n", file);
    } else {
      fprintf(file, "%d ", b.lits[i]);
    }
  }

  if (stats_out != NULL) {
    stats_out->aux_vars = encoded_edges;
    stats_out->clauses_unit = 0;
    stats_out->clauses_binary = 2U * encoded_edges;
    stats_out->clauses_ternary = 0;
    stats_out->encoded_edges = encoded_edges;
    stats_out->skipped_edges = skipped_edges;
  }

  free(pair_var);
  builder_free(&b);
  if (ferror(file)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

bool qsop_wmc_write(FILE *file, const qsop_instance_t *qsop, const qsop_wmc_options_t *options,
                    qsop_error_t *error) {
  if (file == NULL || qsop == NULL || options == NULL) {
    set_error(error, "internal error: null WMC write argument");
    return false;
  }
  if (qsop->r < 2U || (qsop->r % 2U) != 0U) {
    set_error(error, "WMC export requires a positive even modulus, got %" PRIu32, qsop->r);
    return false;
  }

  if (options->encoding == QSOP_WMC_ENCODING_AMPLITUDE ||
      options->encoding == QSOP_WMC_ENCODING_AMP_SOFT) {
    wmc_factor_graph_t fg = {0};
    if (!fg_from_qsop(qsop, &fg, error)) {
      return false;
    }
    bool ok;
    if (options->encoding == QSOP_WMC_ENCODING_AMPLITUDE) {
      ok = write_amplitude(file, qsop, &fg, options->emit_metadata, options->stats_out, error);
    } else {
      ok = write_amp_soft(file, qsop, &fg, options->emit_metadata, options->stats_out, error);
    }
    fg_free(&fg);
    return ok;
  }

  /* RESIDUE encoding. */
  if (width_for_modulus(qsop->r) > WMC_MAX_WIDTH) {
    set_error(error, "WMC export modulus %" PRIu32 " is too large", qsop->r);
    return false;
  }
  if (!options->all_residues && options->residue >= qsop->r) {
    set_error(error, "residue %" PRIu32 " is out of range for modulus %" PRIu32, options->residue,
              qsop->r);
    return false;
  }

  wmc_builder_t builder = {0};
  int afinal[WMC_MAX_WIDTH + 1];
  uint32_t w = 0;
  if (!build_shared(qsop, &builder, afinal, &w, error)) {
    builder_free(&builder);
    return false;
  }

  bool ok = true;
  if (options->all_residues) {
    for (uint32_t k = 0; ok && k < qsop->r; k++) {
      ok = write_block(file, &builder, qsop, w, afinal, k, options->emit_metadata, true, error);
    }
  } else {
    ok = write_block(file, &builder, qsop, w, afinal, options->residue, options->emit_metadata,
                     false, error);
  }

  builder_free(&builder);
  return ok;
}
