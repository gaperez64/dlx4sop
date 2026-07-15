#!/usr/bin/env python3
"""Regression guard for --branch-shadow: off/auto/on must agree on the amplitude (within the
numeric error bound each side reports) on a corpus of gadget-heavy fixtures shaped like the QNN
motif the shadow-graph heuristic targets (a small, densely-connected "logical" variable core with
many degree-1/2 check/value gadget chains hanging off it), and shadow mode must not regress search
effort by more than a generous multiple of the legacy heuristic's.

Usage: python3 tests/test_branch_shadow_regression_guard.py <sop-solve>
"""

from __future__ import annotations

import math
import pathlib
import random
import subprocess
import sys

# Soft cap: --branch-shadow on's total search_nodes across the corpus must not exceed this
# multiple of off's. Generous (the shadow heuristic is expected to *reduce* search effort on
# this corpus -- see the docstring -- but this guard exists to catch a catastrophic regression,
# not to enforce a specific speedup).
SHADOW_TIME_RATIO_CAP = 3.0

_FIXTURE_SEED = 20260715
# (n_logical, gadgets_per_edge) pairs: n_logical vars form a complete graph (a stable degree
# >= n_logical-1 core), each logical edge gets `gadgets_per_edge` independent
# x -- check -- value chains (check also edges to the other logical endpoint, matching the
# design doc's x_i, x_j -> check -> value motif).
_FIXTURE_SHAPES = [(4, 1), (5, 1)]

# Wall-clock-derived counters vary run to run even with identical search behavior; exclude them
# from the "default matches explicit off" byte-equality check below.
_TIMING_KEYS = {"branch_materialized_reduction_ns", "branch_shadow_build_ns"}

_FORCE_CUTSET_ARGS = [
    "--format", "stats",
    "--solve-mode", "single-fourier",
    "--branch-rw-source", "none",
    "--branch-single-delegate-max-dp-work", "1",
    "--branch-single-max-fallback-vars", "5",
    "--branch-single-cutset-depth", "60",
    "--branch-single-max-conditioning-nodes", "500000",
    "--branch-single-materialized-reduction",
    "--branch-single-fourier-fallback", "auto",
]


