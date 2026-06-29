#ifndef DLX4SOP_QSOP_WMC_H
#define DLX4SOP_QSOP_WMC_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Export of a QSOP instance to DIMACS CNF / WPCNF for external counters.
 *
 * Encodings:
 *
 * QSOP_WMC_ENCODING_RESIDUE (plain #SAT, residue-accumulator):
 *   Mod-r adder + plain #SAT, r CNF blocks. Debug/reference path.
 *   amplitude = sum_k counts[k] * exp(2*pi*i*k/r).
 *
 * QSOP_WMC_ENCODING_AMPLITUDE / QSOP_WMC_ENCODING_AMP_AND (amp-and):
 *   Deterministic amplitude encoding. Each sign edge x_u*x_v*(r/2) gets a
 *   Tseitin AND auxiliary y_e with W(y_e=1)=-1, W(y_e=0)=1. Three clauses
 *   per encoded edge (2 binary + 1 ternary). Use Ganak --mode 6.
 *
 * QSOP_WMC_ENCODING_AMP_SOFT (amp-soft):
 *   Soft-feature amplitude encoding. Each edge gets an implication auxiliary
 *   y_e with only y_e->x_u and y_e->x_v (2 binary clauses, no ternary).
 *   W(y_e=1) = omega^b - 1, W(y_e=0) = 1. Reduces ternary clause count.
 *   amplitude = ganak_output * amplitude_factor.
 *
 * Amplitude encodings are specialized for signed QSOP edges.
 */

typedef enum {
  QSOP_WMC_ENCODING_RESIDUE,          /* mod-r adder + plain #SAT (residue-accumulator) */
  QSOP_WMC_ENCODING_AMPLITUDE,        /* Tseitin AND aux, complex weights (amp-and) */
  QSOP_WMC_ENCODING_AMP_SOFT,         /* implication aux, (omega^b - 1) weights (amp-soft) */
  QSOP_WMC_ENCODING_RESIDUE_FOURIER,  /* r WPCNF blocks (one per Fourier exponent t) + outer iDFT */
  QSOP_WMC_ENCODING_AMP_BLOCK,        /* block counter + residue-pair selectors, amp-soft fallback */
  QSOP_WMC_ENCODING_AMP_AND = QSOP_WMC_ENCODING_AMPLITUDE,  /* alias */
} qsop_wmc_encoding_t;

/* Structural statistics collected during WMC export. */
typedef struct qsop_wmc_stats {
  uint32_t aux_vars;         /* auxiliary variables introduced */
  uint64_t clauses_unit;     /* unit/forcing clauses */
  uint64_t clauses_binary;   /* binary clauses */
  uint64_t clauses_ternary;  /* ternary clauses */
  uint32_t encoded_edges;    /* edges with non-trivial auxiliary */
  uint32_t skipped_edges;    /* sign edges absorbed or canceled before export */
} qsop_wmc_stats_t;

/*
 * WMC preprocessing applied inside the exporter after complex weights are
 * assigned. This never writes back to the QSOP core.
 *
 * NONE:      No preprocessing; factor graph is written as-is.
 * PEEL1:     Degree-0 and degree-1 variable elimination. Absorbed variables
 *            are summed out analytically: degree-0 multiplies the global factor
 *            by (1 + w_true[v]); degree-1 absorbs the x-sum into the neighbor's
 *            unary weight. Zero-factor cases force the neighbor to true/false or
 *            short-circuit to amplitude=0.
 * PEEL2_SAFE: PEEL1 plus degree-2 elimination when all factor denominators are
 *             nonzero and the fill budget is not exceeded. Behind a flag; not
 *             default until benchmarked.
 */
typedef enum {
  QSOP_WMC_PREPROCESS_NONE = 0,
  QSOP_WMC_PREPROCESS_PEEL1 = 1,
  QSOP_WMC_PREPROCESS_PEEL2_SAFE = 2,
} qsop_wmc_preprocess_t;

typedef struct qsop_wmc_options {
  qsop_wmc_encoding_t encoding; /* which encoding to emit */
  bool all_residues;    /* RESIDUE: emit one CNF block per residue 0..r-1 */
  uint32_t residue;     /* RESIDUE: residue to emit when !all_residues */
  bool emit_metadata;   /* prefix each block with `c` comment metadata */
  qsop_wmc_preprocess_t preprocess; /* factor-graph preprocessing before WPCNF write */
  uint32_t peel2_fill_budget;  /* PEEL2_SAFE: max fill edges to create (default 0 = unlimited) */
  /* RESIDUE_FOURIER: inner amplitude encoding for each Fourier exponent block.
   * Accepts AMPLITUDE (amp-and) or AMP_SOFT. Defaults to AMP_SOFT. */
  qsop_wmc_encoding_t fourier_inner;
  qsop_wmc_stats_t *stats_out; /* if non-NULL, filled with structural stats */
  uint32_t block_min_side;    /* AMP_BLOCK: min |A|, |B| for a block to qualify (default 4) */
  int64_t  block_min_savings; /* AMP_BLOCK: min (|A|*|B| - est_block_cost) to trigger (default 0) */
} qsop_wmc_options_t;

/* Sensible defaults: residue encoding, all residues, metadata on, no preprocess, no stats. */
qsop_wmc_options_t qsop_wmc_options_default(void);

/*
 * Write the WMC CNF for `qsop` to `file`. For residue encoding, when
 * `options->all_residues` is true the output is a multi-document stream with
 * one standalone DIMACS CNF per residue, each introduced by a
 * `c --- residue k ---` marker. For amplitude encoding a single WPCNF is
 * written regardless of `all_residues`. Returns false and fills `error` on
 * bad arguments, an out-of-range residue, or a write failure.
 */
bool qsop_wmc_write(FILE *file, const qsop_instance_t *qsop,
                    const qsop_wmc_options_t *options, qsop_error_t *error);

#endif
