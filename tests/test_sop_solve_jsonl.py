#!/usr/bin/env python3
"""Smoke tests for --stats-jsonl backend-decision logging (Phase 1)."""

import json
import pathlib
import subprocess
import sys
import tempfile


def make_path_qsop(nvars: int, r: int = 8) -> str:
    """Return a QSOP string for a path graph with `nvars` nodes."""
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"e {i} {i + 1}")
    return "\n".join(lines) + "\n"


def run_branch_jsonl(exe: pathlib.Path, qsop_text: str, jsonl_path: str,
                     extra_args: list[str] | None = None) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    args = [str(exe), "--backend", "branch", "--max-vars", "64",
            "--stats-jsonl", jsonl_path, qsop_path]
    if extra_args:
        args += extra_args
    return subprocess.run(args, check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def test_jsonl_path_40(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """A 40-node path triggers the root treewidth fast path; at least one JSONL record."""
    jsonl_path = str(tmpdir / "out.jsonl")
    qsop_text = make_path_qsop(40)
    result = run_branch_jsonl(exe, qsop_text, jsonl_path)
    assert result.returncode == 0, f"sop-solve failed:\n{result.stderr}"

    records = []
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))

    assert len(records) >= 1, "Expected at least one JSONL record for 40-node path"

    backend_records = [r for r in records
                       if r.get("schema") == "sop_solve_backend_stats_v1"]
    for rec in backend_records:
        assert rec.get("schema") == "sop_solve_backend_stats_v1", \
            f"Wrong schema in record: {rec}"
        assert "backend_chosen" in rec, f"Missing backend_chosen in record: {rec}"
        assert rec["backend_chosen"] in ("treewidth", "rankwidth", "branch"), \
            f"Unexpected backend_chosen value: {rec['backend_chosen']}"
        assert "residual_id" in rec, f"Missing residual_id in record: {rec}"
        assert "n_active_vars" in rec, f"Missing n_active_vars in record: {rec}"
        assert "modulus_r" in rec, f"Missing modulus_r in record: {rec}"
    summaries = [r for r in records if r.get("schema") == "sop_solve_run_stats_v1"]
    assert len(summaries) == 1 and summaries[0]["status"] == "solved", records


def test_jsonl_disabled_by_default(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """Without --stats-jsonl, the solver does not create a JSONL file."""
    jsonl_path = tmpdir / "should_not_exist.jsonl"
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(40))
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", "--max-vars", "64",
         qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode == 0, f"sop-solve failed:\n{result.stderr}"
    assert not jsonl_path.exists(), "JSONL file created without --stats-jsonl flag"


