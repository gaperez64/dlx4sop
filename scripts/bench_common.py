#!/usr/bin/env python3
"""Shared helpers for scripts/bench_branch.py and scripts/bench_gauntlet.py.

Both scripts drive the same qasm2sop exact-then-approx import contract (the
only thing `--approx` can rescue is a phase/angle outside qasm2sop's exact
grid -- an "angle" or "non-sign quadratic" refusal) and instrument sop-solve
identically; this module is the single place that contract lives so the two
scripts' output tables stay column-for-column comparable.

Not a standalone CLI.
"""

from __future__ import annotations

import resource
import subprocess

# Matches .gauntlet/adapter.py: the only thing --approx can rescue is an angle
# outside qasm2sop's exact grid.
APPROX_EPSILON = 1e-8

# Counters worth a column. `search_nodes` and the cache pair say whether the
# recursion was entered at all and whether the memo is working; the rest say
# which sub-solver actually did the work.
STAT_KEYS = [
    "solve_mode_kernel",
    "search_nodes",
    "cache_hits",
    "cache_misses",
    "cache_entries",
    "cache_estimated_bytes",
    "treewidth_delegations",
    "rankwidth_delegations",
    "branch_fallthroughs",
    "branch_treewidth_skips",
    "branch_rankwidth_skips",
    "branch_propagations",
    "branch_zero_prunes",
    "branch_width_probes",
    "branch_probe_skips",
    "branch_cutset_size",
    "branch_materialized_calls",
    "branch_materialized_eliminations",
    "branch_materialized_degree2_merges",
    "branch_materialized_reduction_ns",
    "branch_conditioning_nodes",
    "branch_conditioning_lookaheads",
    "branch_delegate_probes",
    "branch_delegate_probe_skips",
    "branch_max_cutset_depth",
    "treewidth_factor_scope_tests",
    "treewidth_factor_bucket_visits",
    "treewidth_factor_multiplications",
    "treewidth_factor_allocations",
    "treewidth_factor_discovery_ns",
    "treewidth_numeric_join_ns",
    "treewidth_sum_out_ns",
    "treewidth_peak_live_bytes",
    "treewidth_pool_retained_bytes",
    "treewidth_largest_allocation_bytes",
    "max_residual_vars",
    "max_residual_min_fill_width",
    "max_residual_prefix_cut_rank",
    "decomposition_width",
    "rankwidth_cutrank_width",
    "rankwidth_table_forecast",
    "rankwidth_join_pair_forecast",
    "table_entries",
    "max_table_entries",
    "join_pairs",
    "simd_vectorized_ops",
    "simd_scalar_fallback_ops",
]

SHAPE_KEYS = [
    "modulus",
    "variables",
    "quadratic_terms",
    "components",
    "min_fill_width",
    "min_fill_dp_work",
    "prefix_cut_rank",
]


def refusal_reason(stderr: str) -> str:
    """A stable slug per refusal, so rows stay comparable across stages."""
    table = [
        ("fallback refused component", "max_fallback_vars"),
        ("search-node cap exceeded", "max_search_nodes"),
        ("recursion-depth cap", "max_recursion_depth"),
        ("cutset budget", "cutset_budget"),
        ("no delegate available", "no_delegate"),
        ("exceeds", "root_sanity"),
        ("pass a larger --max-vars", "max_vars"),
        ("memory-skip", "memory_skip"),
        ("out of memory", "oom"),
    ]
    for needle, slug in table:
        if needle in stderr:
            return slug
    return "other"


def run(cmd: list[str], *, stdin: str | None = None, timeout: float | None = None,
        preexec_fn=None):
    return subprocess.run(cmd, input=stdin, check=False, capture_output=True, text=True,
                          timeout=timeout, preexec_fn=preexec_fn)


def address_space_limiter(limit_bytes: int):
    """A `preexec_fn` that caps the child's RLIMIT_AS, or None if `limit_bytes` disables it.

    A runaway wide DP then fails its own allocation -- recorded as a refusal/kill -- instead of
    inviting the kernel OOM killer on a shared machine.
    """
    if limit_bytes <= 0:
        return None

    def _apply():
        resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))

    return _apply


def import_qsop(qasm2sop: str, qasm: str, zero: str, timeout: float, manifest_tool,
                exact_only: bool = False) -> tuple[str, str, str, str]:
    """Import `qasm` at the all-zero boundary `zero`: exact first, `--approx` only when the
    exact failure is a phase/angle-representability one (the only thing --approx can rescue).

    Returns (qsop_text, mode, error_class, diagnostic); error_class/diagnostic are only
    populated when the exact attempt failed (empty strings on a clean exact import).
    """
    exact = run([qasm2sop, "--input", zero, "--output", zero, "-"], stdin=qasm, timeout=timeout)
    if exact.returncode == 0:
        return exact.stdout, "exact", "", ""
    if "angle" not in exact.stderr and "non-sign quadratic" not in exact.stderr:
        raise RuntimeError(exact.stderr.strip())
    diagnostic = manifest_tool.diagnostic_from_exception(RuntimeError(exact.stderr))
    error_class = manifest_tool.classify_error(diagnostic)
    # Import-only audits need the exact/approx classification, not the approximate SOP itself.
    # Avoid materializing huge approximate QNN/QFT instances that will never be solved.
    if exact_only:
        return "", "approx", error_class, diagnostic
    approx = run(
        [qasm2sop, "--approx", repr(APPROX_EPSILON), "--input", zero, "--output", zero, "-"],
        stdin=qasm, timeout=timeout)
    if approx.returncode != 0:
        raise RuntimeError(approx.stderr.strip())
    return approx.stdout, "approx", error_class, diagnostic
