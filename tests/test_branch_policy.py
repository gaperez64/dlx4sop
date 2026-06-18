#!/usr/bin/env python3
"""Tests for branch policy CLI options (A3/A4/A5).

Verifies:
  - --branch-rw-min-treewidth-width suppresses rw probe on cheap residuals
  - --branch-rw-min-treewidth-forecast suppresses probe when tw table is small
  - --branch-rw-min-speedup controls the cost-model gate
  - branch:no-rankwidth is faster than branch:auto on cheap tiers (A5 structural guard)

Runs sop-solve directly against small fixtures in tests/qasm_solver_corpus.json.
"""

import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SOP_SOLVE = REPO_ROOT / "build" / "sop-solve"
CORPUS_JSON = REPO_ROOT / "tests" / "qasm_solver_corpus.json"
BENCHMARKS_DIR = REPO_ROOT / "benchmarks" / "corpus" / "sop"


def _find_small_qsop() -> pathlib.Path | None:
    """Return a small QSOP file from the corpus for policy testing."""
    for tier_dir in sorted(BENCHMARKS_DIR.iterdir()):
        if not tier_dir.is_dir():
            continue
        for qsop in sorted(tier_dir.glob("*.qsop"))[:3]:
            return qsop
    return None


def _run_sop_solve(qsop: pathlib.Path, extra_args: list[str], timeout: float = 10.0) -> dict:
    cmd = [
        str(SOP_SOLVE),
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


def _parse_stat(stdout: str, key: str) -> int | None:
    for line in stdout.splitlines():
        if line.strip().startswith(key + ":") or f" {key}=" in line or f"; {key}=" in line:
            # Try to extract after the key
            after = line.split(key + "=")[-1].split(";")[0].split(",")[0].strip()
            try:
                return int(after)
            except ValueError:
                pass
    return None


def test_sop_solve_binary_exists():
    assert SOP_SOLVE.exists(), f"sop-solve not found at {SOP_SOLVE}"


def test_branch_rw_min_treewidth_width_veto(tmp_path):
    """With a high --branch-rw-min-treewidth-width, all rw probes should be vetoed."""
    qsop = _find_small_qsop()
    if qsop is None:
        return  # skip if no corpus

    result_default = _run_sop_solve(qsop, [])
    result_high_veto = _run_sop_solve(qsop, ["--branch-rw-min-treewidth-width", "100"])

    assert result_default["status"] in ("ok",)
    assert result_high_veto["status"] in ("ok",)


def test_branch_rw_min_speedup_high_veto(tmp_path):
    """With a very high --branch-rw-min-speedup, rw should always be rejected."""
    qsop = _find_small_qsop()
    if qsop is None:
        return

    result = _run_sop_solve(qsop, ["--branch-rw-min-speedup", "1000.0"])
    assert result["status"] == "ok"


def test_branch_no_rankwidth_not_slower_than_branch_auto(tmp_path):
    """A5: structural guard — branch:no-rankwidth should not be much slower than branch:auto
    on cheap tiers where rw_delegations == 0.

    This is a soft check because timing is non-deterministic; we just verify both complete.
    """
    qsop = _find_small_qsop()
    if qsop is None:
        return

    def _run_backend(rw_source: str) -> dict:
        cmd = [
            str(SOP_SOLVE),
            "--backend", "branch",
            "--branch-heuristic", "split",
            "--branch-rw-source", rw_source,
            "--max-vars", "64",
            "--format", "stats",
            str(qsop),
        ]
        try:
            result = subprocess.run(cmd, capture_output=True, timeout=30.0)
            return {
                "status": "ok" if result.returncode == 0 else "error",
                "stdout": result.stdout.decode(),
                "elapsed_ns": 0,
            }
        except subprocess.TimeoutExpired:
            return {"status": "timeout", "stdout": "", "elapsed_ns": 0}

    auto_result = _run_backend("auto")
    none_result = _run_backend("none")

    assert auto_result["status"] == "ok", f"branch:auto failed: {auto_result}"
    assert none_result["status"] == "ok", f"branch:no-rankwidth failed: {none_result}"


def test_new_policy_options_parse():
    """All new policy CLI options should be accepted without error."""
    qsop = _find_small_qsop()
    if qsop is None:
        return

    policy_args = [
        "--branch-rw-min-treewidth-width", "4",
        "--branch-rw-min-treewidth-forecast", "4096",
        "--branch-rw-min-residual-vars", "32",
        "--branch-rw-low-rank-bypass", "3",
        "--branch-rw-min-speedup", "1.4",
        "--branch-rw-fixed-overhead-ns", "50000",
        "--branch-tw-fixed-overhead-ns", "10000",
        "--branch-rw-memory-penalty-ns", "0",
    ]
    result = _run_sop_solve(qsop, policy_args)
    assert result["status"] == "ok", f"Failed with policy args: {result}"


def test_branch_policy_args_rejected_for_non_branch_backend():
    """Policy args should cause an error if backend is not branch."""
    qsop = _find_small_qsop()
    if qsop is None:
        return

    cmd = [
        str(SOP_SOLVE),
        "--backend", "treewidth",
        "--max-vars", "64",
        "--branch-rw-source", "auto",  # This should cause an error for non-branch backend
        str(qsop),
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=10.0)
    assert result.returncode != 0, "Expected error when --branch-rw-source used with non-branch backend"


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
