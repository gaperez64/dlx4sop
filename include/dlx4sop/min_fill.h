#ifndef DLX4SOP_MIN_FILL_H
#define DLX4SOP_MIN_FILL_H

#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h" /* qsop_treewidth_order_t */

#include <stdbool.h>
#include <stdint.h>

/* Greedy elimination-order heuristic over the sparse support graph given by the undirected
 * edge list (self-loops and duplicate edges are ignored, matching the dense bitset builders).
 *
 * Selection each step reproduces the dense implementations byte-for-byte: minimize fill, break
 * ties by degree (ascending for MIN_FILL, descending for MIN_FILL_MAX_DEGREE; MIN_DEGREE orders
 * by degree first then fill), then by lowest vertex index.
 *
 * Outputs (any of order/fill/dp_work may be NULL):
 *   *width_out       = max elimination degree (treewidth upper bound)
 *   order_out[0..n-1]= elimination order
 *   *fill_edges_out  = total fill edges added
 *   *dp_work_out     = sum over elimination steps of 2^(bag size), saturating at UINT64_MAX.
 *                      This is the number of DP table entries a treewidth solve over this order
 *                      touches, and is the quantity a cost model wants: the usual
 *                      nvars * 2^(width+1) bound assumes every bag is as wide as the widest one,
 *                      and overstates the real work by an order of magnitude or more on circuit
 *                      graphs, where a handful of bags carry the width.
 *
 * If width_abort_threshold != UINT32_MAX, elimination stops as soon as the recorded width would
 * exceed the threshold; *width_capped_out is set true and order/fill are left partial. With the
 * default UINT32_MAX the full order is produced and *width_capped_out is false.
 *
 * Returns false only on allocation failure. */
bool qsop_min_fill_eliminate(uint32_t nvars, const uint32_t *edge_u, const uint32_t *edge_v,
                             uint32_t nedges, qsop_treewidth_order_t tie_break,
                             uint32_t width_abort_threshold, uint32_t *order_out,
                             uint32_t *width_out, uint64_t *fill_edges_out, uint64_t *dp_work_out,
                             bool *width_capped_out, qsop_error_t *error);

/* Exact prefix-cut-rank of the support graph along the natural variable order: the maximum over
 * cuts c in 1..nvars-1 of the GF(2) rank of the bipartite adjacency between {0..c-1} and
 * {c..nvars-1}. Computed incrementally (each cut moves one vertex from the right side to the
 * left, i.e. adds one row and deletes one column), so it is ~O(nvars * rank * nvars/64) instead
 * of the dense O(nvars^4/64) rebuild-per-cut. Byte-identical result to the dense computation.
 * Returns false only on allocation failure. */
bool qsop_prefix_cut_rank(uint32_t nvars, const uint32_t *edge_u, const uint32_t *edge_v,
                          uint32_t nedges, uint32_t *out_rank, qsop_error_t *error);

#endif
