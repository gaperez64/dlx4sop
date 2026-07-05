#include "dlx4sop/qsop_wmc.h"
#include "dlx4sop/bitset.h"

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
 *   S = (constant + sum_v unary[v]*x_v + (r/2)*sum_e y_e) mod r,
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
#define WMC_ROOT_CACHE_MAX 256U

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

/* Build the shared CNF; fills afinal[0..w-1] (LSB first) and *w_out.
 * Gated by qsop_wmc_write's RESIDUE-encoding dispatch to qsop->r <= UINT32_MAX (in fact
 * < 2^WMC_MAX_WIDTH) before reaching this count-table-style encoding. */
static bool build_shared(const qsop_instance_t *qsop, wmc_builder_t *b, int *afinal,
                         uint32_t *w_out, qsop_error_t *error) {
  const uint32_t r = (uint32_t)qsop->r;
  const uint32_t w = width_for_modulus(r);
  *w_out = w;

  b->nvars = qsop->nvars; /* x_v == DIMACS var v+1 */
  const int lit_true = new_var(b);
  add_clause1(b, lit_true); /* pin the constant-true literal */

  const uint32_t c0 = (uint32_t)(qsop->constant % r);
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
    const uint64_t coeff = r / 2U;
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

/* Gated by qsop_wmc_write's RESIDUE-encoding dispatch to qsop->r <= UINT32_MAX (in fact
 * < 2^WMC_MAX_WIDTH) before reaching this count-table-style encoding. */
static bool write_metadata(FILE *file, const qsop_instance_t *qsop, uint32_t w,
                           const int *afinal, uint32_t residue) {
  fprintf(file,
          "c sop2wmc encoding=residue r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
          " format=qsop-sign norm_h=%" PRIu64 "\n",
          (uint32_t)qsop->r, qsop->nvars, qsop->nedges, qsop->norm_h);
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
  options.preprocess = QSOP_WMC_PREPROCESS_NONE;
  options.peel2_fill_budget = 0;
  options.fourier_inner = QSOP_WMC_ENCODING_AMP_SOFT;
  options.fourier_all_modes = true;
  options.fourier_mode = 0;
  options.stats_out = NULL;
  options.block_min_side = 4;
  options.block_min_savings = 0;
  return options;
}

/* omega^k = exp(2*pi*i*k/r); r never sizes an array here, only a divisor, so this is safe
 * for any r that fits in a double's mantissa without needing a modulus ceiling. */
static void omega_power(uint64_t k, uint64_t r, double *re, double *im) {
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
 * var_active and pair_active are set by preprocessing (peel1/peel2-safe).
 */
typedef struct wmc_pair {
  uint32_t u;
  uint32_t v;
  double R_re;  /* active multiplier when x_u = x_v = 1: omega^b */
  double R_im;
} wmc_pair_t;

typedef struct wmc_factor_graph {
  uint32_t nvars;
  double global_re;   /* accumulated global complex factor */
  double global_im;
  double *w_true_re;  /* w_true[v] = omega^a_v, modified by preprocessing (length nvars) */
  double *w_true_im;
  wmc_pair_t *pairs;  /* pairs with R_uv != 1 at construction time (length npairs_alloc) */
  size_t npairs;      /* original pair count (includes deactivated pairs) */
  size_t npairs_cap;
  bool *var_active;   /* length nvars: variable remains in the WPCNF after preprocessing */
  int8_t *var_forced; /* length nvars: 0=not forced, +1=force true, -1=force false */
  bool *pair_active;  /* length npairs: pair remains in the WPCNF after preprocessing */
  bool is_zero;       /* amplitude is analytically zero; skip WPCNF entirely */
} wmc_factor_graph_t;

#define FG_CMAG2(re, im) ((re) * (re) + (im) * (im))
#define FG_ZERO_EPS 1e-12

static void fg_free(wmc_factor_graph_t *fg) {
  if (fg == NULL) {
    return;
  }
  free(fg->w_true_re);
  free(fg->w_true_im);
  free(fg->pairs);
  free(fg->var_active);
  free(fg->var_forced);
  free(fg->pair_active);
  fg->w_true_re = NULL;
  fg->w_true_im = NULL;
  fg->pairs = NULL;
  fg->var_active = NULL;
  fg->var_forced = NULL;
  fg->pair_active = NULL;
  fg->npairs = 0;
  fg->npairs_cap = 0;
}

static bool complex_near(double re, double im, double target_re, double target_im) {
  return fabs(re - target_re) <= FG_ZERO_EPS && fabs(im - target_im) <= FG_ZERO_EPS;
}

static bool fg_reserve_pairs(wmc_factor_graph_t *fg, size_t needed, qsop_error_t *error) {
  if (needed <= fg->npairs_cap) {
    return true;
  }
  size_t new_cap = fg->npairs_cap == 0 ? 8U : fg->npairs_cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "WMC factor graph pair table is too large");
      return false;
    }
    new_cap *= 2U;
  }
  wmc_pair_t *pairs = calloc(new_cap, sizeof(*pairs));
  bool *pair_active = calloc(new_cap, sizeof(*pair_active));
  if (pairs == NULL || pair_active == NULL) {
    free(pairs);
    free(pair_active);
    set_error(error, "out of memory growing WMC factor graph");
    return false;
  }
  if (fg->npairs != 0) {
    memcpy(pairs, fg->pairs, fg->npairs * sizeof(*pairs));
    memcpy(pair_active, fg->pair_active, fg->npairs * sizeof(*pair_active));
  }
  free(fg->pairs);
  free(fg->pair_active);
  fg->pairs = pairs;
  fg->pair_active = pair_active;
  fg->npairs_cap = new_cap;
  return true;
}

static bool fg_find_active_pair(const wmc_factor_graph_t *fg, uint32_t u, uint32_t v,
                                size_t *out) {
  if (u > v) {
    const uint32_t tmp = u;
    u = v;
    v = tmp;
  }
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    const uint32_t a = fg->pairs[p].u < fg->pairs[p].v ? fg->pairs[p].u : fg->pairs[p].v;
    const uint32_t b = fg->pairs[p].u < fg->pairs[p].v ? fg->pairs[p].v : fg->pairs[p].u;
    if (a == u && b == v) {
      if (out != NULL) {
        *out = p;
      }
      return true;
    }
  }
  return false;
}

static bool fg_multiply_pair(wmc_factor_graph_t *fg, uint32_t u, uint32_t v,
                             double r_re, double r_im, qsop_error_t *error) {
  if (u == v || complex_near(r_re, r_im, 1.0, 0.0)) {
    return true;
  }
  size_t existing = 0;
  if (fg_find_active_pair(fg, u, v, &existing)) {
    const double old_re = fg->pairs[existing].R_re;
    const double old_im = fg->pairs[existing].R_im;
    const double next_re = old_re * r_re - old_im * r_im;
    const double next_im = old_re * r_im + old_im * r_re;
    if (complex_near(next_re, next_im, 1.0, 0.0)) {
      fg->pair_active[existing] = false;
    } else {
      fg->pairs[existing].R_re = next_re;
      fg->pairs[existing].R_im = next_im;
    }
    return true;
  }
  if (!fg_reserve_pairs(fg, fg->npairs + 1U, error)) {
    return false;
  }
  fg->pairs[fg->npairs] = (wmc_pair_t){
      .u = u < v ? u : v,
      .v = u < v ? v : u,
      .R_re = r_re,
      .R_im = r_im,
  };
  fg->pair_active[fg->npairs] = true;
  fg->npairs++;
  return true;
}

