#ifndef DLX4SOP_QPF_MAGIC_H
#define DLX4SOP_QPF_MAGIC_H

#include "dlx4sop/qpf.h"
#include "dlx4sop/qsop_solve.h"

typedef struct qsop_qpf_term_list {
  qsop_qpf_t *terms;
  uint64_t len;
} qsop_qpf_term_list_t;

bool qsop_qpf_phase_order_supported(uint64_t r, uint32_t max_order);
uint64_t qsop_qpf_naive_term_bound(uint32_t magic_vertices);
uint64_t qsop_qpf_stabilizer_term_bound(const qsop_instance_t *qsop, uint64_t target_mode);
bool qsop_qpf_magic_decompose(const qsop_instance_t *qsop, uint64_t target_mode,
                              uint64_t max_terms, const qsop_qpf_scalar_t *unit,
                              qsop_qpf_term_list_t *out, qsop_error_t *error);
void qsop_qpf_term_list_free(qsop_qpf_term_list_t *list);
bool qsop_qpf_term_list_total(const qsop_qpf_term_list_t *list, qsop_qpf_scalar_t *out,
                              qsop_error_t *error);

#endif
