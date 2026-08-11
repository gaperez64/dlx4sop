#!/usr/bin/env python3
"""Tests for branch policy CLI options (A3/A4/A5).

Usage: python3 tests/test_branch_policy.py <sop-solve>
"""

import pathlib
import subprocess
import sys
import tempfile


def _small_qsop_fixture(tmp: pathlib.Path) -> pathlib.Path:
    """8-variable path graph plus 2 chords, with a few unary terms."""
    nvars = 8
    r = 8
    edges = [(i, i + 1) for i in range(nvars - 1)] + [(0, 3), (2, 6)]
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", "cst 1"]
    for i in range(nvars):
        if i % 2 == 0:
            lines.append(f"u {i} {(i % (r - 1)) + 1}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    path = tmp / "fixture.qsop"
    path.write_text("\n".join(lines) + "\n")
    return path


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


def test_removed_threshold_flags_are_rejected(sop_solve, tmp):
    # The unified delegation cost model subsumed these four hand-tuned pre-probe vetoes:
    # a treewidth estimate too small to be worth probing against, or a cut rank small enough
    # that rankwidth obviously wins, both fall out of rw_est * rw_min_speedup < tw_est.
    qsop = _small_qsop_fixture(tmp)
    for flag in (
        "--branch-rw-min-treewidth-width",
        "--branch-rw-min-treewidth-forecast",
        "--branch-rw-min-residual-vars",
        "--branch-rw-low-rank-bypass",
    ):
        result = _run_sop_solve(sop_solve, qsop, [flag, "1"])
        if result["status"] == "ok":
            raise AssertionError(f"{flag} should no longer be accepted: {result}")


def test_branch_rw_min_speedup_high_veto(sop_solve, tmp):
    qsop = _small_qsop_fixture(tmp)
    result = _run_sop_solve(sop_solve, qsop, ["--branch-rw-min-speedup", "1000.0"])
    if result["status"] != "ok":
        raise AssertionError(f"high-speedup run failed: {result}")


def test_branch_no_rankwidth_completes(sop_solve, tmp):
    qsop = _small_qsop_fixture(tmp)
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
    qsop = _small_qsop_fixture(tmp)
    policy_args = [
        "--branch-rw-min-speedup", "1.1",
        "--branch-rw-fixed-overhead-ns", "20000",
        "--branch-tw-fixed-overhead-ns", "10000",
        "--branch-rw-memory-penalty-ns", "0",
    ]
    result = _run_sop_solve(sop_solve, qsop, policy_args)
    if result["status"] != "ok":
        raise AssertionError(f"policy args parse failed: {result}")


def test_non_branch_backend_rejects_rw_source(sop_solve, tmp):
    qsop = _small_qsop_fixture(tmp)
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


def test_auto_large_residue_vector_uses_single_fourier(sop_solve, tmp):
    """A scalar with huge R must not allocate the count-table result vector."""
    qsop = tmp / "large-residue.qsop"
    qsop.write_text("p qsop-sign 536870912 0 0\nn 28\ncst 0\n")
    result = subprocess.run(
        [str(sop_solve), "--backend", "branch", "--solve-mode", "auto",
         "--format", "stats", str(qsop)],
        capture_output=True, text=True, timeout=10.0,
    )
    if result.returncode != 0:
        raise AssertionError(f"large-residue auto solve failed: {result.stderr}")
    if "solve_mode_kernel: single-fourier" not in result.stdout:
        raise AssertionError(f"auto did not preflight the count vector:\n{result.stdout}")


def test_branch_rankwidth_delegate_uses_twist_model(sop_solve, tmp):
    """A forced rankwidth delegate must carry the AUTO forecast through execution and stats."""
    edges = (
        (0, 5), (0, 8), (0, 11), (1, 2), (1, 10), (1, 13), (1, 22), (1, 23),
        (1, 25), (1, 26), (2, 5), (2, 10), (2, 22), (2, 27), (3, 9), (3, 16),
        (3, 20), (4, 13), (4, 19), (4, 20), (4, 29), (5, 8), (5, 19), (5, 20),
        (5, 24), (5, 27), (6, 14), (6, 21), (7, 10), (7, 19), (7, 24), (7, 28),
        (8, 11), (8, 15), (9, 23), (9, 26), (10, 12), (10, 23), (12, 21),
        (12, 28), (13, 16), (13, 17), (13, 19), (13, 22), (14, 24), (14, 26),
        (15, 29), (16, 20), (17, 22), (18, 20), (19, 22), (20, 21), (20, 26),
        (23, 25), (24, 27), (24, 29), (26, 27),
    )
    qsop = tmp / "branch-twist.qsop"
    lines = [f"p qsop-sign 8 30 {len(edges)}", "n 0", "cst 0"]
    lines.extend(f"u {vertex} {(3 * vertex + 1) % 8}" for vertex in range(30))
    lines.extend(f"e {left} {right}" for left, right in edges)
    qsop.write_text("\n".join(lines) + "\n")

    branch = subprocess.run(
        [
            str(sop_solve), "--backend", "branch", "--solve-mode", "single-fourier",
            "--branch-rw-source", "from-treewidth", "--branch-no-qpf",
            "--branch-single-propagate", "off", "--branch-single-delegate-max-width", "1",
            "--branch-single-fourier-fallback", "delegate-only", "--max-vars", "64",
            "--format", "stats", str(qsop),
        ],
        capture_output=True, text=True, timeout=20.0,
    )
    if branch.returncode != 0:
        raise AssertionError(f"forced rankwidth delegate failed: {branch.stderr}")
    stats = {
        key.strip(): value.strip()
        for line in branch.stdout.splitlines()
        for key, separator, value in [line.partition(":")]
        if separator
    }
    for field in ("rankwidth_delegations", "rankwidth_twist_join_events",
                  "rankwidth_predicted_twist_join_events", "rankwidth_predicted_solve_ns"):
        if int(stats.get(field, "0")) <= 0:
            raise AssertionError(f"expected positive {field}:\n{branch.stdout}")

    reference = subprocess.run(
        [
            str(sop_solve), "--backend", "treewidth", "--solve-mode", "single-fourier",
            "--max-vars", "64", "--format", "stats", str(qsop),
        ],
        capture_output=True, text=True, timeout=20.0,
    )
    if reference.returncode != 0:
        raise AssertionError(f"treewidth reference failed: {reference.stderr}")
    reference_stats = {
        key.strip(): value.strip()
        for line in reference.stdout.splitlines()
        for key, separator, value in [line.partition(":")]
        if separator
    }
    for component in ("amplitude_re", "amplitude_im"):
        observed = float(stats[component])
        expected = float(reference_stats[component])
        if abs(observed - expected) > 1e-8 * (1.0 + abs(expected)):
            raise AssertionError(
                f"branch twist {component} mismatch: {observed} != {expected}"
            )


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_branch_policy.py <sop-solve>", file=sys.stderr)
        return 2

    sop_solve = pathlib.Path(sys.argv[1])
    tests = [
        ("binary_exists", test_binary_exists),
        ("removed_threshold_flags_are_rejected", test_removed_threshold_flags_are_rejected),
        ("branch_rw_min_speedup_high_veto", test_branch_rw_min_speedup_high_veto),
        ("branch_no_rankwidth_completes", test_branch_no_rankwidth_completes),
        ("new_policy_options_parse", test_new_policy_options_parse),
        ("non_branch_backend_rejects_rw_source", test_non_branch_backend_rejects_rw_source),
        ("auto_large_residue_vector_uses_single_fourier",
         test_auto_large_residue_vector_uses_single_fourier),
        ("branch_rankwidth_delegate_uses_twist_model",
         test_branch_rankwidth_delegate_uses_twist_model),
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