/* Build a factor graph for Fourier exponent t: weights become omega^(t*label mod r). r never
 * sizes an allocation here (only nvars/nedges do), so this path is O(1) in r and needs no
 * modulus ceiling -- unlike the RESIDUE/RESIDUE_FOURIER(all-modes) encodings below. */
static bool fg_from_qsop(const qsop_instance_t *qsop, uint32_t t, wmc_factor_graph_t *fg,
                          qsop_error_t *error) {
  const uint64_t r = qsop->r;
  *fg = (wmc_factor_graph_t){0};
  fg->nvars = qsop->nvars;

  double root_re[WMC_ROOT_CACHE_MAX];
  double root_im[WMC_ROOT_CACHE_MAX];
  const bool cache_roots = r <= WMC_ROOT_CACHE_MAX;
  if (cache_roots) {
    for (uint64_t k = 0; k < r; k++) {
      omega_power(k, r, &root_re[k], &root_im[k]);
    }
  }

  /* Global factor: omega^(t*c0 mod r). */
  const uint64_t global_coeff = ((uint64_t)t * (qsop->constant % r)) % r;
  if (cache_roots) {
    fg->global_re = root_re[global_coeff];
    fg->global_im = root_im[global_coeff];
  } else {
    omega_power(global_coeff, r, &fg->global_re, &fg->global_im);
  }

  /* Per-variable true-weights: omega^(t*a_v mod r). w_false[v] = 1 (implicit). */
  if (qsop->nvars > 0) {
    fg->w_true_re = malloc(qsop->nvars * sizeof(*fg->w_true_re));
    fg->w_true_im = malloc(qsop->nvars * sizeof(*fg->w_true_im));
    fg->var_active = malloc(qsop->nvars * sizeof(*fg->var_active));
    fg->var_forced = calloc(qsop->nvars, sizeof(*fg->var_forced));
    if (!fg->w_true_re || !fg->w_true_im || !fg->var_active || !fg->var_forced) {
      set_error(error, "out of memory building WMC factor graph");
      fg_free(fg);
      return false;
    }
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      const uint64_t coeff = ((uint64_t)t * (qsop->unary[v] % r)) % r;
      if (cache_roots) {
        fg->w_true_re[v] = root_re[coeff];
        fg->w_true_im[v] = root_im[coeff];
      } else {
        omega_power(coeff, r, &fg->w_true_re[v], &fg->w_true_im[v]);
      }
      fg->var_active[v] = true;
    }
  }

  /* Pair factors: only odd Fourier modes see sign edges; even modes have multiplier 1. */
  if (qsop->nedges > 0) {
    if ((t & 1U) == 0U) {
      return true;
    }
    if (!fg_reserve_pairs(fg, qsop->nedges, error)) {
      fg_free(fg);
      return false;
    }
    for (uint32_t e = 0; e < qsop->nedges; e++) {
      wmc_pair_t *p = &fg->pairs[fg->npairs];
      p->u = qsop->edge_u[e];
      p->v = qsop->edge_v[e];
      p->R_re = -1.0;
      p->R_im = 0.0;
      fg->pair_active[fg->npairs] = true;
      fg->npairs++;
    }
  }

  return true;
}

/* Complex multiply a *= b (in-place). */
static void cmul_ip(double *a_re, double *a_im, double b_re, double b_im) {
  const double re = *a_re * b_re - *a_im * b_im;
  const double im = *a_re * b_im + *a_im * b_re;
  *a_re = re;
  *a_im = im;
}

static void fg_peel1(wmc_factor_graph_t *fg);

/* Complex multiply-then-divide: a *= (b / c) in-place. Assumes |c| > 0. */
static void cmul_div_ip(double *a_re, double *a_im, double b_re, double b_im, double c_re,
                        double c_im) {
  const double mag2 = FG_CMAG2(c_re, c_im);
  /* a *= b * conj(c) / |c|^2 */
  const double bc_re = b_re * c_re + b_im * c_im;  /* Re(b * conj(c)) */
  const double bc_im = b_im * c_re - b_re * c_im;  /* Im(b * conj(c)) */
  const double ab_re = *a_re * bc_re - *a_im * bc_im;
  const double ab_im = *a_re * bc_im + *a_im * bc_re;
  *a_re = ab_re / mag2;
  *a_im = ab_im / mag2;
}

static bool cdiv(double a_re, double a_im, double b_re, double b_im,
                 double *out_re, double *out_im) {
  const double mag2 = FG_CMAG2(b_re, b_im);
  if (mag2 < FG_ZERO_EPS * FG_ZERO_EPS) {
    return false;
  }
  *out_re = (a_re * b_re + a_im * b_im) / mag2;
  *out_im = (a_im * b_re - a_re * b_im) / mag2;
  return true;
}

static void fg_degree2_record(uint32_t var, uint32_t neighbor, size_t pair_index,
                              uint32_t *degree, size_t *pair_idx, uint32_t *nbr) {
  const uint32_t deg = degree[var];
  if (deg < 2U) {
    const size_t slot = 2U * (size_t)var + deg;
    pair_idx[slot] = pair_index;
    nbr[slot] = neighbor;
    degree[var] = deg + 1U;
  } else if (deg == 2U) {
    degree[var] = 3U;
  }
}

