#!/usr/bin/env python3
"""Tests for tools/plot_scoreboard.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    path = TOOLS_DIR / "plot_scoreboard.py"
    spec = importlib.util.spec_from_file_location("plot_scoreboard", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _make_record(
    instance_id: str,
    tier: str,
    backend: str,
    elapsed_ns: int,
    status: str = "ok",
    source: str = "FeynmanDD",
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
        "stats": {},
        "source": source,
        "provenance": {"source": source},
    }


def test_svg_doc_renders():
    tool = _load_tool()
    doc = tool.SVGDoc(400, 300)
    doc.rect(10, 10, 100, 50, fill="#red")
    doc.line(0, 0, 100, 100, stroke="#blue")
    doc.text(50, 50, "Hello & <World>", anchor="middle")
    doc.polyline([(0, 0), (100, 50), (200, 100)], stroke="#green")
    svg = doc.close()
    assert svg.startswith("<svg")
    assert svg.endswith("</svg>")
    assert "&amp;" in svg
    assert "&lt;" in svg


def test_svg_text_escape():
    tool = _load_tool()
    assert tool._svg_text_escape("a & b") == "a &amp; b"
    assert tool._svg_text_escape("<tag>") == "&lt;tag&gt;"
    assert tool._svg_text_escape("plain") == "plain"


def test_canonical_backend():
    tool = _load_tool()
    assert tool._canonical_backend({"backend": "treewidth"}) == "treewidth"
    assert tool._canonical_backend({
        "backend": "branch",
        "backend_config": {"branch_rw_source": "auto"},
    }) == "branch:auto"
    assert tool._canonical_backend({
        "backend": "branch",
        "backend_config": {"branch_rw_source": "none"},
    }) == "branch:no-rankwidth"
    assert tool._canonical_backend({
        "backend": "branch",
        "backend_config": {"branch_rw_source": "from-treewidth"},
    }) == "branch:from-treewidth"


def test_survival_curves_empty():
    tool = _load_tool()
    curves = tool._survival_curves([], "FeynmanDD", ["treewidth", "branch:auto"])
    assert curves == {}


def test_survival_curves_one_source():
    tool = _load_tool()
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000, source="FeynmanDD"),
        _make_record("i2", "0-32", "treewidth", 50_000_000, source="FeynmanDD"),
        _make_record("i3", "0-32", "treewidth", 1_000_000_000, source="PyZX"),
    ]
    curves = tool._survival_curves(records, "FeynmanDD", ["treewidth"])
    assert "treewidth" in curves
    budgets, fracs = curves["treewidth"]
    assert len(budgets) == len(tool.NS_BUDGETS)
    # At 1ms budget: 1 out of 2 FeynmanDD records is solved
    # budgets[0] = 1ms = 1_000_000
    assert fracs[0] == 0.5
    # At 100ms budget: both solved
    assert fracs[4] == 1.0


def test_plot_survival_svg_outputs_file(tmp_path):
    tool = _load_tool()
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000, source="FeynmanDD"),
        _make_record("i2", "0-32", "branch", 2_000_000, source="FeynmanDD", rw_source="auto"),
    ]
    out = tmp_path / "survival-test.svg"
    tool.plot_survival_svg(records, "FeynmanDD", out)
    assert out.exists()
    content = out.read_text()
    assert "<svg" in content
    assert "FeynmanDD" in content


def test_plot_survival_svg_no_data(tmp_path):
    """Should not crash with no matching records."""
    tool = _load_tool()
    out = tmp_path / "survival-empty.svg"
    tool.plot_survival_svg([], "FeynmanDD", out)
    assert not out.exists()  # Nothing to plot → no file


def test_plot_solver_time_by_tier(tmp_path):
    tool = _load_tool()
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000),
        _make_record("i2", "0-32", "branch", 2_000_000, rw_source="auto"),
        _make_record("i3", "33-64", "treewidth", 5_000_000),
    ]
    out = tmp_path / "tier-plot.svg"
    tool.plot_solver_time_by_tier_svg(records, out)
    assert out.exists()
    content = out.read_text()
    assert "<svg" in content


def test_plot_speedup_vs_treewidth(tmp_path):
    tool = _load_tool()
    records = [
        _make_record("i1", "0-32", "treewidth", 2_000_000),
        _make_record("i1", "0-32", "branch", 1_000_000, rw_source="auto"),
    ]
    out = tmp_path / "speedup.svg"
    tool.plot_speedup_vs_treewidth_svg(records, out)
    assert out.exists()


def test_plot_branch_dispatch(tmp_path):
    tool = _load_tool()
    records = [
        {
            "schema": "sop_bench_result_v2",
            "instance_id": "i1",
            "tier": "0-32",
            "backend": "branch",
            "backend_config": {"branch_rw_source": "auto"},
            "elapsed_ns": 1_000_000,
            "status": "ok",
            "stats": {
                "treewidth_delegations": 50,
                "rankwidth_delegations": 0,
                "branch_fallthroughs": 10,
            },
            "provenance": {"source": "FeynmanDD"},
        }
    ]
    out = tmp_path / "dispatch.svg"
    tool.plot_branch_dispatch_svg(records, out)
    assert out.exists()


def test_main_no_records(tmp_path):
    tool = _load_tool()
    out_dir = tmp_path / "plots"
    rc = tool.main([
        "--artifact-dir", str(tmp_path / "nonexistent"),
        "--output-dir", str(out_dir),
    ])
    assert rc == 0  # Should complete even with no records


def test_main_with_records(tmp_path):
    tool = _load_tool()
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000, source="FeynmanDD"),
        _make_record("i2", "0-32", "branch", 2_000_000, source="FeynmanDD", rw_source="auto"),
        _make_record("i3", "33-64", "treewidth", 5_000_000, source="MQT Bench"),
    ]
    art_dir = tmp_path / "artifacts"
    art_dir.mkdir()
    (art_dir / "data.jsonl").write_text(
        "\n".join(json.dumps(r) for r in records) + "\n"
    )
    out_dir = tmp_path / "plots"
    rc = tool.main([
        "--artifact-dir", str(art_dir),
        "--output-dir", str(out_dir),
        "--sources", "FeynmanDD",
        "--sources", "MQT Bench",
    ])
    assert rc == 0
    assert out_dir.exists()


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
