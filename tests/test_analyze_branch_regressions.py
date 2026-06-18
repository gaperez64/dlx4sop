#!/usr/bin/env python3
"""Tests for tools/analyze_branch_regressions.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile


TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    path = TOOLS_DIR / "analyze_branch_regressions.py"
    spec = importlib.util.spec_from_file_location("analyze_branch_regressions", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["analyze_branch_regressions"] = module
    spec.loader.exec_module(module)
    return module


def _make_record(
    instance_id: str,
    tier: str,
    backend: str,
    elapsed_ns: int,
    status: str = "ok",
    rw_delegations: int = 0,
    rw_skips: int = 0,
    tw_delegations: int = 0,
    fallthroughs: int = 0,
    rw_source: str = "auto",
) -> dict:
    return {
        "schema": "sop_bench_result_v2",
        "instance_id": instance_id,
        "tier": tier,
        "backend": backend,
        "backend_config": {"branch_rw_source": rw_source},
        "elapsed_ns": elapsed_ns,
        "solve_elapsed_ns": elapsed_ns,
        "status": status,
        "stats": {
            "rankwidth_delegations": rw_delegations,
            "branch_rankwidth_skips": rw_skips,
            "treewidth_delegations": tw_delegations,
            "branch_fallthroughs": fallthroughs,
        },
        "source": "Internal corpus",
        "provenance": {"source": "Internal corpus"},
    }


def _write_jsonl(records: list[dict]) -> pathlib.Path:
    tmp = tempfile.NamedTemporaryFile(
        mode="w", suffix=".jsonl", delete=False, encoding="utf-8"
    )
    for r in records:
        tmp.write(json.dumps(r) + "\n")
    tmp.close()
    return pathlib.Path(tmp.name)


def test_load_and_group(tmp_path):
    tool = _load_tool()
    records_raw = [
        _make_record("inst1", "0-32", "treewidth", 1_000_000),
        _make_record("inst1", "0-32", "branch", 2_000_000, rw_source="auto"),
        _make_record("inst1", "0-32", "branch", 1_500_000, rw_source="none"),
        _make_record("inst2", "33-64", "treewidth", 5_000_000),
        _make_record("inst2", "33-64", "branch", 6_000_000, rw_source="auto"),
    ]
    jf = tmp_path / "test.jsonl"
    jf.write_text("\n".join(json.dumps(r) for r in records_raw) + "\n")

    sys.path.insert(0, str(TOOLS_DIR))
    records = tool.load_jsonl_dir(tmp_path)
    assert len(records) == 5
    grouped = tool.group_by_tier_backend(records)
    assert "0-32" in grouped
    assert "33-64" in grouped


def test_top_offenders_vs_treewidth(tmp_path):
    tool = _load_tool()
    # Create InstanceRecord objects directly with canonical backend names
    records = [
        tool.InstanceRecord("inst-A", "0-32", "treewidth", 1_000_000, "ok",
                            {"rankwidth_delegations": 0, "branch_rankwidth_skips": 0}),
        tool.InstanceRecord("inst-A", "0-32", "branch:auto", 5_000_000, "ok",
                            {"rankwidth_delegations": 0, "branch_rankwidth_skips": 10}),
        tool.InstanceRecord("inst-B", "0-32", "treewidth", 1_000_000, "ok", {}),
        tool.InstanceRecord("inst-B", "0-32", "branch:auto", 2_000_000, "ok", {}),
    ]
    offenders = tool.top_offenders_branch_vs_treewidth(records)
    assert len(offenders) >= 1
    # inst-A has larger excess (4_000_000 vs 1_000_000)
    assert offenders[0]["instance_id"] == "inst-A"
    assert offenders[0]["ratio"] == 5.0


def test_top_offenders_vs_no_rankwidth(tmp_path):
    tool = _load_tool()
    # Create InstanceRecord objects directly with canonical backend names
    records = [
        tool.InstanceRecord("inst-C", "65-128", "branch:auto", 10_000_000, "ok", {}),
        tool.InstanceRecord("inst-C", "65-128", "branch:no-rankwidth", 4_000_000, "ok", {}),
        tool.InstanceRecord("inst-D", "65-128", "branch:auto", 3_000_000, "ok", {}),
        tool.InstanceRecord("inst-D", "65-128", "branch:no-rankwidth", 2_500_000, "ok", {}),
    ]
    offenders = tool.top_offenders_branch_vs_no_rankwidth(records)
    assert len(offenders) >= 1
    # inst-C has larger excess (6_000_000 vs 500_000)
    assert offenders[0]["instance_id"] == "inst-C"


def test_tier_summary_rw_zero_delegations(tmp_path):
    tool = _load_tool()
    records_raw = [
        _make_record("i1", "0-32", "treewidth", 1_000_000),
        _make_record("i1", "0-32", "branch", 2_000_000, rw_source="auto",
                     rw_skips=5, rw_delegations=0),
    ]
    jf = tmp_path / "data.jsonl"
    jf.write_text("\n".join(json.dumps(r) for r in records_raw) + "\n")

    sys.path.insert(0, str(TOOLS_DIR))
    all_records = tool.load_jsonl_dir(tmp_path)
    grouped = tool.group_by_tier_backend(all_records)
    summary = tool.build_tier_summary(grouped)

    assert len(summary) >= 1
    row = summary[0]
    assert row["rw_delegations"] == 0
    assert row["rw_skips"] == 5
    assert row["br_vs_tw_ratio"] != "N/A"


def test_render_markdown_non_empty(tmp_path):
    tool = _load_tool()
    tier_summary = [{
        "tier": "0-32",
        "tw_ok": 100,
        "tw_ns": 1_000_000_000,
        "br_ok": 100,
        "br_ns": 1_500_000_000,
        "nr_ok": 100,
        "nr_ns": 1_200_000_000,
        "rw_delegations": 0,
        "rw_skips": 50,
        "tw_delegations": 100,
        "fallthroughs": 10,
        "br_vs_tw_ratio": "1.500x",
        "br_vs_nr_ratio": "1.250x",
    }]
    md = tool.render_markdown(tier_summary, [], [], [])
    assert "# Branch Regression Analysis" in md
    assert "0-32" in md
    assert "rw_skips=50" in md


def test_main_with_empty_dir(tmp_path):
    tool = _load_tool()
    empty = tmp_path / "empty"
    empty.mkdir()
    out_md = tmp_path / "out.md"
    rc = tool.main([
        "--artifact-dir", str(empty),
        "--output", str(out_md),
    ])
    assert rc == 1  # No records → error


def test_main_with_records(tmp_path):
    tool = _load_tool()
    records_raw = [
        _make_record("x1", "0-32", "treewidth", 1_000_000),
        _make_record("x1", "0-32", "branch", 2_000_000, rw_source="auto", rw_skips=3),
        _make_record("x1", "0-32", "branch", 1_800_000, rw_source="none"),
    ]
    art_dir = tmp_path / "artifacts"
    art_dir.mkdir()
    (art_dir / "data.jsonl").write_text(
        "\n".join(json.dumps(r) for r in records_raw) + "\n"
    )
    out_md = tmp_path / "report.md"
    out_json = tmp_path / "report.json"
    rc = tool.main([
        "--artifact-dir", str(art_dir),
        "--output", str(out_md),
        "--json", str(out_json),
    ])
    assert rc == 0
    assert out_md.exists()
    assert "# Branch Regression" in out_md.read_text()
    assert out_json.exists()
    data = json.loads(out_json.read_text())
    assert "tier_summary" in data


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
