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

static bool write_metadata(FILE *file, const qsop_instance_t *qsop, uint32_t w,
                           const int *afinal, uint32_t residue) {
  fprintf(file,
          "c sop2wmc encoding=residue r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
          " format=qsop-sign norm_h=%" PRIu64 "\n",
          qsop->r, qsop->nvars, qsop->nedges, qsop->norm_h);
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
  options.stats_out = NULL;
  options.block_min_side = 4;
  options.block_min_savings = 0;
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
  bool *var_active;   /* length nvars: variable remains in the WPCNF after preprocessing */
  int8_t *var_forced; /* length nvars: 0=not forced, +1=force true, -1=force false */
  bool *pair_active;  /* length npairs: pair remains in the WPCNF after preprocessing */
  bool is_zero;       /* amplitude is analytically zero; skip WPCNF entirely */
} wmc_factor_graph_t;

#define FG_CMAG2(re, im) ((re) * (re) + (im) * (im))

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
}

/* Build a factor graph for Fourier exponent t: weights become omega^(t*label mod r). */
static bool fg_from_qsop(const qsop_instance_t *qsop, uint32_t t, wmc_factor_graph_t *fg,
                          qsop_error_t *error) {
  const uint32_t r = qsop->r;
  *fg = (wmc_factor_graph_t){0};
  fg->nvars = qsop->nvars;

  /* Global factor: omega^(t*c0 mod r). */
  omega_power((uint32_t)((t * (uint64_t)(qsop->constant % r)) % r), r,
              &fg->global_re, &fg->global_im);

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
      const uint32_t coeff = (uint32_t)((t * (uint64_t)(qsop->unary[v] % r)) % r);
      omega_power(coeff, r, &fg->w_true_re[v], &fg->w_true_im[v]);
      fg->var_active[v] = true;
    }
  }

  /* Pair factors: only odd Fourier modes see sign edges; even modes have multiplier 1. */
  if (qsop->nedges > 0) {
    fg->pairs = malloc(qsop->nedges * sizeof(*fg->pairs));
    fg->pair_active = malloc(qsop->nedges * sizeof(*fg->pair_active));
    if (!fg->pairs || !fg->pair_active) {
      set_error(error, "out of memory building WMC factor graph");
      fg_free(fg);
      return false;
    }
    fg->npairs = 0;
    for (uint32_t e = 0; e < qsop->nedges; e++) {
      const uint32_t coeff = (t & 1U) == 0U ? 0U : r / 2U;
      if (coeff == 0U) {
        continue;
      }
      wmc_pair_t *p = &fg->pairs[fg->npairs];
      p->u = qsop->edge_u[e];
      p->v = qsop->edge_v[e];
      omega_power(coeff, r, &p->R_re, &p->R_im);
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

#define FG_ZERO_EPS 1e-12

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
          cmul_ip(&fg->w_true_re[y], &fg->w_true_im[y], F1_re, F1_im);
          fg->var_forced[y] = +1;
          fg->var_active[v] = false;
          fg->pair_active[single_p] = false;
          changed = true;
        } else if (mag2_F1 < FG_ZERO_EPS * FG_ZERO_EPS) {
          /* F1=0: force y=0. Absorb F0 into global. */
          cmul_ip(&fg->global_re, &fg->global_im, F0_re, F0_im);
          fg->var_forced[y] = -1;
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
  const uint32_t r = qsop->r;
  const uint32_t c0 = (uint32_t)(qsop->constant % r);
  fprintf(file,
          "c sop2wmc encoding=%s r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
          " format=qsop-sign norm_h=%" PRIu64 "\n",
          encoding_tag, r, qsop->nvars, qsop->nedges, qsop->norm_h);
  if (n_active_vars != qsop->nvars || n_active_pairs != (uint32_t)fg->npairs) {
    fprintf(file, "c preprocess nvars_after=%" PRIu32 " pairs_after=%" PRIu32 "\n",
            n_active_vars, n_active_pairs);
  }
  fprintf(file, "c constant_phase %" PRIu32 "\n", c0);
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

  /* Count active pairs and build Tseitin AND clauses. */
  wmc_builder_t b = {0};
  b.nvars = n_active_vars;

  int *pair_var = fg->npairs > 0 ? calloc(fg->npairs, sizeof(*pair_var)) : NULL;
  if (fg->npairs > 0 && pair_var == NULL) {
    set_error(error, "out of memory while building amplitude CNF");
    free(var_dimacs);
    return false;
  }
  uint32_t encoded_edges = 0;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    pair_var[p] = gate_and(&b, var_dimacs[fg->pairs[p].u], var_dimacs[fg->pairs[p].v]);
    encoded_edges++;
  }
  if (b.failed) {
    set_error(error, "out of memory while building amplitude CNF");
    free(pair_var);
    free(var_dimacs);
    builder_free(&b);
    return false;
  }

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b.nvars, b.nclauses);
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
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
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
    stats_out->skipped_edges = qsop->nedges - encoded_edges;
  }

  free(pair_var);
  free(var_dimacs);
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
  uint32_t n_active_vars = 0;
  int *var_dimacs = fg_build_var_map(fg, &n_active_vars);
  if (fg->nvars > 0 && var_dimacs == NULL) {
    set_error(error, "out of memory while building amp-soft CNF");
    return false;
  }

  wmc_builder_t b = {0};
  b.nvars = n_active_vars;

  int *pair_var = fg->npairs > 0 ? calloc(fg->npairs, sizeof(*pair_var)) : NULL;
  if (fg->npairs > 0 && pair_var == NULL) {
    set_error(error, "out of memory while building amp-soft CNF");
    free(var_dimacs);
    return false;
  }
  uint32_t encoded_edges = 0;
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
    const int y = new_var(&b);
    pair_var[p] = y;
    add_clause2(&b, -y, var_dimacs[fg->pairs[p].u]);  /* y -> x_u */
    add_clause2(&b, -y, var_dimacs[fg->pairs[p].v]);  /* y -> x_v */
    encoded_edges++;
  }
  if (b.failed) {
    set_error(error, "out of memory while building amp-soft CNF");
    free(pair_var);
    free(var_dimacs);
    builder_free(&b);
    return false;
  }

  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b.nvars, b.nclauses);
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
  for (size_t p = 0; p < fg->npairs; p++) {
    if (!fg->pair_active[p]) {
      continue;
    }
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
    const uint32_t total_skipped = qsop->nedges - (uint32_t)fg->npairs + (uint32_t)fg->npairs -
                                    encoded_edges;
    stats_out->aux_vars = encoded_edges;
    stats_out->clauses_unit = 0;
    stats_out->clauses_binary = 2U * encoded_edges;
    stats_out->clauses_ternary = 0;
    stats_out->encoded_edges = encoded_edges;
    stats_out->skipped_edges = total_skipped;
  }

  free(pair_var);
  free(var_dimacs);
  builder_free(&b);
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
            "c sop2wmc encoding=zero r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
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