static bool fg_peel2_once(wmc_factor_graph_t *fg, uint32_t *fill_used,
                          uint32_t fill_budget, bool *changed_out,
                          qsop_error_t *error) {
  *changed_out = false;
  const uint32_t n = fg->nvars;
  if (n == 0) {
    return true;
  }

  uint32_t *degree = calloc(n, sizeof(*degree));
  size_t *pair_idx = calloc(2U * (size_t)n, sizeof(*pair_idx));
  uint32_t *nbr = calloc(2U * (size_t)n, sizeof(*nbr));
  if (degree == NULL || pair_idx == NULL || nbr == NULL) {
    free(degree);
    free(pair_idx);
    free(nbr);
    set_error(error, "out of memory while indexing WMC peel2 degrees");
    return false;
  }

  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    const uint32_t u = fg->pairs[p].u;
    const uint32_t v = fg->pairs[p].v;
    if (fg->var_active[u]) {
      fg_degree2_record(u, v, p, degree, pair_idx, nbr);
    }
    if (fg->var_active[v]) {
      fg_degree2_record(v, u, p, degree, pair_idx, nbr);
    }
  }

  for (uint32_t v = 0; v < fg->nvars && !fg->is_zero; v++) {
    if (!fg->var_active[v] || fg->var_forced[v] != 0) {
      continue;
    }

    if (degree[v] != 2U) {
      continue;
    }
    const size_t pair0 = pair_idx[2U * (size_t)v];
    const size_t pair1 = pair_idx[2U * (size_t)v + 1U];
    const uint32_t nbr0 = nbr[2U * (size_t)v];
    const uint32_t nbr1 = nbr[2U * (size_t)v + 1U];
    if (nbr0 == nbr1 || !fg->var_active[nbr0] || !fg->var_active[nbr1]) {
      continue;
    }

    double u_re = fg->w_true_re[v];
    double u_im = fg->w_true_im[v];
    const wmc_pair_t *p0 = &fg->pairs[pair0];
    const wmc_pair_t *p1 = &fg->pairs[pair1];

    const double r0_re = p0->R_re;
    const double r0_im = p0->R_im;
    const double r1_re = p1->R_re;
    const double r1_im = p1->R_im;

    const double ur0_re = u_re * r0_re - u_im * r0_im;
    const double ur0_im = u_re * r0_im + u_im * r0_re;
    const double ur1_re = u_re * r1_re - u_im * r1_im;
    const double ur1_im = u_re * r1_im + u_im * r1_re;
    const double r01_re = r0_re * r1_re - r0_im * r1_im;
    const double r01_im = r0_re * r1_im + r0_im * r1_re;
    const double ur01_re = u_re * r01_re - u_im * r01_im;
    const double ur01_im = u_re * r01_im + u_im * r01_re;

    const double f00_re = 1.0 + u_re;
    const double f00_im = u_im;
    const double f10_re = 1.0 + ur0_re;
    const double f10_im = ur0_im;
    const double f01_re = 1.0 + ur1_re;
    const double f01_im = ur1_im;
    const double f11_re = 1.0 + ur01_re;
    const double f11_im = ur01_im;

    if (FG_CMAG2(f00_re, f00_im) < FG_ZERO_EPS * FG_ZERO_EPS ||
        FG_CMAG2(f10_re, f10_im) < FG_ZERO_EPS * FG_ZERO_EPS ||
        FG_CMAG2(f01_re, f01_im) < FG_ZERO_EPS * FG_ZERO_EPS) {
      continue;
    }

    double wy_re = 0.0, wy_im = 0.0;
    double wz_re = 0.0, wz_im = 0.0;
    if (!cdiv(f10_re, f10_im, f00_re, f00_im, &wy_re, &wy_im) ||
        !cdiv(f01_re, f01_im, f00_re, f00_im, &wz_re, &wz_im)) {
      continue;
    }

    const double numerator_re = f11_re * f00_re - f11_im * f00_im;
    const double numerator_im = f11_re * f00_im + f11_im * f00_re;
    const double denominator_re = f10_re * f01_re - f10_im * f01_im;
    const double denominator_im = f10_re * f01_im + f10_im * f01_re;
    double pair_re = 0.0, pair_im = 0.0;
    if (!cdiv(numerator_re, numerator_im, denominator_re, denominator_im,
              &pair_re, &pair_im)) {
      continue;
    }

    const bool creates_fill =
        !complex_near(pair_re, pair_im, 1.0, 0.0) &&
        !fg_find_active_pair(fg, nbr0, nbr1, NULL);
    if (creates_fill && fill_budget != 0 && *fill_used >= fill_budget) {
      continue;
    }

    cmul_ip(&fg->global_re, &fg->global_im, f00_re, f00_im);
    cmul_ip(&fg->w_true_re[nbr0], &fg->w_true_im[nbr0], wy_re, wy_im);
    cmul_ip(&fg->w_true_re[nbr1], &fg->w_true_im[nbr1], wz_re, wz_im);
    fg->pair_active[pair0] = false;
    fg->pair_active[pair1] = false;
    fg->var_active[v] = false;
    if (creates_fill) {
      (*fill_used)++;
    }
    if (!fg_multiply_pair(fg, nbr0, nbr1, pair_re, pair_im, error)) {
      free(degree);
      free(pair_idx);
      free(nbr);
      return false;
    }
    *changed_out = true;
    free(degree);
    free(pair_idx);
    free(nbr);
    return true;
  }
  free(degree);
  free(pair_idx);
  free(nbr);
  return true;
}

static bool fg_preprocess(wmc_factor_graph_t *fg, qsop_wmc_preprocess_t preprocess,
                          uint32_t peel2_fill_budget, qsop_error_t *error) {
  if (preprocess == QSOP_WMC_PREPROCESS_NONE) {
    return true;
  }
  fg_peel1(fg);
  if (preprocess != QSOP_WMC_PREPROCESS_PEEL2_SAFE) {
    return true;
  }
  uint32_t fill_used = 0;
  bool changed = true;
  while (changed && !fg->is_zero) {
    if (!fg_peel2_once(fg, &fill_used, peel2_fill_budget, &changed, error)) {
      return false;
    }
    if (changed) {
      fg_peel1(fg);
    }
  }
  return true;
}

/*
 * Peel1: analytical degree-0 and degree-1 variable elimination.
 *
 * Degree-0: sum_v w(x_v) = 1 + w_true[v] → absorb into global, remove v.
 * Degree-1: sum over x_v given its one neighbor y → absorb F0 into global,
 *           adjust w_true[y] by F1/F0, remove v and pair. Zero-factor cases:
 *           - F0=F1=0: amplitude is zero, set is_zero.
 *           - F0=0: force y=1, absorb w_true[y]*F1; then peel y recursively.
 *           - F1=0: force y=0, absorb F0 into global; then peel y recursively.
 */
