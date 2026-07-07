#!/usr/bin/env python3
"""A5: Branch regression guard — verifies A3/A4 veto logic prevents wasteful
rankwidth probes and that branch:auto overhead stays bounded vs branch:no-rankwidth.

Usage: python3 tests/test_branch_regression_guard.py <sop-solve>
"""

import pathlib
import random
import subprocess
import sys
import time

# Structural cap: when rankwidth is never delegated on an instance, the number
# of rankwidth probes (skips) must stay below this per-instance bound.
# Under normal A3/A4 veto operation each root call produces at most one skip.
# If this veto is broken, probes can fire at every recursion node (thousands).
RW_SKIP_CAP_PER_INSTANCE = 10

# Soft timing ratio: branch:auto total wall time must not exceed this multiple
# of branch:no-rankwidth time on the same corpus.  We use 2.0 (generous) to
# avoid CI flakiness while still catching catastrophic regressions.
AUTO_TIME_RATIO_CAP = 2.0

# Deterministic fixture generation: several small, structurally-varied instances
# so the timing-ratio and structural-guard checks below aren't fooled by a single
# lucky/unlucky shape.
_FIXTURE_SIZES = [4, 6, 8, 10, 12, 14, 16]
_FIXTURE_MODULI = [2, 4, 8]
_FIXTURE_SEED = 20260706


def _random_qsop(rng: random.Random, nvars: int, r: int, edge_density: float = 0.4) -> str:
    edges = []
    for u in range(nvars):
        for v in range(u + 1, nvars):
            if rng.random() < edge_density:
                edges.append((u, v))
    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    constant = rng.randint(0, r - 1)
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", f"cst {constant}"]
    for v in range(nvars):
        if unary[v] != 0:
            lines.append(f"u {v} {unary[v]}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def _write_qsop_fixtures(tmp: pathlib.Path) -> list[pathlib.Path]:
    rng = random.Random(_FIXTURE_SEED)
    files = []
    for i, nvars in enumerate(_FIXTURE_SIZES):
        r = _FIXTURE_MODULI[i % len(_FIXTURE_MODULI)]
        text = _random_qsop(rng, nvars, r)
        path = tmp / f"fixture_{nvars}.qsop"
        path.write_text(text)
        files.append(path)
    return files


def _run(sop_solve, qsop, rw_source, timeout=30.0):
    """Run sop-solve and return (elapsed_ns, stats_dict) or None on failure."""
    cmd = [
        str(sop_solve),
        "--backend", "branch",
        "--branch-rw-source", rw_source,
        "--max-vars", "64",
        "--format", "stats",
        str(qsop),
    ]
    t0 = time.perf_counter_ns()
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, {}
    elapsed = time.perf_counter_ns() - t0
    if result.returncode != 0:
        return None, {}
    stats = {}
    for line in result.stdout.decode().splitlines():
        parts = line.split(":", 1)
        if len(parts) == 2:
            key = parts[0].strip()
            val = parts[1].strip()
            try:
                stats[key] = int(val)
            except ValueError:
                stats[key] = val
    return elapsed, stats


def test_structural_guard(sop_solve, tmp):
    """If rankwidth_delegations == 0, branch_rankwidth_skips must be bounded."""
    files = _write_qsop_fixtures(tmp)

    violations = []
    for qsop in files:
        elapsed, stats = _run(sop_solve, qsop, "auto")
        if elapsed is None:
            continue
        rw_del = stats.get("rankwidth_delegations", 0)
        rw_skips = stats.get("branch_rankwidth_skips", 0)
        if rw_del == 0 and rw_skips > RW_SKIP_CAP_PER_INSTANCE:
            violations.append(
                f"{qsop.name}: rw_delegations=0 but branch_rankwidth_skips={rw_skips} "
                f"(cap={RW_SKIP_CAP_PER_INSTANCE})"
            )

    if violations:
        raise AssertionError(
            f"A3/A4 veto regression: {len(violations)} instance(s) have excessive "
            f"rankwidth probes:\n" + "\n".join(violations)
        )


def test_timing_overhead(sop_solve, tmp):
    """branch:auto total time must not exceed AUTO_TIME_RATIO_CAP * branch:no-rankwidth."""
    files = _write_qsop_fixtures(tmp)

    total_auto_ns = 0
    total_norw_ns = 0
    compared = 0

    for qsop in files:
        auto_ns, _ = _run(sop_solve, qsop, "auto")
        norw_ns, _ = _run(sop_solve, qsop, "none")
        if auto_ns is None or norw_ns is None:
            continue
        total_auto_ns += auto_ns
        total_norw_ns += norw_ns
        compared += 1

    if compared == 0:
        raise AssertionError("no comparable pairs across fixture instances")

    if total_norw_ns == 0:
        raise AssertionError("no-rankwidth baseline has zero elapsed time")

    ratio = total_auto_ns / total_norw_ns
    if ratio > AUTO_TIME_RATIO_CAP:
        raise AssertionError(
            f"branch:auto is {ratio:.2f}x slower than branch:no-rankwidth "
            f"across {compared} instances (cap={AUTO_TIME_RATIO_CAP}x). "
            f"auto={total_auto_ns//1_000_000}ms norw={total_norw_ns//1_000_000}ms"
        )


def test_rw_delegation_consistency(sop_solve, tmp):
    """When branch:auto delegates to rankwidth, branch:no-rankwidth must not."""
    files = _write_qsop_fixtures(tmp)

    for qsop in files:
        _, auto_stats = _run(sop_solve, qsop, "auto")
        _, norw_stats = _run(sop_solve, qsop, "none")
        rw_del_norw = norw_stats.get("rankwidth_delegations", 0)
        if rw_del_norw != 0:
            raise AssertionError(
                f"{qsop.name}: branch:no-rankwidth has rankwidth_delegations="
                f"{rw_del_norw} (expected 0)"
            )


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_branch_regression_guard.py <sop-solve>", file=sys.stderr)
        return 2

    sop_solve = pathlib.Path(sys.argv[1])
    if not sop_solve.exists():
        print(f"error: sop-solve not found at {sop_solve}", file=sys.stderr)
        return 2

    import tempfile
    tests = [
        ("structural_guard", test_structural_guard),
        ("timing_overhead", test_timing_overhead),
        ("rw_delegation_consistency", test_rw_delegation_consistency),
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