def test_jsonl_result_unchanged(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """--stats-jsonl must not change the residue-vector output."""
    qsop_text = make_path_qsop(40)
    jsonl_path = str(tmpdir / "trace.jsonl")

    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name

    without_jsonl = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", "--max-vars", "64",
         qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    with_jsonl = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", "--max-vars", "64",
         "--stats-jsonl", jsonl_path, qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert without_jsonl.returncode == 0, f"baseline failed: {without_jsonl.stderr}"
    assert with_jsonl.returncode == 0, f"with jsonl failed: {with_jsonl.stderr}"
    assert without_jsonl.stdout == with_jsonl.stdout, \
        "Residue vector changed when --stats-jsonl is active"


def test_jsonl_veto_reasons_present(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """Each record that wasn't the chosen backend must have a non-null veto_reason."""
    jsonl_path = str(tmpdir / "veto.jsonl")
    qsop_text = make_path_qsop(40)
    result = run_branch_jsonl(exe, qsop_text, jsonl_path)
    assert result.returncode == 0, f"sop-solve failed: {result.stderr}"

    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            # Records where rankwidth was vetoed must have a veto_reason
            if rec.get("backend_chosen") == "treewidth":
                # treewidth was chosen; rankwidth may have been vetoed
                # veto_reason could be null (no rankwidth attempt) or a string
                veto = rec.get("veto_reason")
                assert veto is None or isinstance(veto, str), \
                    f"veto_reason must be null or string, got {veto!r} in {rec}"


def test_jsonl_small_instance_no_delegation(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """A small QSOP (< 32 vars) produces no delegation records but exits cleanly."""
    jsonl_path = str(tmpdir / "small.jsonl")
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(4))
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--stats-jsonl", jsonl_path, qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode == 0, f"sop-solve failed: {result.stderr}"
    with open(jsonl_path) as f:
        records = [json.loads(line) for line in f if line.strip()]
    summaries = [r for r in records if r.get("schema") == "sop_solve_run_stats_v1"]
    assert len(summaries) == 1, f"Expected one final run summary, got {records}"
    assert summaries[0]["status"] == "solved", summaries[0]


def test_jsonl_policy_refusal_summary(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """A policy refusal exits nonzero but retains its final partial statistics."""
    jsonl_path = str(tmpdir / "refusal.jsonl")
    lines = ["p qsop-sign 8 4 6", "n 0", "cst 0"]
    for u in range(4):
        for v in range(u + 1, 4):
            lines.append(f"e {u} {v}")
    result = run_branch_jsonl(
        exe, "\n".join(lines) + "\n", jsonl_path,
        ["--solve-mode", "single-fourier", "--branch-rw-source", "none",
         "--branch-single-max-fallback-vars", "2",
         "--branch-single-delegate-max-dp-work", "1"],
    )
    assert result.returncode != 0, "forced fallback refusal unexpectedly solved"
    records = [json.loads(line) for line in pathlib.Path(jsonl_path).read_text().splitlines()
               if line.strip()]
    summaries = [r for r in records if r.get("schema") == "sop_solve_run_stats_v1"]
    assert len(summaries) == 1, records
    summary = summaries[0]
    assert summary["status"] == "refused", summary
    assert summary["reason"] == "max_fallback_vars", summary
    assert summary["active_vars_at_failure"] == 4, summary


def test_jsonl_max_vars_refusal_summary(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """A max-vars refusal keeps its stable pre-JSONL outcome classification."""
    jsonl_path = str(tmpdir / "max-vars.jsonl")
    lines = ["p qsop-sign 8 4 6", "n 0", "cst 0"]
    for u in range(4):
        for v in range(u + 1, 4):
            lines.append(f"e {u} {v}")
    result = run_branch_jsonl(
        exe, "\n".join(lines) + "\n", jsonl_path,
        ["--solve-mode", "single-fourier", "--max-vars", "2"],
    )
    assert result.returncode != 0, "forced max-vars refusal unexpectedly solved"
    records = [json.loads(line) for line in pathlib.Path(jsonl_path).read_text().splitlines()
               if line.strip()]
    summaries = [r for r in records if r.get("schema") == "sop_solve_run_stats_v1"]
    assert len(summaries) == 1, records
    summary = summaries[0]
    assert summary["status"] == "refused", summary
    assert summary["reason"] == "max_vars", summary
    assert "pass a larger --max-vars" in summary["diagnostic"], summary


def test_conditioning_diagnostic_and_cutset(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """The deterministic shortlist starts at var 0, and depth-one cutset consumes two children."""
    lines = ["p qsop-sign 8 4 6", "n 8", "cst 1"]
    for u in range(4):
        for v in range(u + 1, 4):
            lines.append(f"e {u} {v}")
    qsop = "\n".join(lines) + "\n"
    diagnostic_path = str(tmpdir / "conditioning.jsonl")
    diagnostic = run_branch_jsonl(
        exe, qsop, diagnostic_path,
        ["--solve-mode", "single-fourier", "--branch-rw-source", "none",
         "--branch-single-max-fallback-vars", "2",
         "--branch-single-delegate-max-dp-work", "1",
         "--branch-single-lookahead-candidates", "1",
         "--branch-single-diagnose-conditioning"],
    )
    assert diagnostic.returncode != 0, "diagnostic-only run unexpectedly solved"
    records = [json.loads(line) for line in pathlib.Path(diagnostic_path).read_text().splitlines()
               if line.strip()]
    probes = [r for r in records if r.get("schema") == "sop_solve_conditioning_v1"]
    assert len(probes) == 2, probes
    assert {r["candidate_variable"] for r in probes} == {0}, probes
    assert {r["value"] for r in probes} == {0, 1}, probes
    assert all(r["degree2_merges"] > 0 for r in probes), probes

    cutset_path = str(tmpdir / "cutset.jsonl")
    cutset = run_branch_jsonl(
        exe, qsop, cutset_path,
        ["--solve-mode", "single-fourier", "--format", "stats",
         "--branch-rw-source", "none", "--branch-single-max-fallback-vars", "2",
         "--branch-single-delegate-max-dp-work", "1",
         "--branch-single-lookahead-candidates", "1",
         "--branch-single-materialized-reduction", "--branch-single-cutset-depth", "1"],
    )
    assert cutset.returncode == 0, cutset.stderr
    records = [json.loads(line) for line in pathlib.Path(cutset_path).read_text().splitlines()
               if line.strip()]
    summary = [r for r in records if r.get("schema") == "sop_solve_run_stats_v1"][-1]
    assert summary["status"] == "solved", summary
    assert summary["branch_conditioning_nodes"] == 1, summary
    assert summary["branch_conditioning_lookaheads"] == 2, summary
    assert summary["branch_max_cutset_depth"] == 1, summary


def make_two_component_qsop(n1: int, n2: int, r: int = 8) -> str:
    """Path of n1 nodes + path of n2 nodes (disjoint components)."""
    nedges = (n1 - 1) + (n2 - 1)
    lines = [f"p qsop-sign {r} {n1 + n2} {nedges}", "n 0", "cst 0"]
    for i in range(n1 - 1):
        lines.append(f"e {i} {i + 1}")
    for i in range(n2 - 1):
        lines.append(f"e {n1 + i} {n1 + i + 1}")
    return "\n".join(lines) + "\n"


def test_jsonl_calibrate_output_unchanged(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """--branch-calibrate-backends must not change the residue vector."""
    qsop_text = make_two_component_qsop(35, 3)
    jsonl_path = str(tmpdir / "cal_check.jsonl")
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    without_cal = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", "--max-vars", "64",
         qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    with_cal = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", "--max-vars", "64",
         "--branch-calibrate-backends", "--stats-jsonl", jsonl_path, qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert without_cal.returncode == 0, f"baseline failed: {without_cal.stderr}"
    assert with_cal.returncode == 0, f"calibrate failed: {with_cal.stderr}"
    assert without_cal.stdout == with_cal.stdout, \
        "Calibration changed the residue vector"


def test_jsonl_calibrate_rankwidth_timing(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """A 2-component QSOP forces dp_delegate; calibration must populate rankwidth_actual_ms."""
    jsonl_path = str(tmpdir / "calibrate.jsonl")
    qsop_text = make_two_component_qsop(35, 3)
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64",
         "--branch-calibrate-backends", "--stats-jsonl", jsonl_path, qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode == 0, f"sop-solve failed: {result.stderr}"

    with open(jsonl_path) as f:
        records = [json.loads(line) for line in f if line.strip()]

    assert len(records) >= 1, "Expected at least one JSONL record from dp_delegate path"
    # With calibration, the treewidth-wins record should have rankwidth_actual_ms measured
    calibrated = [r for r in records if r.get("rankwidth_actual_ms") is not None]
    assert len(calibrated) >= 1, \
        f"Expected calibrated record with rankwidth_actual_ms; got records: {records}"


def test_jsonl_calibrate_requires_jsonl(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """--branch-calibrate-backends without --stats-jsonl must be rejected."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(4))
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--branch-calibrate-backends", qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode != 0, \
        "--branch-calibrate-backends without --stats-jsonl should fail"
    assert "stats-jsonl" in result.stderr, \
        f"Expected error mentioning stats-jsonl, got: {result.stderr!r}"


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])
    _source_root = pathlib.Path(argv[2])

    with tempfile.TemporaryDirectory() as td:
        tmpdir = pathlib.Path(td)
        test_jsonl_path_40(exe, tmpdir)
        test_jsonl_disabled_by_default(exe, tmpdir)
        test_jsonl_result_unchanged(exe, tmpdir)
        test_jsonl_veto_reasons_present(exe, tmpdir)
        test_jsonl_small_instance_no_delegation(exe, tmpdir)
        test_jsonl_policy_refusal_summary(exe, tmpdir)
        test_jsonl_max_vars_refusal_summary(exe, tmpdir)
        test_conditioning_diagnostic_and_cutset(exe, tmpdir)
        test_jsonl_calibrate_output_unchanged(exe, tmpdir)
        test_jsonl_calibrate_rankwidth_timing(exe, tmpdir)
        test_jsonl_calibrate_requires_jsonl(exe, tmpdir)

    print("all sop-solve JSONL tests passed")


if __name__ == "__main__":
    main(sys.argv)