static void fg_peel1(wmc_factor_graph_t *fg) {
  bool changed = true;
  while (changed && !fg->is_zero) {
    changed = false;

    for (uint32_t v = 0; v < fg->nvars && !fg->is_zero; v++) {
      if (!fg->var_active[v]) {
        continue;
      }

      if (fg->var_forced[v] != 0) {
        /* Forced variable: absorb all its active pairs into neighbors, then absorb it. */
        bool all_pairs_done = true;
        for (size_t p = 0; p < fg->npairs; p++) {
          if (!fg->pair_active[p]) {
            continue;
          }
          uint32_t u = fg->pairs[p].u, w = fg->pairs[p].v;
          if (u != v && w != v) {
            continue;
          }
          const uint32_t z = (u == v) ? w : u;
          if (fg->var_forced[v] == +1) {
            /* y=1 always: pair contributes R when z=1, 1 when z=0 → w_true[z] *= R */
            cmul_ip(&fg->w_true_re[z], &fg->w_true_im[z], fg->pairs[p].R_re, fg->pairs[p].R_im);
          }
          /* If var_forced[v] == -1: pair contributes 1 (since v=0), just drop it. */
          fg->pair_active[p] = false;
          all_pairs_done = false;
          changed = true;
        }
        if (all_pairs_done || changed) {
          /* All pairs processed; absorb v's own weight then deactivate. */
          if (fg->var_forced[v] == +1) {
            /* v=1: absorb w_true[v] into global */
            cmul_ip(&fg->global_re, &fg->global_im, fg->w_true_re[v], fg->w_true_im[v]);
          }
          /* v=0: w_false[v]=1, nothing to absorb into global */
          fg->var_active[v] = false;
          changed = true;
        }
        continue;
      }

      /* Count active pair degree and remember the single neighbor. */
      uint32_t deg = 0;
      size_t single_p = 0;
      for (size_t p = 0; p < fg->npairs; p++) {
        if (!fg->pair_active[p]) {
          continue;
        }
        if (fg->pairs[p].u == v || fg->pairs[p].v == v) {
          single_p = p;
          deg++;
        }
      }

      if (deg == 0) {
        /* Degree-0: sum_v = 1 + w_true[v] */
        cmul_ip(&fg->global_re, &fg->global_im,
                1.0 + fg->w_true_re[v], fg->w_true_im[v]);
        fg->var_active[v] = false;
        changed = true;
      } else if (deg == 1) {
        /* Degree-1: sum over v given neighbor y. */
        const uint32_t y =
            (fg->pairs[single_p].u == v) ? fg->pairs[single_p].v : fg->pairs[single_p].u;
        const double R_re = fg->pairs[single_p].R_re;
        const double R_im = fg->pairs[single_p].R_im;
        const double U_re = fg->w_true_re[v];
        const double U_im = fg->w_true_im[v];

        /* F0 = 1 + U (contribution when y=0) */
        const double F0_re = 1.0 + U_re;
        const double F0_im = U_im;

        /* F1 = 1 + U*R (contribution when y=1) */
        const double F1_re = 1.0 + U_re * R_re - U_im * R_im;
        const double F1_im = U_re * R_im + U_im * R_re;

        const double mag2_F0 = FG_CMAG2(F0_re, F0_im);
        const double mag2_F1 = FG_CMAG2(F1_re, F1_im);

        if (mag2_F0 < FG_ZERO_EPS * FG_ZERO_EPS && mag2_F1 < FG_ZERO_EPS * FG_ZERO_EPS) {
          fg->is_zero = true;
        } else if (mag2_F0 < FG_ZERO_EPS * FG_ZERO_EPS) {
          /* F0=0: force y=1. Absorb F1 into w_true[y] (= old_w_true[y] * F1). */
          if (fg->var_forced[y] == -1) {
            fg->is_zero = true;
          } else {
            cmul_ip(&fg->w_true_re[y], &fg->w_true_im[y], F1_re, F1_im);
            fg->var_forced[y] = +1;
          }
          fg->var_active[v] = false;
          fg->pair_active[single_p] = false;
          changed = true;
        } else if (mag2_F1 < FG_ZERO_EPS * FG_ZERO_EPS) {
          /* F1=0: force y=0. Absorb F0 into global. */
          if (fg->var_forced[y] == +1) {
            fg->is_zero = true;
          } else {
            cmul_ip(&fg->global_re, &fg->global_im, F0_re, F0_im);
            fg->var_forced[y] = -1;
          }
          fg->var_active[v] = false;
          fg->pair_active[single_p] = false;
          changed = true;
        } else {
          /* Normal: global *= F0, w_true[y] *= F1/F0. */
          cmul_ip(&fg->global_re, &fg->global_im, F0_re, F0_im);
          cmul_div_ip(&fg->w_true_re[y], &fg->w_true_im[y], F1_re, F1_im, F0_re, F0_im);
          fg->var_active[v] = false;
          fg->pair_active[single_p] = false;
          changed = true;
        }
      }
    }
  }
}

/* Shared metadata writer for amplitude encodings. */
static void write_amp_metadata(FILE *file, const qsop_instance_t *qsop,
                                const wmc_factor_graph_t *fg, const char *encoding_tag,
                                uint32_t n_active_vars, uint32_t n_active_pairs) {
  const uint64_t r = qsop->r;
  const uint64_t c0 = qsop->constant % r;
  fprintf(file,
          "c sop2wmc encoding=%s r=%" PRIu64 " nvars=%" PRIu32 " nedges=%" PRIu32
          " format=qsop-sign norm_h=%" PRIu64 "\n",
          encoding_tag, r, qsop->nvars, qsop->nedges, qsop->norm_h);
  if (n_active_vars != qsop->nvars || n_active_pairs != (uint32_t)fg->npairs) {
    fprintf(file, "c preprocess nvars_after=%" PRIu32 " pairs_after=%" PRIu32 "\n",
            n_active_vars, n_active_pairs);
  }
  fprintf(file, "c constant_phase %" PRIu64 "\n", c0);
  fprintf(file, "c amplitude_factor %.17g+%.17gi\n", fg->global_re, fg->global_im);
  fputs("c amplitude = ganak_output * amplitude_factor\n", file);
  fputs("c probability = |amplitude|^2 * 2^(-norm_h)\n", file);
  fputs("c invoke: ganak --mode 6 --verb 0 <this-file>\n", file);
}

/*
 * Build a compact DIMACS variable map for active (non-eliminated) variables.
 * Returns the number of active variables. var_dimacs[v] = 0 for eliminated v.
 * Caller must free the returned array.
 */
static int *fg_build_var_map(const wmc_factor_graph_t *fg, uint32_t *n_active_out) {
  int *var_dimacs = calloc(fg->nvars, sizeof(*var_dimacs));
  if (var_dimacs == NULL) {
    return NULL;
  }
  int next_id = 1;
  for (uint32_t v = 0; v < fg->nvars; v++) {
    if (fg->var_active[v]) {
      var_dimacs[v] = next_id++;
    }
  }
  *n_active_out = (uint32_t)(next_id - 1);
  return var_dimacs;
}

static uint32_t fg_count_active_mapped_pairs(const wmc_factor_graph_t *fg,
                                             const int *var_dimacs) {
  uint32_t count = 0;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    if (var_dimacs[fg->pairs[p].u] == 0 || var_dimacs[fg->pairs[p].v] == 0) {
      continue;
    }
    count++;
  }
  return count;
}

/* amp-and: Tseitin AND auxiliary per encoded edge.
 * W(y=1) = omega^b, W(y=0) = 1. Three clauses per edge (2 binary + 1 ternary). */