def _gadget_qsop(rng: random.Random, n_logical: int, gadgets_per_edge: int, r: int) -> str:
    edges: list[tuple[int, int]] = []
    for i in range(n_logical):
        for j in range(i + 1, n_logical):
            edges.append((i, j))
    logical_edges = list(edges)
    next_id = n_logical
    for x, y in logical_edges:
        for _ in range(gadgets_per_edge):
            check, value = next_id, next_id + 1
            next_id += 2
            edges.append((x, check))
            edges.append((y, check))
            edges.append((check, value))
    nvars = next_id
    # Unary values deliberately avoid {0, r/2}: those trigger the *exact* [HH] Hadamard-collapse
    # rule (qsop_residual_propagate / materialized reduction), which would eliminate every
    # degree-<=2 gadget vertex outright and leave nothing for the branching heuristic -- shadow
    # or legacy -- to ever choose between.
    off_limits = {0, r // 2}
    choices = [v for v in range(r) if v not in off_limits]
    unary = [rng.choice(choices) for _ in range(nvars)]
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", "cst 0"]
    for v in range(nvars):
        lines.append(f"u {v} {unary[v]}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def _write_fixtures(tmp: pathlib.Path) -> list[pathlib.Path]:
    rng = random.Random(_FIXTURE_SEED)
    files = []
    for i, (n_logical, gadgets_per_edge) in enumerate(_FIXTURE_SHAPES):
        text = _gadget_qsop(rng, n_logical, gadgets_per_edge, r=4)
        path = tmp / f"gadget_{i}_{n_logical}x{gadgets_per_edge}.qsop"
        path.write_text(text)
        files.append(path)
    return files


def _run(sop_solve: pathlib.Path, qsop: pathlib.Path, shadow_mode: str | None,
         timeout: float = 30.0) -> dict[str, str]:
    args = list(_FORCE_CUTSET_ARGS)
    if shadow_mode is not None:
        args += ["--branch-shadow", shadow_mode]
    completed = subprocess.run(
        [str(sop_solve), *args, str(qsop)],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"sop-solve {' '.join(args)} {qsop.name} failed with {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    values: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        key, sep, value = line.partition(":")
        if sep:
            values[key.strip()] = value.strip()
    return values


def _amplitude(stats: dict[str, str]) -> tuple[float, float, float]:
    return (
        float(stats["amplitude_re"]),
        float(stats["amplitude_im"]),
        float(stats["numeric_error_bound"]),
    )


def _assert_within_bounds(name: str, lhs: dict[str, str], rhs: dict[str, str]) -> None:
    lhs_re, lhs_im, lhs_bound = _amplitude(lhs)
    rhs_re, rhs_im, rhs_bound = _amplitude(rhs)
    error = math.hypot(lhs_re - rhs_re, lhs_im - rhs_im)
    bound = lhs_bound + rhs_bound
    if error > bound:
        raise AssertionError(f"{name}: amplitude distance {error:.3g} exceeds bound {bound:.3g}")


def test_default_is_off(sop_solve: pathlib.Path, tmp: pathlib.Path) -> None:
    """Omitting --branch-shadow entirely must match --branch-shadow off exactly."""
    for qsop in _write_fixtures(tmp):
        implicit = _run(sop_solve, qsop, None)
        explicit_off = _run(sop_solve, qsop, "off")
        implicit_stable = {k: v for k, v in implicit.items() if k not in _TIMING_KEYS}
        off_stable = {k: v for k, v in explicit_off.items() if k not in _TIMING_KEYS}
        if implicit_stable != off_stable:
            raise AssertionError(f"{qsop.name}: default output differs from --branch-shadow off")
        if int(implicit.get("branch_shadow_builds", "0")) != 0:
            raise AssertionError(f"{qsop.name}: default run built a shadow graph")


def test_correctness_and_effort(sop_solve: pathlib.Path, tmp: pathlib.Path) -> None:
    """off/auto/on must agree on the amplitude; on's total search effort must not regress by
    more than SHADOW_TIME_RATIO_CAP; and on must actually engage on at least one fixture."""
    total_off_nodes = 0
    total_on_nodes = 0
    any_shadow_build = False
    any_shadow_selected = False

    for qsop in _write_fixtures(tmp):
        off = _run(sop_solve, qsop, "off")
        auto = _run(sop_solve, qsop, "auto")
        on = _run(sop_solve, qsop, "on")

        _assert_within_bounds(f"{qsop.name}: off vs auto", off, auto)
        _assert_within_bounds(f"{qsop.name}: off vs on", off, on)

        total_off_nodes += int(off.get("search_nodes", "0"))
        total_on_nodes += int(on.get("search_nodes", "0"))
        any_shadow_build = any_shadow_build or int(on.get("branch_shadow_builds", "0")) > 0
        any_shadow_selected = any_shadow_selected or int(on.get("branch_shadow_selected", "0")) > 0

    if not any_shadow_build:
        raise AssertionError("--branch-shadow on never built a shadow graph on any fixture")
    if not any_shadow_selected:
        raise AssertionError(
            "--branch-shadow on never won the final candidate comparison on any fixture"
        )
    if total_off_nodes == 0:
        raise AssertionError("off baseline reported zero total search_nodes")

    ratio = total_on_nodes / total_off_nodes
    if ratio > SHADOW_TIME_RATIO_CAP:
        raise AssertionError(
            f"--branch-shadow on used {ratio:.2f}x the search_nodes of off across the corpus "
            f"(cap={SHADOW_TIME_RATIO_CAP}x): on={total_on_nodes} off={total_off_nodes}"
        )


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_branch_shadow_regression_guard.py <sop-solve>", file=sys.stderr)
        return 2

    sop_solve = pathlib.Path(sys.argv[1])
    if not sop_solve.exists():
        print(f"error: sop-solve not found at {sop_solve}", file=sys.stderr)
        return 2

    import tempfile

    tests = [
        ("default_is_off", test_default_is_off),
        ("correctness_and_effort", test_correctness_and_effort),
    ]
    failed = []
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        for name, fn in tests:
            try:
                fn(sop_solve, tmp)
                print(f"  PASS {name}")
            except Exception as exc:
                print(f"  FAIL {name}: {exc}")
                failed.append(name)

    if failed:
        print(f"\n{len(failed)} test(s) failed: {failed}", file=sys.stderr)
        return 1
    print(f"\n{len(tests)} test(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
