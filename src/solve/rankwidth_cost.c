/* Shared single-Fourier join cost model. Decomposition planning calls this with structural
 * forecasts. The realized join selector calls it with the table sizes and ranks it observes.
 * Keeping the decision in one place prevents the planner and executor from optimizing for
 * different algorithms. */
#include "../core/qsop_internal.h"
#include "rankwidth_internal.h"

#include <limits.h>

/* Nanosecond coefficients fitted from the controlled twist family at k = 6 through 10. The
 * calibration used no real benchmark case. scripts/calibrate_rankwidth_cost.py emits the
 * structural features, realized join times, and robust per-unit summaries needed to refit them.
 * The cache-pressure term models the measured increase in pair cost once the two dense shore
 * spaces outgrow the near caches. */
#define RW_COST_JOIN_FIXED_NS UINT64_C(2500)
#define RW_COST_TABLE_STATE_NS UINT64_C(35)
#define RW_COST_STREAM_PAIR_NS UINT64_C(52)
#define RW_COST_MATERIALIZED_PAIR_NS UINT64_C(153)
#define RW_COST_DENSE_PAIR_NS UINT64_C(54)
#define RW_COST_TWIST_OP_NS UINT64_C(3)
#define RW_COST_TWIST_PLAN_WORD_NS UINT64_C(24)
#define RW_COST_TWIST_BIN_NS UINT64_C(120)
#define RW_COST_STREAM_CACHE_DIVISOR UINT64_C(5)
#define RW_COST_MATERIALIZED_CACHE_DIVISOR UINT64_C(3)
#define RW_COST_DENSE_CACHE_DIVISOR UINT64_C(5)

static uint64_t pow2_saturating(uint32_t exponent) {
  return exponent >= 64U ? UINT64_MAX : UINT64_C(1) << exponent;
}

static uint64_t add_cost(uint64_t fixed, uint64_t coefficient, uint64_t units) {
  return qsop_saturating_add_u64(fixed, qsop_saturating_mul_u64(coefficient, units));
}

static uint64_t pair_cache_cost(uint64_t pairs, uint64_t left_dense, uint64_t right_dense,
                                uint64_t divisor) {
  const uint64_t dense_shores = qsop_saturating_add_u64(left_dense, right_dense);
  const uint64_t pressure = dense_shores / divisor;
  return qsop_saturating_mul_u64(pairs, pressure);
}

static bool within_budget(uint64_t bytes, uint64_t budget) {
  return budget == 0U || bytes <= budget;
}

static bool speedup_margin_wins(uint64_t candidate, uint64_t baseline) {
  const uint64_t quotient = baseline / RW_SINGLE_AUTO_MIN_SPEEDUP_NUM;
  const uint64_t remainder = baseline % RW_SINGLE_AUTO_MIN_SPEEDUP_NUM;
  const uint64_t threshold =
      quotient * RW_SINGLE_AUTO_MIN_SPEEDUP_DEN +
      (remainder * RW_SINGLE_AUTO_MIN_SPEEDUP_DEN) / RW_SINGLE_AUTO_MIN_SPEEDUP_NUM;
  return candidate <= threshold;
}

uint64_t rw_twist_workspace_bytes_dims(uint32_t parent_dim, uint32_t crossing_rank,
                                       size_t value_size) {
  if (parent_dim + crossing_rank >= 64U) {
    return UINT64_MAX;
  }
  const uint64_t n_pc = UINT64_C(1) << (parent_dim + crossing_rank);
  const uint64_t n_p = UINT64_C(1) << parent_dim;
  const uint64_t planes =
      qsop_saturating_mul_u64(qsop_saturating_mul_u64(6U, n_pc), (uint64_t)value_size);
  const uint64_t coords =
      qsop_saturating_mul_u64(qsop_saturating_mul_u64(6U, n_p), (uint64_t)sizeof(uint32_t));
  const uint64_t reach =
      qsop_saturating_mul_u64(qsop_saturating_mul_u64(3U, n_p), (uint64_t)sizeof(uint64_t));
  return qsop_saturating_add_u64(planes, qsop_saturating_add_u64(coords, reach));
}

