#!/usr/bin/env python3
"""Tests for branch policy CLI options (A3/A4/A5).

Usage: python3 tests/test_branch_policy.py <sop-solve>
"""

import pathlib
import subprocess
import sys
import tempfile


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BENCHMARKS_DIR = REPO_ROOT / "benchmarks" / "corpus" / "sop"


def _find_small_qsop():
    """Return a small QSOP file from the corpus for policy testing."""
    for tier_dir in sorted(BENCHMARKS_DIR.iterdir()):
        if not tier_dir.is_dir():
            continue
        for qsop in sorted(tier_dir.glob("*.qsop"))[:3]:
            return qsop
    return None


def _run_sop_solve(sop_solve, qsop, extra_args, timeout=10.0):
    cmd = [
        str(sop_solve),
        "--backend", "branch",
        "--branch-heuristic", "split",
        "--branch-rw-source", "auto",
        "--max-vars", "64",
        "--format", "stats",
    ] + extra_args + [str(qsop)]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=timeout)
        if result.returncode != 0:
            return {"status": "error", "stderr": result.stderr.decode()[:200]}
        return {"status": "ok", "stdout": result.stdout.decode()}
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}


def test_binary_exists(sop_solve, tmp):
    if not sop_solve.exists():
        raise AssertionError(f"sop-solve not found at {sop_solve}")


def test_branch_rw_min_treewidth_width_veto(sop_solve, tmp):
    qsop = _find_small_qsop()
    if qsop is None:
        print("  SKIP: no corpus found")
        return
    r1 = _run_sop_solve(sop_solve, qsop, [])
    r2 = _run_sop_solve(sop_solve, qsop, ["--branch-rw-min-treewidth-width", "100"])
    if r1["status"] != "ok":
        raise AssertionError(f"default run failed: {r1}")
    if r2["status"] != "ok":
        raise AssertionError(f"high-veto run failed: {r2}")


def test_branch_rw_min_speedup_high_veto(sop_solve, tmp):
    qsop = _find_small_qsop()
    if qsop is None:
        return
    result = _run_sop_solve(sop_solve, qsop, ["--branch-rw-min-speedup", "1000.0"])
    if result["status"] != "ok":
        raise AssertionError(f"high-speedup run failed: {result}")


def test_branch_no_rankwidth_completes(sop_solve, tmp):
    qsop = _find_small_qsop()
    if qsop is None:
        return
    cmd = [
        str(sop_solve),
        "--backend", "branch",
        "--branch-heuristic", "split",
        "--branch-rw-source", "none",
        "--max-vars", "64",
        "--format", "stats",
        str(qsop),
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=30.0)
    if result.returncode != 0:
        raise AssertionError(f"branch:no-rankwidth failed: {result.stderr.decode()[:200]}")


def test_new_policy_options_parse(sop_solve, tmp):
    qsop = _find_small_qsop()
    if qsop is None:
        return
    policy_args = [
        "--branch-rw-min-treewidth-width", "2",
        "--branch-rw-min-treewidth-forecast", "512",
        "--branch-rw-min-residual-vars", "16",
        "--branch-rw-low-rank-bypass", "4",
        "--branch-rw-min-speedup", "1.1",
        "--branch-rw-fixed-overhead-ns", "20000",
        "--branch-tw-fixed-overhead-ns", "10000",
        "--branch-rw-memory-penalty-ns", "0",
    ]
    result = _run_sop_solve(sop_solve, qsop, policy_args)
    if result["status"] != "ok":
        raise AssertionError(f"policy args parse failed: {result}")


def test_non_branch_backend_rejects_rw_source(sop_solve, tmp):
    qsop = _find_small_qsop()
    if qsop is None:
        return
    cmd = [
        str(sop_solve),
        "--backend", "treewidth",
        "--max-vars", "64",
        "--branch-rw-source", "auto",
        str(qsop),
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=10.0)
    if result.returncode == 0:
        raise AssertionError("expected error when --branch-rw-source used with non-branch backend")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_branch_policy.py <sop-solve>", file=sys.stderr)
        return 2

    sop_solve = pathlib.Path(sys.argv[1])
    tests = [
        ("binary_exists", test_binary_exists),
        ("branch_rw_min_treewidth_width_veto", test_branch_rw_min_treewidth_width_veto),
        ("branch_rw_min_speedup_high_veto", test_branch_rw_min_speedup_high_veto),
        ("branch_no_rankwidth_completes", test_branch_no_rankwidth_completes),
        ("new_policy_options_parse", test_new_policy_options_parse),
        ("non_branch_backend_rejects_rw_source", test_non_branch_backend_rejects_rw_source),
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