static bool write_amplitude(FILE *file, const qsop_instance_t *qsop,
                            const wmc_factor_graph_t *fg, bool emit_metadata,
                            qsop_wmc_stats_t *stats_out, qsop_error_t *error) {
  uint32_t n_active_vars = 0;
  int *var_dimacs = fg_build_var_map(fg, &n_active_vars);
  if (fg->nvars > 0 && var_dimacs == NULL) {
    set_error(error, "out of memory while building amplitude CNF");
    return false;
  }

  const uint32_t encoded_edges = fg_count_active_mapped_pairs(fg, var_dimacs);
  const uint32_t nvars_total = n_active_vars + encoded_edges;
  const uint64_t nclauses = 3U * (uint64_t)encoded_edges;

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", nvars_total, nclauses);
  if (emit_metadata) {
    write_amp_metadata(file, qsop, fg, "amp-and", n_active_vars, encoded_edges);
    for (uint32_t v = 0; v < fg->nvars; v++) {
      if (var_dimacs[v] != 0) {
        fprintf(file, "c xvar %" PRIu32 " %d\n", v, var_dimacs[v]);
      }
    }
  }

  /* Literal weights for active variables with non-trivial true-weight. */
  for (uint32_t v = 0; v < fg->nvars; v++) {
    if (var_dimacs[v] == 0) {
      continue;
    }
    const double re = fg->w_true_re[v];
    const double im = fg->w_true_im[v];
    if (fabs(re - 1.0) > 1e-12 || fabs(im) > 1e-12) {
      write_weight(file, var_dimacs[v], re, im);
    }
  }

  /* Literal weights for Tseitin AND variables: W(y=1) = R_uv, W(y=0) = 1. */
  int aux = (int)n_active_vars + 1;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    if (var_dimacs[fg->pairs[p].u] == 0 || var_dimacs[fg->pairs[p].v] == 0) {
      continue;
    }
    write_weight(file, aux, fg->pairs[p].R_re, fg->pairs[p].R_im);
    aux++;
  }

  aux = (int)n_active_vars + 1;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    const int u = var_dimacs[fg->pairs[p].u];
    const int v = var_dimacs[fg->pairs[p].v];
    if (u == 0 || v == 0) {
      continue;
    }
    fprintf(file, "%d %d 0\n", -aux, u);
    fprintf(file, "%d %d 0\n", -aux, v);
    fprintf(file, "%d %d %d 0\n", aux, -u, -v);
    aux++;
  }

  if (stats_out != NULL) {
    *stats_out = (qsop_wmc_stats_t){0};
    stats_out->aux_vars = encoded_edges;
    stats_out->clauses_unit = 0;
    stats_out->clauses_binary = 2U * encoded_edges;
    stats_out->clauses_ternary = encoded_edges;
    stats_out->encoded_edges = encoded_edges;
    stats_out->skipped_edges = qsop->nedges - encoded_edges;
    stats_out->residual_edges = encoded_edges;
    stats_out->active_vars = n_active_vars;
    stats_out->eliminated_vars = qsop->nvars - n_active_vars;
  }

  free(var_dimacs);
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
  uint32_t n_active_vars = 0;
  int *var_dimacs = fg_build_var_map(fg, &n_active_vars);
  if (fg->nvars > 0 && var_dimacs == NULL) {
    set_error(error, "out of memory while building amp-soft CNF");
    return false;
  }

  const uint32_t encoded_edges = fg_count_active_mapped_pairs(fg, var_dimacs);
  const uint32_t nvars_total = n_active_vars + encoded_edges;
  const uint64_t nclauses = 2U * (uint64_t)encoded_edges;

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", nvars_total, nclauses);
  if (emit_metadata) {
    write_amp_metadata(file, qsop, fg, "amp-soft", n_active_vars, encoded_edges);
    for (uint32_t v = 0; v < fg->nvars; v++) {
      if (var_dimacs[v] != 0) {
        fprintf(file, "c xvar %" PRIu32 " %d\n", v, var_dimacs[v]);
      }
    }
  }

  /* Literal weights for active variables with non-trivial true-weight. */
  for (uint32_t v = 0; v < fg->nvars; v++) {
    if (var_dimacs[v] == 0) {
      continue;
    }
    const double re = fg->w_true_re[v];
    const double im = fg->w_true_im[v];
    if (fabs(re - 1.0) > 1e-12 || fabs(im) > 1e-12) {
      write_weight(file, var_dimacs[v], re, im);
    }
  }

  /* Literal weights for soft auxiliaries: W(y=1) = R_uv - 1, W(y=0) = 1. */
  int aux = (int)n_active_vars + 1;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    if (var_dimacs[fg->pairs[p].u] == 0 || var_dimacs[fg->pairs[p].v] == 0) {
      continue;
    }
    write_weight(file, aux, fg->pairs[p].R_re - 1.0, fg->pairs[p].R_im);
    aux++;
  }

  aux = (int)n_active_vars + 1;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    const int u = var_dimacs[fg->pairs[p].u];
    const int v = var_dimacs[fg->pairs[p].v];
    if (u == 0 || v == 0) {
      continue;
    }
    fprintf(file, "%d %d 0\n", -aux, u);  /* y -> x_u */
    fprintf(file, "%d %d 0\n", -aux, v);  /* y -> x_v */
    aux++;
  }

  if (stats_out != NULL) {
    const uint32_t total_skipped = qsop->nedges - (uint32_t)fg->npairs + (uint32_t)fg->npairs -
                                    encoded_edges;
    *stats_out = (qsop_wmc_stats_t){0};
    stats_out->aux_vars = encoded_edges;
    stats_out->clauses_unit = 0;
    stats_out->clauses_binary = 2U * encoded_edges;
    stats_out->clauses_ternary = 0;
    stats_out->encoded_edges = encoded_edges;
    stats_out->skipped_edges = total_skipped;
    stats_out->residual_edges = encoded_edges;
    stats_out->active_vars = n_active_vars;
    stats_out->eliminated_vars = qsop->nvars - n_active_vars;
  }

  free(var_dimacs);
  if (ferror(file)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

/*
 * Write a WPCNF for a zero-amplitude result (amplitude = 0).
 * Emits a trivially unsatisfiable CNF: empty clause (0 0) with amplitude_factor = 0+0i.
 */
static bool write_zero_amplitude(FILE *file, const qsop_instance_t *qsop, bool emit_metadata,
                                  qsop_error_t *error) {
  fprintf(file, "p cnf 0 1\n");
  if (emit_metadata) {
    fprintf(file,
            "c sop2wmc encoding=zero r=%" PRIu64 " nvars=%" PRIu32 " nedges=%" PRIu32
            " format=qsop-sign norm_h=%" PRIu64 "\n",
            qsop->r, qsop->nvars, qsop->nedges, qsop->norm_h);
    fputs("c amplitude_factor 0+0i\n", file);
    fputs("c amplitude = 0 (analytically determined by preprocessing)\n", file);
  }
  /* Empty clause (no literals before 0) → UNSAT → WMC = 0. */
  fputs("0\n", file);
  if (ferror(file)) {
    set_error(error, "write failed: %s", strerror(errno));
    return false;
  }
  return true;
}

typedef struct wmc_sign_block {
  uint32_t *a;
  uint32_t *b;
  uint32_t a_len;
  uint32_t b_len;
  uint32_t edge_count;
} wmc_sign_block_t;

typedef struct wmc_sign_blocks {
  wmc_sign_block_t *items;
  size_t len;
  size_t cap;
} wmc_sign_blocks_t;

static void sign_block_free(wmc_sign_block_t *block) {
  if (block == NULL) {
    return;
  }
  free(block->a);
  free(block->b);
  *block = (wmc_sign_block_t){0};
}

static void sign_blocks_free(wmc_sign_blocks_t *blocks) {
  if (blocks == NULL) {
    return;
  }
  for (size_t i = 0; i < blocks->len; i++) {
    sign_block_free(&blocks->items[i]);
  }
  free(blocks->items);
  *blocks = (wmc_sign_blocks_t){0};
}

static bool sign_blocks_push(wmc_sign_blocks_t *blocks, wmc_sign_block_t *block,
                             qsop_error_t *error) {
  if (blocks->len == blocks->cap) {
    const size_t new_cap = blocks->cap == 0 ? 4U : blocks->cap * 2U;
    wmc_sign_block_t *items = realloc(blocks->items, new_cap * sizeof(*items));
    if (items == NULL) {
      set_error(error, "out of memory storing amp-block parity blocks");
      return false;
    }
    blocks->items = items;
    blocks->cap = new_cap;
  }
  blocks->items[blocks->len++] = *block;
  *block = (wmc_sign_block_t){0};
  return true;
}

static bool fg_pair_is_sign(const wmc_pair_t *pair) {
  return pair != NULL && complex_near(pair->R_re, pair->R_im, -1.0, 0.0);
}

/* Estimated auxiliaries for a sign-edge block: side parity XOR chains + one soft factor. */
static int64_t block_cost(uint32_t a, uint32_t b) {
  const uint32_t a_xor = a > 0U ? a - 1U : 0U;
  const uint32_t b_xor = b > 0U ? b - 1U : 0U;
  return (int64_t)a_xor + (int64_t)b_xor + 1;
}

static bool bitset_contains_all(const uint64_t *set, const uint64_t *subset, size_t words) {
  for (size_t w = 0; w < words; w++) {
    if ((set[w] & subset[w]) != subset[w]) {
      return false;
    }
  }
  return true;
}

/*
 * Find the best uncovered complete bipartite sign block in the active factor graph.
 * The search is bitset-based and greedy; callers repeat it while marking covered
 * sign-pair indices to extract edge-disjoint blocks.
 */
static bool find_best_sign_block(const wmc_factor_graph_t *fg, const uint64_t *adj,
                                 const uint64_t *active_bits, size_t words,
                                 uint64_t *b_bits, uint32_t *a_tmp, uint32_t *b_tmp,
                                 uint32_t min_side, int64_t min_savings,
                                 wmc_sign_block_t *out, bool *found_out,
                                 qsop_error_t *error) {
  const uint32_t n = fg->nvars;
  if (found_out != NULL) {
    *found_out = false;
  }
  if (n == 0 || fg->npairs == 0) return true;
  if (min_side == 0U) min_side = 1U;

  uint32_t best_score = 0;
  uint32_t *best_a = NULL, *best_b = NULL;
  uint32_t best_a_len = 0, best_b_len = 0;

  for (uint32_t u = 0; u < n; u++) {
    if (!fg->var_active[u]) {
      continue;
    }
    const uint64_t *u_adj = qsop_bitset_const_row(adj, words, u);

    /* B = uncovered sign neighbours of u. */
    qsop_bitset_copy(b_bits, u_adj, words);
    qsop_bitset_and(b_bits, active_bits, words);
    const uint32_t b_len = qsop_bitset_popcount(b_bits, words);
    if (b_len < min_side) continue;
    uint32_t b_written = 0;
    for (uint32_t v = 0; v < n; v++) {
      if (qsop_bitset_get(b_bits, v)) {
        b_tmp[b_written++] = v;
      }
    }

    /* A = vertices not in B whose uncovered sign-neighbourhood contains B. */
    uint32_t a_len = 0;
    for (uint32_t up = 0; up < n; up++) {
      if (!fg->var_active[up]) {
        continue;
      }
      if (qsop_bitset_get(b_bits, up)) continue;

      const uint64_t *up_adj = qsop_bitset_const_row(adj, words, up);
      if (bitset_contains_all(up_adj, b_bits, words)) a_tmp[a_len++] = up;
    }
    if (a_len < min_side) continue;

    const int64_t sv = (int64_t)a_len * (int64_t)b_len - block_cost(a_len, b_len);
    if (sv < min_savings) continue;
    const uint32_t score = a_len * b_len;
    if (score > best_score) {
      best_score = score;
      best_a_len = a_len;
      best_b_len = b_len;
      free(best_a); free(best_b);
      best_a = malloc(a_len * sizeof(*best_a));
      best_b = malloc(b_len * sizeof(*best_b));
      if (!best_a || !best_b) {
        free(best_a); free(best_b);
        set_error(error, "out of memory searching amp-block parity blocks");
        return false;
      }
      memcpy(best_a, a_tmp, a_len * sizeof(*best_a));
      memcpy(best_b, b_tmp, b_len * sizeof(*best_b));
    }
  }

  if (best_score == 0) return true;
  out->a = best_a;
  out->a_len = best_a_len;
  out->b = best_b;
  out->b_len = best_b_len;
  out->edge_count = best_a_len * best_b_len;
  if (found_out != NULL) {
    *found_out = true;
  }
  return true;
}

static bool extract_sign_blocks(const wmc_factor_graph_t *fg, uint32_t min_side,
                                int64_t min_savings, wmc_sign_blocks_t *blocks,
                                bool **covered_out, qsop_error_t *error) {
  bool *covered = calloc(fg->npairs == 0 ? 1U : fg->npairs, sizeof(*covered));
  if (covered == NULL) {
    set_error(error, "out of memory extracting amp-block parity blocks");
    return false;
  }
  if (fg->nvars == 0 || fg->npairs == 0) {
    *covered_out = covered;
    return true;
  }

  const uint32_t n = fg->nvars;
  const size_t words = qsop_bitset_words(n);
  uint64_t *adj = calloc((size_t)n * words, sizeof(*adj));
  uint64_t *active_bits = calloc(words, sizeof(*active_bits));
  uint64_t *b_bits = calloc(words, sizeof(*b_bits));
  uint32_t *b_tmp = malloc(n * sizeof(*b_tmp));
  uint32_t *a_tmp = malloc(n * sizeof(*a_tmp));
  uint32_t *mark_a = calloc(n, sizeof(*mark_a));
  uint32_t *mark_b = calloc(n, sizeof(*mark_b));
  if (adj == NULL || active_bits == NULL || b_bits == NULL || b_tmp == NULL ||
      a_tmp == NULL || mark_a == NULL || mark_b == NULL) {
    free(adj);
    free(active_bits);
    free(b_bits);
    free(b_tmp);
    free(a_tmp);
    free(mark_a);
    free(mark_b);
    free(covered);
    set_error(error, "out of memory extracting amp-block parity blocks");
    return false;
  }

  for (uint32_t v = 0; v < n; v++) {
    if (fg->var_active[v]) {
      qsop_bitset_set(active_bits, v);
    }
  }
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p] || !fg_pair_is_sign(&fg->pairs[p])) {
      continue;
    }
    const uint32_t u = fg->pairs[p].u;
    const uint32_t v = fg->pairs[p].v;
    if (!fg->var_active[u] || !fg->var_active[v]) {
      continue;
    }
    qsop_bitset_set(qsop_bitset_row(adj, words, u), v);
    qsop_bitset_set(qsop_bitset_row(adj, words, v), u);
  }

  uint32_t marker_stamp = 0;
  for (;;) {
    wmc_sign_block_t block = {0};
    bool found = false;
    if (!find_best_sign_block(fg, adj, active_bits, words, b_bits, a_tmp, b_tmp,
                              min_side, min_savings, &block, &found, error)) {
      sign_blocks_free(blocks);
      free(adj);
      free(active_bits);
      free(b_bits);
      free(b_tmp);
      free(a_tmp);
      free(mark_a);
      free(mark_b);
      free(covered);
      return false;
    }
    if (!found) {
      break;
    }
    marker_stamp++;
    if (marker_stamp == 0) {
      memset(mark_a, 0, (size_t)n * sizeof(*mark_a));
      memset(mark_b, 0, (size_t)n * sizeof(*mark_b));
      marker_stamp = 1;
    }
    for (uint32_t i = 0; i < block.a_len; i++) mark_a[block.a[i]] = marker_stamp;
    for (uint32_t i = 0; i < block.b_len; i++) mark_b[block.b[i]] = marker_stamp;
    uint32_t marked = 0;
    for (size_t p = 0; p < fg->npairs; p++) {
      if (!fg->pair_active[p] || covered[p] || !fg_pair_is_sign(&fg->pairs[p])) {
        continue;
      }
      const uint32_t u = fg->pairs[p].u;
      const uint32_t v = fg->pairs[p].v;
      if ((mark_a[u] == marker_stamp && mark_b[v] == marker_stamp) ||
          (mark_a[v] == marker_stamp && mark_b[u] == marker_stamp)) {
        covered[p] = true;
        qsop_bitset_clear(qsop_bitset_row(adj, words, u), v);
        qsop_bitset_clear(qsop_bitset_row(adj, words, v), u);
        marked++;
      }
    }
    if (marked != block.edge_count) {
      sign_block_free(&block);
      continue;
    }
    if (!sign_blocks_push(blocks, &block, error)) {
      sign_block_free(&block);
      sign_blocks_free(blocks);
      free(adj);
      free(active_bits);
      free(b_bits);
      free(b_tmp);
      free(a_tmp);
      free(mark_a);
      free(mark_b);
      free(covered);
      return false;
    }
  }
  free(adj);
  free(active_bits);
  free(b_bits);
  free(b_tmp);
  free(a_tmp);
  free(mark_a);
  free(mark_b);
  *covered_out = covered;
  return true;
}