rw_single_join_forecast_t rw_single_join_forecast(qsop_rankwidth_single_kernel_t kernel,
                                                  uint64_t left_entries, uint64_t right_entries,
                                                  uint64_t parent_entries, uint32_t left_dim,
                                                  uint32_t right_dim, uint32_t parent_dim,
                                                  uint32_t crossing_rank, size_t words,
                                                  size_t value_size, uint64_t materialize_max_pairs,
                                                  uint64_t memory_budget_bytes) {
  rw_single_join_forecast_t out = {
      .feasible = true,
      .selected = RW_SINGLE_JOIN_STREAMING,
      .pairwise_ns = UINT64_MAX,
      .twist_ns = UINT64_MAX,
      .selected_ns = UINT64_MAX,
      .left_entries = left_entries,
      .right_entries = right_entries,
      .parent_entries = parent_entries,
      .left_dim = left_dim,
      .right_dim = right_dim,
      .parent_dim = parent_dim,
      .crossing_rank = crossing_rank,
  };
  const uint64_t pairs = qsop_saturating_mul_u64(left_entries, right_entries);
  const uint64_t states =
      qsop_saturating_add_u64(qsop_saturating_add_u64(left_entries, right_entries), parent_entries);
  const uint64_t state_cost = qsop_saturating_mul_u64(RW_COST_TABLE_STATE_NS, states);
  const uint64_t left_dense = pow2_saturating(left_dim);
  const uint64_t right_dense = pow2_saturating(right_dim);
  uint64_t stream_ns = add_cost(RW_COST_JOIN_FIXED_NS, RW_COST_STREAM_PAIR_NS, pairs);
  stream_ns = qsop_saturating_add_u64(
      stream_ns, pair_cache_cost(pairs, left_dense, right_dense, RW_COST_STREAM_CACHE_DIVISOR));
  stream_ns = qsop_saturating_add_u64(stream_ns, state_cost);

  const uint64_t transition_bytes = qsop_saturating_mul_u64(pairs, UINT64_C(16));
  uint64_t materialized_ns = UINT64_MAX;
  if ((materialize_max_pairs == 0U || pairs <= materialize_max_pairs) &&
      within_budget(transition_bytes, memory_budget_bytes)) {
    materialized_ns = add_cost(RW_COST_JOIN_FIXED_NS, RW_COST_MATERIALIZED_PAIR_NS, pairs);
    materialized_ns = qsop_saturating_add_u64(
        materialized_ns,
        pair_cache_cost(pairs, left_dense, right_dense, RW_COST_MATERIALIZED_CACHE_DIVISOR));
    materialized_ns = qsop_saturating_add_u64(materialized_ns, state_cost);
  }

  const uint64_t dense_pairs = qsop_saturating_mul_u64(left_dense, right_dense);
  out.dense_pairs = dense_pairs;
  const uint64_t dense_values = qsop_saturating_add_u64(
      qsop_saturating_add_u64(left_dense, right_dense), pow2_saturating(parent_dim));
  const uint64_t dense_bytes = qsop_saturating_mul_u64(
      dense_values, qsop_saturating_add_u64((uint64_t)(2U * value_size), UINT64_C(16)));
  uint64_t dense_ns = UINT64_MAX;
  if (dense_pairs <= RW_DENSE_REFERENCE_MAX_VALUES &&
      within_budget(dense_bytes, memory_budget_bytes)) {
    dense_ns = add_cost(RW_COST_JOIN_FIXED_NS, RW_COST_DENSE_PAIR_NS, dense_pairs);
    dense_ns =
        qsop_saturating_add_u64(dense_ns, pair_cache_cost(dense_pairs, left_dense, right_dense,
                                                          RW_COST_DENSE_CACHE_DIVISOR));
    dense_ns = qsop_saturating_add_u64(dense_ns, state_cost);
  }

  rw_single_join_kind_t pair_kind = RW_SINGLE_JOIN_STREAMING;
  uint64_t pair_ns = stream_ns;
  uint64_t pair_bytes = 0U;
  if (materialized_ns < pair_ns) {
    pair_kind = RW_SINGLE_JOIN_MATERIALIZED;
    pair_ns = materialized_ns;
    pair_bytes = transition_bytes;
  }
  if (dense_ns < pair_ns) {
    pair_kind = RW_SINGLE_JOIN_DENSE;
    pair_ns = dense_ns;
    pair_bytes = dense_bytes;
  }
  out.pairwise_ns = pair_ns;
  out.pairwise_workspace_bytes = pair_bytes;

  if (parent_dim + crossing_rank <= RW_TWIST_MAX_DIM &&
      (kernel != QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO || pairs >= RW_TWIST_AUTO_MIN_PAIRS)) {
    const uint32_t pc = parent_dim + crossing_rank;
    const uint64_t transform_ops = qsop_saturating_add_u64(
        qsop_saturating_mul_u64(pow2_saturating(pc), (uint64_t)pc + 1U),
        qsop_saturating_mul_u64(pow2_saturating(parent_dim), (uint64_t)parent_dim + 1U));
    out.twist_ops = qsop_saturating_add_u64(transform_ops,
                                            qsop_saturating_add_u64(left_entries, right_entries));
    out.twist_workspace_bytes =
        rw_twist_workspace_bytes_dims(parent_dim, crossing_rank, value_size);
    const uint64_t plan_units = qsop_saturating_mul_u64(
        (uint64_t)words, (uint64_t)left_dim + right_dim + parent_dim + crossing_rank + 1U);
    uint64_t twist_ns = add_cost(RW_COST_JOIN_FIXED_NS, RW_COST_TWIST_OP_NS, out.twist_ops);
    twist_ns = qsop_saturating_add_u64(
        twist_ns, qsop_saturating_mul_u64(RW_COST_TWIST_PLAN_WORD_NS, plan_units));
    twist_ns =
        qsop_saturating_add_u64(twist_ns, qsop_saturating_mul_u64(RW_COST_TWIST_BIN_NS, states));
    out.twist_ns = twist_ns;
    uint64_t twist_budget = memory_budget_bytes;
    if (kernel == QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO &&
        (twist_budget == 0U || twist_budget > RW_TWIST_AUTO_MAX_BYTES)) {
      twist_budget = RW_TWIST_AUTO_MAX_BYTES;
    }
    out.twist_feasible = within_budget(out.twist_workspace_bytes, twist_budget);
  }

  switch (kernel) {
  case QSOP_RANKWIDTH_SINGLE_KERNEL_STREAMING:
    out.selected = RW_SINGLE_JOIN_STREAMING;
    out.selected_ns = stream_ns;
    out.selected_workspace_bytes = 0U;
    break;
  case QSOP_RANKWIDTH_SINGLE_KERNEL_MATERIALIZED:
    out.selected = RW_SINGLE_JOIN_MATERIALIZED;
    out.selected_ns = materialized_ns;
    out.selected_workspace_bytes = transition_bytes;
    out.feasible = materialized_ns != UINT64_MAX;
    break;
  case QSOP_RANKWIDTH_SINGLE_KERNEL_DENSE:
    out.selected = RW_SINGLE_JOIN_DENSE;
    out.selected_ns = dense_ns;
    out.selected_workspace_bytes = dense_bytes;
    out.feasible = dense_ns != UINT64_MAX;
    break;
  case QSOP_RANKWIDTH_SINGLE_KERNEL_TWIST:
    out.selected = RW_SINGLE_JOIN_TWIST;
    out.selected_ns = out.twist_ns;
    out.selected_workspace_bytes = out.twist_workspace_bytes;
    out.feasible = out.twist_feasible;
    break;
  case QSOP_RANKWIDTH_SINGLE_KERNEL_PAIRWISE:
    out.selected = pair_kind;
    out.selected_ns = pair_ns;
    out.selected_workspace_bytes = pair_bytes;
    break;
  case QSOP_RANKWIDTH_SINGLE_KERNEL_AUTO:
  default:
    out.selected = pair_kind;
    out.selected_ns = pair_ns;
    out.selected_workspace_bytes = pair_bytes;
    if (out.twist_feasible && speedup_margin_wins(out.twist_ns, pair_ns)) {
      out.selected = RW_SINGLE_JOIN_TWIST;
      out.selected_ns = out.twist_ns;
      out.selected_workspace_bytes = out.twist_workspace_bytes;
    }
    break;
  }
  return out;
}
