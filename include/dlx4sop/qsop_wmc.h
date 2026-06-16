#ifndef DLX4SOP_QSOP_WMC_H
#define DLX4SOP_QSOP_WMC_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Export of a QSOP instance to DIMACS CNF / WPCNF for external counters.
 *
 * Two encodings are available:
 *
 * QSOP_WMC_ENCODING_RESIDUE (plain #SAT, r separate CNF blocks):
 *   For target residue k, the models of the emitted CNF correspond exactly to
 *   assignments x in {0,1}^nvars with phase(x) == k. A plain #SAT counter
 *   returns counts[k]; the full amplitude is reconstructed as
 *     amplitude = sum_k counts[k] * exp(2*pi*i*k/r).
 *
 * QSOP_WMC_ENCODING_AMPLITUDE (complex literal weights, single WPCNF call):
 *   Each free variable x_v carries the literal weight omega^unary[v] (true) / 1
 *   (false), and each Tseitin AND var y_e carries omega^edge_q[e] / 1. A single
 *   complex weighted model count equals
 *     sum_x omega^(unary-part + quadratic-part),
 *   and the full amplitude is obtained by multiplying by omega^constant (written
 *   as `c amplitude_factor` metadata in the CNF). Use Ganak --mode 6.
 *
 * Both encodings work for sign and labelled QSOPs without special-casing.
 */

typedef enum {
  QSOP_WMC_ENCODING_RESIDUE,    /* mod-r adder + plain #SAT, r CNF blocks */
  QSOP_WMC_ENCODING_AMPLITUDE,  /* complex literal weights, single WPCNF */
} qsop_wmc_encoding_t;

typedef struct qsop_wmc_options {
  qsop_wmc_encoding_t encoding; /* which encoding to emit */
  bool all_residues;    /* RESIDUE: emit one CNF block per residue 0..r-1 */
  uint32_t residue;     /* RESIDUE: residue to emit when !all_residues */
  bool emit_metadata;   /* prefix each block with `c` comment metadata */
} qsop_wmc_options_t;

/* Sensible defaults: residue encoding, all residues, metadata on. */
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
