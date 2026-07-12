/* Public qsop_solve_rankwidth_... and qsop_rankwidth_decomposition_... entry points that dispatch
 * to the DP families above.
 *
 * Part of the rankwidth.c file split (pure movement, no logic changes) -- see
 * rankwidth_internal.h for the shared types and cross-TU declarations. */
#include "../core/qsop_internal.h"
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop_solve.h"
#include "dlx4sop/residue.h"
#include "dlx4sop/simd.h"
#include "rankwidth_internal.h"
#include "trace.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool qsop_solve_rankwidth_count_table_mod_stats(const qsop_instance_t *qsop,
                                                const qsop_rankwidth_decomposition_t *decomposition,
                                                uint64_t count_modulus, uint64_t *counts,
                                                qsop_solve_stats_t *stats,
                                                qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (qsop == NULL || decomposition == NULL || counts == NULL) {
    qsop_set_error(error, "internal error: null rankwidth modular solve argument");
    return false;
  }
  if (decomposition->nvars != qsop->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (!rw_refuse_large_modulus(qsop, error)) {
    return false;
  }
  qsop_counts_clear((uint32_t)qsop->r, counts);
  if (qsop->nvars == 0) {
    return rw_solve_constant_mod(qsop, count_modulus, counts, stats, trace);
  }
  if (!rw_record_decomposition_diagnostics(qsop, decomposition, stats, trace, error)) {
    return false;
  }

  if (count_modulus == 0) {
    if (qsop->nvars >= 64U) {
      qsop_set_error(error, "rankwidth exact count-table handoff requires fewer than 64 variables");
      return false;
    }
    if (qsop->nedges == 0) {
      return rw_solve_no_edges_count_table_mod_once(qsop, 0, counts, stats, trace, error);
    }
    qsop_result_t *result = NULL;
    uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
    const bool ok = rw_solve_count_table(qsop, decomposition, adj, QSOP_RANKWIDTH_JOIN_AUTO, 0,
                                         &result, stats, trace, error);
    free(adj);
    if (!ok) {
      qsop_result_free(result);
      return false;
    }
    memcpy(counts, result->counts, (size_t)qsop->r * sizeof(*counts));
    qsop_result_free(result);
    return true;
  }
  if (qsop->nedges == 0) {
    return rw_solve_no_edges_count_table_mod_once(qsop, count_modulus, counts, stats, trace, error);
  }

  uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
  if (adj == NULL) {
    return false;
  }
  const bool ok = rw_solve_count_table_mod_once(qsop, decomposition, adj, count_modulus, counts,
                                                stats, trace, error);
  free(adj);
  return ok;
}
bool qsop_solve_rankwidth_options_mode_trace_stats(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, qsop_rankwidth_solve_mode_t mode,
    const qsop_rankwidth_solve_options_t *options, qsop_result_t **out, qsop_solve_stats_t *stats,
    qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    qsop_set_error(error, "internal error: null result pointer");
    return false;
  }
  *out = NULL;
  if (qsop == NULL || decomposition == NULL) {
    qsop_set_error(error, "internal error: null rankwidth solve argument");
    return false;
  }
  if (qsop->nvars != decomposition->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (qsop->nvars > max_vars) {
    qsop_set_error(error,
                   "rankwidth solver refuses %" PRIu32 " variables; pass a larger --max-vars",
                   qsop->nvars);
    return false;
  }
  if (!rw_refuse_large_modulus(qsop, error)) {
    return false;
  }
  if (qsop->nvars == 0) {
    return rw_solve_constant_result(qsop, out, stats, trace, error);
  }
  if (!rw_record_decomposition_diagnostics(qsop, decomposition, stats, trace, error)) {
    return false;
  }
  const qsop_rankwidth_join_strategy_t js =
      (options != NULL) ? options->join_strategy : QSOP_RANKWIDTH_JOIN_AUTO;
  const uint64_t mp = (options != NULL) ? options->materialize_join_max_pairs : 0;
  const qsop_rankwidth_fourier_kernel_t fk =
      (options != NULL) ? options->fourier_kernel : QSOP_RANKWIDTH_FOURIER_KERNEL_AUTO;
  if (qsop->nedges == 0) {
    return mode == QSOP_RANKWIDTH_SOLVE_FOURIER
               ? rw_solve_fourier(qsop, decomposition, NULL, fk, out, stats, trace, error)
               : rw_solve_no_edges_count_table(qsop, out, stats, trace, error);
  }

  uint64_t *adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
  if (adj == NULL) {
    return false;
  }
  /* Fourier path does not support streaming; count-table path does. */
  const bool ok =
      mode == QSOP_RANKWIDTH_SOLVE_FOURIER
          ? rw_solve_fourier(qsop, decomposition, adj, fk, out, stats, trace, error)
          : rw_solve_count_table(qsop, decomposition, adj, js, mp, out, stats, trace, error);
  free(adj);
  return ok;
}
bool qsop_solve_rankwidth_mode_trace_stats(const qsop_instance_t *qsop,
                                           const qsop_rankwidth_decomposition_t *decomposition,
                                           uint32_t max_vars, qsop_rankwidth_solve_mode_t mode,
                                           qsop_result_t **out, qsop_solve_stats_t *stats,
                                           qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_rankwidth_options_mode_trace_stats(qsop, decomposition, max_vars, mode, NULL,
                                                       out, stats, trace, error);
}
bool qsop_solve_rankwidth_single_mode_options(const qsop_instance_t *qsop,
                                              const qsop_rankwidth_decomposition_t *decomposition,
                                              uint32_t max_vars, uint32_t target_mode,
                                              const qsop_rankwidth_single_mode_options_t *options,
                                              qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                              qsop_solve_trace_t *trace, qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    qsop_set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL || decomposition == NULL) {
    qsop_set_error(error, "internal error: null rankwidth solve argument");
    return false;
  }
  if (qsop->r == 0) {
    qsop_set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }
  if (qsop->nvars != decomposition->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (qsop->nvars > max_vars) {
    qsop_set_error(error,
                   "rankwidth solver refuses %" PRIu32 " variables; pass a larger --max-vars",
                   qsop->nvars);
    return false;
  }
  if (qsop->nvars == 0) {
    long double c_re = 0.0L;
    long double c_im = 0.0L;
    qsop_root_of_unity_l(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
    out->re = c_re;
    out->im = c_im;
    return true;
  }
  if (!rw_record_decomposition_diagnostics(qsop, decomposition, stats, trace, error)) {
    return false;
  }

  uint64_t *adj = NULL;
  if (qsop->nedges != 0) {
    adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
  }

  long double re = 0.0L;
  long double im = 0.0L;
  long double numeric_error_bound = 0.0L;
  int scale_exp2 = 0;
  const qsop_rankwidth_single_mode_options_t o =
      options != NULL ? *options : (qsop_rankwidth_single_mode_options_t){0};
  const bool ok = rw_solve_single_mode_once(qsop, decomposition, adj, target_mode, &scale_exp2, &re,
                                            &im, &numeric_error_bound, o.kernel,
                                            o.materialize_join_max_pairs, stats, trace, error);
  free(adj);
  if (!ok) {
    return false;
  }
  out->re = re;
  out->im = im;
  out->scale_exp2 = scale_exp2;
  out->numeric_error_bound = numeric_error_bound;
  return true;
}
bool qsop_solve_rankwidth_single_mode(const qsop_instance_t *qsop,
                                      const qsop_rankwidth_decomposition_t *decomposition,
                                      uint32_t max_vars, uint32_t target_mode,
                                      qsop_amplitude_t *out, qsop_solve_stats_t *stats,
                                      qsop_solve_trace_t *trace, qsop_error_t *error) {
  return qsop_solve_rankwidth_single_mode_options(qsop, decomposition, max_vars, target_mode, NULL,
                                                  out, stats, trace, error);
}
bool qsop_solve_rankwidth_single_mode_f64_options(
    const qsop_instance_t *qsop, const qsop_rankwidth_decomposition_t *decomposition,
    uint32_t max_vars, uint32_t target_mode, const qsop_rankwidth_single_mode_options_t *options,
    qsop_amplitude_t *out, qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
    qsop_error_t *error) {
  if (stats != NULL) {
    *stats = (qsop_solve_stats_t){0};
  }
  if (out == NULL) {
    qsop_set_error(error, "internal error: null amplitude result pointer");
    return false;
  }
  *out = (qsop_amplitude_t){0};
  if (qsop == NULL || decomposition == NULL) {
    qsop_set_error(error, "internal error: null rankwidth solve argument");
    return false;
  }
  if (qsop->r == 0) {
    qsop_set_error(error, "internal error: QSOP instance has a zero modulus");
    return false;
  }
  if (qsop->nvars != decomposition->nvars) {
    qsop_set_error(error, "rankwidth decomposition variable count does not match QSOP");
    return false;
  }
  if (qsop->nvars > max_vars) {
    qsop_set_error(error,
                   "rankwidth solver refuses %" PRIu32 " variables; pass a larger --max-vars",
                   qsop->nvars);
    return false;
  }
  if (qsop->nvars == 0) {
    double c_re = 0.0;
    double c_im = 0.0;
    qsop_root_of_unity_f64(qsop->r, target_mode, qsop->constant % qsop->r, &c_re, &c_im);
    out->re = (long double)c_re;
    out->im = (long double)c_im;
    if (stats != NULL) {
      stats->rankwidth_single_complex_kernel = 2U;
    }
    return true;
  }
  if (!rw_record_decomposition_diagnostics(qsop, decomposition, stats, trace, error)) {
    return false;
  }

  uint64_t *adj = NULL;
  if (qsop->nedges != 0) {
    adj = rw_adjacency_bitsets(qsop, decomposition->words, error);
    if (adj == NULL) {
      return false;
    }
  }

  long double re = 0.0L;
  long double im = 0.0L;
  long double numeric_error_bound = 0.0L;
  int scale_exp2 = 0;
  const qsop_rankwidth_single_mode_options_t o =
      options != NULL ? *options : (qsop_rankwidth_single_mode_options_t){0};
  const bool ok = rw_solve_single_mode_once_f64(
      qsop, decomposition, adj, target_mode, &scale_exp2, &re, &im, &numeric_error_bound, o.kernel,
      o.materialize_join_max_pairs, o.simd, stats, trace, error);
  free(adj);
  if (!ok) {
    return false;
  }
  out->re = re;
  out->im = im;
  out->scale_exp2 = scale_exp2;
  out->numeric_error_bound = numeric_error_bound;
  return true;
}
bool qsop_solve_rankwidth_single_mode_f64(const qsop_instance_t *qsop,
                                          const qsop_rankwidth_decomposition_t *decomposition,
                                          uint32_t max_vars, uint32_t target_mode,
                                          const qsop_simd_vtable_t *simd, qsop_amplitude_t *out,
                                          qsop_solve_stats_t *stats, qsop_solve_trace_t *trace,
                                          qsop_error_t *error) {
  const qsop_rankwidth_single_mode_options_t options = {
      .simd = simd,
  };
  return qsop_solve_rankwidth_single_mode_f64_options(qsop, decomposition, max_vars, target_mode,
                                                      &options, out, stats, trace, error);
}
