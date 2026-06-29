#ifndef DLX4SOP_RESIDUAL_H
#define DLX4SOP_RESIDUAL_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct qsop_residual qsop_residual_t;

bool qsop_residual_create(const qsop_instance_t *qsop, qsop_residual_t **out,
                          qsop_error_t *error);

void qsop_residual_free(qsop_residual_t *residual);

size_t qsop_residual_checkpoint(const qsop_residual_t *residual);

bool qsop_residual_undo(qsop_residual_t *residual, size_t checkpoint, qsop_error_t *error);

bool qsop_residual_branch(qsop_residual_t *residual, uint32_t v, uint8_t value,
                          qsop_error_t *error);

uint32_t qsop_residual_modulus(const qsop_residual_t *residual);

uint32_t qsop_residual_nvars(const qsop_residual_t *residual);

uint32_t qsop_residual_nedges(const qsop_residual_t *residual);

uint32_t qsop_residual_active_vars(const qsop_residual_t *residual);

uint32_t qsop_residual_active_edges(const qsop_residual_t *residual);

uint32_t qsop_residual_constant(const qsop_residual_t *residual);

uint32_t qsop_residual_unary(const qsop_residual_t *residual, uint32_t v);

uint32_t qsop_residual_edge_u(const qsop_residual_t *residual, uint32_t e);

uint32_t qsop_residual_edge_v(const qsop_residual_t *residual, uint32_t e);

uint64_t qsop_residual_fingerprint(const qsop_residual_t *residual);

uint32_t qsop_residual_active_degree(const qsop_residual_t *residual, uint32_t v);

bool qsop_residual_active_components(const qsop_residual_t *residual, uint32_t *component,
                                     uint32_t *ncomponents, qsop_error_t *error);

bool qsop_residual_components_without_var(const qsop_residual_t *residual, uint32_t removed,
                                          uint32_t *out, qsop_error_t *error);

bool qsop_residual_split_without_var(const qsop_residual_t *residual, uint32_t removed,
                                     uint32_t *ncomponents, uint32_t *largest_component,
                                     qsop_error_t *error);

bool qsop_residual_fill_edges_without_var(const qsop_residual_t *residual, uint32_t removed,
                                          uint64_t *out, qsop_error_t *error);

bool qsop_residual_neighbor_cut_rank(const qsop_residual_t *residual, uint32_t v, uint32_t *out,
                                     qsop_error_t *error);

bool qsop_residual_var_active(const qsop_residual_t *residual, uint32_t v);

bool qsop_residual_edge_active(const qsop_residual_t *residual, uint32_t e);

#endif