/* Estimated auxiliaries for a sign-edge block: side parity XOR chains + one soft factor. */
static int64_t block_cost(uint32_t a, uint32_t b) {
  const uint32_t a_xor = a > 0U ? a - 1U : 0U;
  const uint32_t b_xor = b > 0U ? b - 1U : 0U;
  return (int64_t)a_xor + (int64_t)b_xor + 1;
}

/*
 * Find the best uniform-label complete bipartite block in qsop.
 * Returns true and fills *a_out and *b_out (caller must free) if a block
 * with savings >= min_savings and both sides >= min_side is found.
 */
static bool find_block(const qsop_instance_t *qsop,
                       uint32_t min_side, int64_t min_savings,
                       uint32_t **a_out, uint32_t *a_len_out,
                       uint32_t **b_out, uint32_t *b_len_out,
                       uint32_t *label_out) {
  const uint32_t n = qsop->nvars;
  const uint32_t r = qsop->r;

  if (n == 0 || qsop->nedges == 0) return false;
  if (min_side == 0U) min_side = 1U;

  /* Build adjacency matrix for sign edges. */
  bool *adj = calloc((size_t)n * n, sizeof(*adj));
  if (adj == NULL) return false;
  const uint32_t sign_label = r / 2U;
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint32_t u = qsop->edge_u[e];
    const uint32_t v = qsop->edge_v[e];
    adj[u * n + v] = true;
    adj[v * n + u] = true;
  }

  uint32_t best_score = 0;
  uint32_t *best_a = NULL, *best_b = NULL;
  uint32_t best_a_len = 0, best_b_len = 0;
  uint32_t best_label = sign_label;

  uint32_t *b_tmp = malloc(n * sizeof(*b_tmp));
  uint32_t *a_tmp = malloc(n * sizeof(*a_tmp));
  if (!b_tmp || !a_tmp) {
    free(b_tmp); free(a_tmp); free(adj);
    return false;
  }

  for (uint32_t u = 0; u < n; u++) {
    /* B = sign neighbours of u. */
    uint32_t b_len = 0;
    for (uint32_t v = 0; v < n; v++) {
      if (adj[u * n + v]) b_tmp[b_len++] = v;
    }
    if (b_len < min_side) continue;

    /* A = vertices not in B whose whole sign-neighbourhood contains B. */
    uint32_t a_len = 0;
    for (uint32_t up = 0; up < n; up++) {
      bool in_b = false;
      for (uint32_t bi = 0; bi < b_len; bi++) {
        if (b_tmp[bi] == up) { in_b = true; break; }
      }
      if (in_b) continue;

      bool covers_b = true;
      for (uint32_t bi = 0; bi < b_len; bi++) {
        if (!adj[up * n + b_tmp[bi]]) { covers_b = false; break; }
      }
      if (covers_b) a_tmp[a_len++] = up;
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
        free(a_tmp); free(b_tmp); free(adj);
        return false;
      }
      memcpy(best_a, a_tmp, a_len * sizeof(*best_a));
      memcpy(best_b, b_tmp, b_len * sizeof(*best_b));
    }
  }

  free(a_tmp); free(b_tmp); free(adj);

  if (best_score == 0) return false;
  *a_out = best_a; *a_len_out = best_a_len;
  *b_out = best_b; *b_len_out = best_b_len;
  *label_out = best_label;
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
                             bool emit_metadata,
                             uint32_t min_side, int64_t min_savings,
                             qsop_wmc_stats_t *stats_out,
                             qsop_error_t *error) {
  uint32_t *a_verts = NULL, *b_verts = NULL;
  uint32_t a_len = 0, b_len = 0, block_label = 0;
  const bool has_block = find_block(qsop, min_side, min_savings,
                                     &a_verts, &a_len, &b_verts, &b_len, &block_label);

  if (!has_block) {
    /* Fallback: amp-soft on the full instance */
    wmc_factor_graph_t fg = {0};
    if (!fg_from_qsop(qsop, 1U, &fg, error)) return false;
    const bool ok = write_amp_soft(file, qsop, &fg, emit_metadata, stats_out, error);
    fg_free(&fg);
    return ok;
  }

  const uint32_t r = qsop->r;

  /* Build a mark set for vertices in A and B. */
  bool *in_a = calloc(qsop->nvars, sizeof(*in_a));
  bool *in_b = calloc(qsop->nvars, sizeof(*in_b));
  if (!in_a || !in_b) {
    free(in_a); free(in_b); free(a_verts); free(b_verts);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }
  for (uint32_t i = 0; i < a_len; i++) in_a[a_verts[i]] = true;
  for (uint32_t i = 0; i < b_len; i++) in_b[b_verts[i]] = true;

  /* Build a mark of block edges (u in A, v in B, label == block_label). */
  bool *is_block_edge = calloc(qsop->nedges, sizeof(*is_block_edge));
  if (!is_block_edge) {
    free(in_a); free(in_b); free(a_verts); free(b_verts);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    const uint32_t u = qsop->edge_u[e], v = qsop->edge_v[e];
    const uint32_t lbl = r / 2U;
    if (lbl == block_label &&
        ((in_a[u] && in_b[v]) || (in_a[v] && in_b[u]))) {
      is_block_edge[e] = true;
    }
  }

  /* Global factor from constant phase: omega^(constant mod r) */
  double global_re, global_im;
  omega_power((uint32_t)(qsop->constant % r), r, &global_re, &global_im);

  wmc_builder_t b = {0};
  b.nvars = qsop->nvars;

  /* Build parity literals for each side. For sign edges,
     (r/2) * |A_true| * |B_true| is non-zero iff both parities are odd. */
  int *a_dimacs = malloc(a_len * sizeof(*a_dimacs));
  int *b_dimacs = malloc(b_len * sizeof(*b_dimacs));
  if (!a_dimacs || !b_dimacs) {
    free(a_dimacs); free(b_dimacs);
    free(in_a); free(in_b); free(a_verts); free(b_verts); free(is_block_edge);
    builder_free(&b);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }
  for (uint32_t i = 0; i < a_len; i++) a_dimacs[i] = (int)(a_verts[i] + 1U);
  for (uint32_t i = 0; i < b_len; i++) b_dimacs[i] = (int)(b_verts[i] + 1U);

  const uint32_t parity_aux = (a_len > 0U ? a_len - 1U : 0U) +
                              (b_len > 0U ? b_len - 1U : 0U);
  const int parity_a = build_parity_lit(&b, a_dimacs, a_len);
  const int parity_b = build_parity_lit(&b, b_dimacs, b_len);
  const int block_var = new_var(&b);
  add_clause2(&b, -block_var, parity_a);
  add_clause2(&b, -block_var, parity_b);

  /* Non-block pair auxiliaries (amp-soft style) */
  uint32_t n_extra = 0;
  int *extra_var = malloc(qsop->nedges * sizeof(*extra_var));
  if (!extra_var) {
    free(extra_var);
    free(a_dimacs); free(b_dimacs);
    free(in_a); free(in_b); free(a_verts); free(b_verts); free(is_block_edge);
    builder_free(&b);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }
  for (uint32_t e = 0; e < qsop->nedges; e++) {
    if (is_block_edge[e]) continue;
    const int y = new_var(&b);
    add_clause2(&b, -y, (int)(qsop->edge_u[e] + 1U));
    add_clause2(&b, -y, (int)(qsop->edge_v[e] + 1U));
    extra_var[n_extra] = y;
    n_extra++;
  }

  if (b.failed) {
    free(extra_var);
    free(a_dimacs); free(b_dimacs);
    free(in_a); free(in_b); free(a_verts); free(b_verts); free(is_block_edge);
    builder_free(&b);
    set_error(error, "out of memory building amp-block CNF");
    return false;
  }

  /* Write WPCNF */
  fprintf(file, "p cnf %" PRIu32 " %" PRIu64 "\n", b.nvars, b.nclauses);
  if (emit_metadata) {
    fprintf(file,
            "c sop2wmc encoding=amp-block r=%" PRIu32 " nvars=%" PRIu32 " nedges=%" PRIu32
            " format=qsop-sign norm_h=%" PRIu64 "\n",
            r, qsop->nvars, qsop->nedges, qsop->norm_h);
    fprintf(file, "c block sign-parity label=%" PRIu32 " a_size=%" PRIu32 " b_size=%" PRIu32 "\n",
            block_label, a_len, b_len);
    fprintf(file, "c amplitude_factor %.17g+%.17gi\n", global_re, global_im);
    fputs("c amplitude = ganak_output * amplitude_factor\n", file);
    fputs("c invoke: ganak --mode 6 --verb 0 <this-file>\n", file);
    for (uint32_t v = 0; v < qsop->nvars; v++) {
      fprintf(file, "c xvar %" PRIu32 " %" PRIu32 "\n", v, v + 1U);
    }
  }

  /* Unary weights */
  for (uint32_t v = 0; v < qsop->nvars; v++) {
    const uint32_t coeff = qsop->unary[v] % r;
    if (coeff != 0) {
      double re, im;
      omega_power(coeff, r, &re, &im);
      write_weight(file, (int)(v + 1U), re, im);
    }
  }

  /* Sign block factor: when both side parities are odd, 1 + W(block_var=1) = -1. */
  write_weight(file, block_var, -2.0, 0.0);

  /* Non-block sign edge factors. */
  for (uint32_t i = 0; i < n_extra; i++) {
    write_weight(file, extra_var[i], -2.0, 0.0);
  }

  /* Clauses */
  for (size_t ci = 0; ci < b.len; ci++) {
    if (b.lits[ci] == 0) {
      fputs("0\n", file);
    } else {
      fprintf(file, "%d ", b.lits[ci]);
    }
  }

  if (stats_out != NULL) {
    stats_out->aux_vars = b.nvars - qsop->nvars;
    stats_out->clauses_unit = 0;
    stats_out->clauses_binary = 2U * (1U + n_extra);
    stats_out->clauses_ternary = 4U * parity_aux;
    stats_out->encoded_edges = (uint32_t)(a_len * b_len) + n_extra;
    stats_out->skipped_edges = 0;
  }

  free(extra_var);
  free(a_dimacs); free(b_dimacs);
  free(in_a); free(in_b); free(a_verts); free(b_verts); free(is_block_edge);
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

  if (options->encoding == QSOP_WMC_ENCODING_AMP_BLOCK) {
    return write_amp_block(file, qsop, options->emit_metadata,
                           options->block_min_side, options->block_min_savings,
                           options->stats_out, error);
  }

  if (options->encoding == QSOP_WMC_ENCODING_AMPLITUDE ||
      options->encoding == QSOP_WMC_ENCODING_AMP_SOFT) {
    wmc_factor_graph_t fg = {0};
    if (!fg_from_qsop(qsop, 1U, &fg, error)) {
      return false;
    }

    if (options->preprocess == QSOP_WMC_PREPROCESS_PEEL1 ||
        options->preprocess == QSOP_WMC_PREPROCESS_PEEL2_SAFE) {
      fg_peel1(&fg);
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
    const uint32_t r = qsop->r;
    qsop_wmc_encoding_t inner = options->fourier_inner;
    /* Default inner encoding: amp-soft. */
    if (inner != QSOP_WMC_ENCODING_AMPLITUDE && inner != QSOP_WMC_ENCODING_AMP_SOFT) {
      inner = QSOP_WMC_ENCODING_AMP_SOFT;
    }

    for (uint32_t t = 0; t < r; t++) {
      if (options->emit_metadata) {
        fprintf(file, "c --- fourier t=%" PRIu32 " r=%" PRIu32 " ---\n", t, r);
      }

      if (t == 0) {
        /* Z_0 = 2^n (all assignments contribute omega^0 = 1). Skip Ganak. */
        fprintf(file, "p cnf %" PRIu32 " 0\n", qsop->nvars);
        if (options->emit_metadata) {
          fprintf(file,
                  "c sop2wmc encoding=fourier-t0 r=%" PRIu32 " nvars=%" PRIu32
                  " t=0 z0_log2=%" PRIu32 "\n",
                  r, qsop->nvars, qsop->nvars);
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

      if (options->preprocess == QSOP_WMC_PREPROCESS_PEEL1 ||
          options->preprocess == QSOP_WMC_PREPROCESS_PEEL2_SAFE) {
        fg_peel1(&fg);
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