/* XOR parity literal for a non-empty side of a sign-edge block. */
static int build_parity_lit(wmc_builder_t *b, const int *vars, uint32_t n_vars) {
  int parity = vars[0];
  for (uint32_t i = 1; i < n_vars; i++) {
    parity = gate_xor(b, parity, vars[i]);
  }
  return parity;
}

static bool write_amp_block(FILE *file, const qsop_instance_t *qsop,
                             const wmc_factor_graph_t *fg, bool emit_metadata,
                             uint32_t min_side, int64_t min_savings,
                             qsop_wmc_stats_t *stats_out,
                             qsop_error_t *error) {
  wmc_sign_blocks_t blocks = {0};
  bool *covered = NULL;
  if (!extract_sign_blocks(fg, min_side, min_savings, &blocks, &covered, error)) {
    return false;
  }
  if (blocks.len == 0) {
    free(covered);
    sign_blocks_free(&blocks);
    return write_amp_soft(file, qsop, fg, emit_metadata, stats_out, error);
  }

  uint32_t n_active_vars = 0;
  int *var_dimacs = fg_build_var_map(fg, &n_active_vars);
  if (fg->nvars > 0 && var_dimacs == NULL) {
    free(covered);
    sign_blocks_free(&blocks);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }

  wmc_builder_t b = {0};
  b.nvars = n_active_vars;

  int *block_var = calloc(blocks.len, sizeof(*block_var));
  uint32_t parity_aux = 0;
  uint32_t covered_edges = 0;
  if (block_var == NULL) {
    free(var_dimacs);
    free(covered);
    sign_blocks_free(&blocks);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }

  for (size_t bi = 0; bi < blocks.len; bi++) {
    const wmc_sign_block_t *block = &blocks.items[bi];
    int *a_dimacs = malloc(block->a_len * sizeof(*a_dimacs));
    int *b_dimacs = malloc(block->b_len * sizeof(*b_dimacs));
    if (a_dimacs == NULL || b_dimacs == NULL) {
      free(a_dimacs);
      free(b_dimacs);
      free(block_var);
      free(var_dimacs);
      free(covered);
      sign_blocks_free(&blocks);
      builder_free(&b);
      set_error(error, "out of memory building amp-block CNF");
      return false;
    }
    for (uint32_t i = 0; i < block->a_len; i++) {
      a_dimacs[i] = var_dimacs[block->a[i]];
    }
    for (uint32_t i = 0; i < block->b_len; i++) {
      b_dimacs[i] = var_dimacs[block->b[i]];
    }
    parity_aux += (block->a_len > 0U ? block->a_len - 1U : 0U) +
                  (block->b_len > 0U ? block->b_len - 1U : 0U);
    const int parity_a = build_parity_lit(&b, a_dimacs, block->a_len);
    const int parity_b = build_parity_lit(&b, b_dimacs, block->b_len);
    block_var[bi] = new_var(&b);
    add_clause2(&b, -block_var[bi], parity_a);
    add_clause2(&b, -block_var[bi], parity_b);
    covered_edges += block->edge_count;
    free(a_dimacs);
    free(b_dimacs);
  }

  uint32_t n_extra = 0;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p] || covered[p]) {
      continue;
    }
    const int u = var_dimacs[fg->pairs[p].u];
    const int v = var_dimacs[fg->pairs[p].v];
    if (u == 0 || v == 0) {
      continue;
    }
    n_extra++;
  }
  const int extra_first_var = (int)b.nvars + 1;
  b.nvars += n_extra;
  b.nclauses += 2U * (uint64_t)n_extra;

  if (b.failed) {
    free(block_var);
    free(var_dimacs);
    free(covered);
    sign_blocks_free(&blocks);
    builder_free(&b);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b.nvars, b.nclauses);
  if (emit_metadata) {
    fprintf(file,
            "c sop2wmc encoding=amp-block r=%" PRIu64 " nvars=%" PRIu32 " nedges=%" PRIu32
            " format=qsop-sign norm_h=%" PRIu64 "\n",
            qsop->r, qsop->nvars, qsop->nedges, qsop->norm_h);
    fprintf(file,
            "c block count=%zu covered_edges=%" PRIu32 " residual_edges=%" PRIu32
            " nvars_after=%" PRIu32 "\n",
            blocks.len, covered_edges, n_extra, n_active_vars);
    for (size_t bi = 0; bi < blocks.len; bi++) {
      fprintf(file, "c block sign-parity index=%zu a_size=%" PRIu32 " b_size=%" PRIu32 "\n",
              bi, blocks.items[bi].a_len, blocks.items[bi].b_len);
    }
    fprintf(file, "c amplitude_factor %.17g+%.17gi\n", fg->global_re, fg->global_im);
    fputs("c amplitude = ganak_output * amplitude_factor\n", file);
    fputs("c invoke: ganak --mode 6 --verb 0 <this-file>\n", file);
    for (uint32_t v = 0; v < fg->nvars; v++) {
      if (var_dimacs[v] != 0) {
        fprintf(file, "c xvar %" PRIu32 " %d\n", v, var_dimacs[v]);
      }
    }
  }

  for (uint32_t v = 0; v < fg->nvars; v++) {
    if (var_dimacs[v] == 0) {
      continue;
    }
    const double re = fg->w_true_re[v];
    const double im = fg->w_true_im[v];
    if (fabs(re - 1.0) > 1e-12 || fabs(im) > 1e-12) {
      write_weight(file, var_dimacs[v], re, im);
    }
  }
  for (size_t bi = 0; bi < blocks.len; bi++) {
    write_weight(file, block_var[bi], -2.0, 0.0);
  }
  int extra_var = extra_first_var;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p] || covered[p]) {
      continue;
    }
    if (var_dimacs[fg->pairs[p].u] == 0 || var_dimacs[fg->pairs[p].v] == 0) {
      continue;
    }
    const wmc_pair_t *pair = &fg->pairs[p];
    write_weight(file, extra_var, pair->R_re - 1.0, pair->R_im);
    extra_var++;
  }

  for (size_t ci = 0; ci < b.len; ci++) {
    if (b.lits[ci] == 0) {
      fputs("0\n", file);
    } else {
      fprintf(file, "%d ", b.lits[ci]);
    }
  }
  extra_var = extra_first_var;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p] || covered[p]) {
      continue;
    }
    const int u = var_dimacs[fg->pairs[p].u];
    const int v = var_dimacs[fg->pairs[p].v];
    if (u == 0 || v == 0) {
      continue;
    }
    fprintf(file, "%d %d 0\n", -extra_var, u);
    fprintf(file, "%d %d 0\n", -extra_var, v);
    extra_var++;
  }

  if (stats_out != NULL) {
    *stats_out = (qsop_wmc_stats_t){0};
    stats_out->aux_vars = b.nvars - n_active_vars;
    stats_out->clauses_binary = 2U * ((uint32_t)blocks.len + n_extra);
    stats_out->clauses_ternary = 4U * parity_aux;
    stats_out->encoded_edges = covered_edges + n_extra;
    stats_out->skipped_edges = qsop->nedges >= stats_out->encoded_edges
                                   ? qsop->nedges - stats_out->encoded_edges
                                   : 0U;
    stats_out->block_count = (uint32_t)blocks.len;
    stats_out->block_edges = covered_edges;
    stats_out->residual_edges = n_extra;
    stats_out->active_vars = n_active_vars;
    stats_out->eliminated_vars = qsop->nvars - n_active_vars;
  }

  free(block_var);
  free(var_dimacs);
  free(covered);
  sign_blocks_free(&blocks);
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
    set_error(error, "WMC export requires a positive even modulus, got %" PRIu64, qsop->r);
    return false;
  }

  if (options->encoding == QSOP_WMC_ENCODING_AMP_BLOCK) {
    wmc_factor_graph_t fg = {0};
    if (!fg_from_qsop(qsop, 1U, &fg, error)) {
      return false;
    }
    if (!fg_preprocess(&fg, options->preprocess, options->peel2_fill_budget, error)) {
      fg_free(&fg);
      return false;
    }
    bool ok;
    if (fg.is_zero) {
      ok = write_zero_amplitude(file, qsop, options->emit_metadata, error);
    } else {
      ok = write_amp_block(file, qsop, &fg, options->emit_metadata,
                           options->block_min_side, options->block_min_savings,
                           options->stats_out, error);
    }
    fg_free(&fg);
    return ok;
  }

  if (options->encoding == QSOP_WMC_ENCODING_AMPLITUDE ||
      options->encoding == QSOP_WMC_ENCODING_AMP_SOFT) {
    wmc_factor_graph_t fg = {0};
    if (!fg_from_qsop(qsop, 1U, &fg, error)) {
      return false;
    }

    if (!fg_preprocess(&fg, options->preprocess, options->peel2_fill_budget, error)) {
      fg_free(&fg);
      return false;
    }

    bool ok;
    if (fg.is_zero) {
      ok = write_zero_amplitude(file, qsop, options->emit_metadata, error);
    } else if (options->encoding == QSOP_WMC_ENCODING_AMPLITUDE) {
      ok = write_amplitude(file, qsop, &fg, options->emit_metadata, options->stats_out, error);
    } else {
      ok = write_amp_soft(file, qsop, &fg, options->emit_metadata, options->stats_out, error);
    }
    fg_free(&fg);
    return ok;
  }

  if (options->encoding == QSOP_WMC_ENCODING_RESIDUE_FOURIER) {
    /* fg_from_qsop (per-t weight computation) is O(1) in r, so a single target mode needs no
     * modulus ceiling. Emitting every mode, below, is a distinct O(r) loop and is gated on its
     * own right before it runs. */
    if (!options->fourier_all_modes && (uint64_t)options->fourier_mode >= qsop->r) {
      set_error(error, "Fourier mode %" PRIu32 " is out of range for modulus %" PRIu64,
                options->fourier_mode, qsop->r);
      return false;
    }
    if (options->fourier_all_modes && qsop->r > UINT32_MAX) {
      set_error(error,
                "WMC residue-fourier(all-modes) encoding refuses modulus > 2^32-1; export a "
                "single --fourier-mode instead");
      return false;
    }
    qsop_wmc_encoding_t inner = options->fourier_inner;
    /* Default inner encoding: amp-soft. */
    if (inner != QSOP_WMC_ENCODING_AMPLITUDE && inner != QSOP_WMC_ENCODING_AMP_SOFT) {
      inner = QSOP_WMC_ENCODING_AMP_SOFT;
    }

    const uint32_t begin_t = options->fourier_all_modes ? 0U : options->fourier_mode;
    const uint32_t end_t =
        options->fourier_all_modes ? (uint32_t)qsop->r : options->fourier_mode + 1U;
    for (uint32_t t = begin_t; t < end_t; t++) {
      if (options->emit_metadata) {
        fprintf(file, "c --- fourier t=%" PRIu32 " r=%" PRIu64 " ---\n", t, qsop->r);
      }

      if (t == 0) {
        /* Z_0 = 2^n (all assignments contribute omega^0 = 1). Skip Ganak. */
        fprintf(file, "p cnf %" PRIu32 " 0\n", qsop->nvars);
        if (options->emit_metadata) {
          fprintf(file,
                  "c sop2wmc encoding=fourier-t0 r=%" PRIu64 " nvars=%" PRIu32
                  " t=0 z0_log2=%" PRIu32 "\n",
                  qsop->r, qsop->nvars, qsop->nvars);
          fputs("c amplitude_factor 1+0i\n", file);
          fputs("c z0 = 2^nvars (trivial: no Ganak call needed)\n", file);
        }
        if (ferror(file)) {
          set_error(error, "write failed: %s", strerror(errno));
          return false;
        }
        continue;
      }

      wmc_factor_graph_t fg = {0};
      if (!fg_from_qsop(qsop, t, &fg, error)) {
        return false;
      }

      if (!fg_preprocess(&fg, options->preprocess, options->peel2_fill_budget, error)) {
        fg_free(&fg);
        return false;
      }

      bool ok;
      if (fg.is_zero) {
        ok = write_zero_amplitude(file, qsop, options->emit_metadata, error);
      } else if (inner == QSOP_WMC_ENCODING_AMPLITUDE) {
        ok = write_amplitude(file, qsop, &fg, options->emit_metadata, NULL, error);
      } else {
        ok = write_amp_soft(file, qsop, &fg, options->emit_metadata, NULL, error);
      }
      fg_free(&fg);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  /* RESIDUE encoding: a mod-r adder circuit, sized O(r) once all residues are emitted. Check
   * the UINT32_MAX ceiling explicitly before calling width_for_modulus (which takes a uint32_t
   * and would otherwise silently truncate a too-wide qsop->r before the WMC_MAX_WIDTH check
   * below ever saw the real value). */
  if (qsop->r > UINT32_MAX || width_for_modulus((uint32_t)qsop->r) > WMC_MAX_WIDTH) {
    set_error(error, "WMC export modulus %" PRIu64 " is too large", qsop->r);
    return false;
  }
  if (!options->all_residues && (uint64_t)options->residue >= qsop->r) {
    set_error(error, "residue %" PRIu32 " is out of range for modulus %" PRIu64, options->residue,
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
    for (uint32_t k = 0; ok && k < (uint32_t)qsop->r; k++) {
      ok = write_block(file, &builder, qsop, w, afinal, k, options->emit_metadata, true, error);
    }
  } else {
    ok = write_block(file, &builder, qsop, w, afinal, options->residue, options->emit_metadata,
                     false, error);
  }

  builder_free(&builder);
  return ok;
}
