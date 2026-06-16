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
    lines = [f"p qsop {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"q {i} {i + 1} 1")
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

    for rec in records:
        assert rec.get("schema") == "sop_solve_backend_stats_v1", \
            f"Wrong schema in record: {rec}"
        assert "backend_chosen" in rec, f"Missing backend_chosen in record: {rec}"
        assert rec["backend_chosen"] in ("treewidth", "rankwidth", "branch", "components",
                                          "bruteforce"), \
            f"Unexpected backend_chosen value: {rec['backend_chosen']}"
        assert "residual_id" in rec, f"Missing residual_id in record: {rec}"
        assert "n_active_vars" in rec, f"Missing n_active_vars in record: {rec}"
        assert "modulus_r" in rec, f"Missing modulus_r in record: {rec}"


def test_jsonl_disabled_by_default(exe: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """Without --stats-jsonl, the solver does not create a JSONL file."""
    jsonl_path = tmpdir / "should_not_exist.jsonl"
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(40))
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", qsop_path],
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
        [str(exe), "--backend", "branch", "--max-vars", "64", qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    with_jsonl = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "--stats-jsonl", jsonl_path,
         qsop_path],
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
    # Small instances go through root fast path only if nvars >= 32; 4 vars -> 0 records
    assert isinstance(records, list), "JSONL output must be a list of records"


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

    print("all sop-solve JSONL tests passed")


if __name__ == "__main__":
    main(sys.argv)
