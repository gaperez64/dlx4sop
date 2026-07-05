#!/usr/bin/env python3
"""Smoke tests for scripts/plot_scoreboard.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "scripts"


def _load_tool():
    path = TOOLS_DIR / "plot_scoreboard.py"
    spec = importlib.util.spec_from_file_location("plot_scoreboard", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["plot_scoreboard"] = module
    spec.loader.exec_module(module)
    return module


def _make_record(instance_id, tier, backend, elapsed_ns, status="ok",
                 source="FeynmanDD", rw_source="auto"):
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


def test_svg_doc_renders(tool, tmp):
    doc = tool.SVGDoc(400, 300)
    doc.rect(10, 10, 100, 50, fill="#red")
    doc.line(0, 0, 100, 100, stroke="#blue")
    doc.text(50, 50, "Hello & <World>", anchor="middle")
    doc.polyline([(0, 0), (100, 50), (200, 100)], stroke="#green")
    svg = doc.close()
    if not svg.startswith("<svg"):
        raise AssertionError("SVG must start with <svg")
    if not svg.endswith("</svg>"):
        raise AssertionError("SVG must end with </svg>")
    if "&amp;" not in svg:
        raise AssertionError("& must be escaped as &amp;")
    if "&lt;" not in svg:
        raise AssertionError("< must be escaped as &lt;")


def test_svg_text_escape(tool, tmp):
    assert tool._svg_text_escape("a & b") == "a &amp; b"
    assert tool._svg_text_escape("<tag>") == "&lt;tag&gt;"
    assert tool._svg_text_escape("plain") == "plain"


def test_canonical_backend(tool, tmp):
    cases = [
        ({"backend": "treewidth"}, "treewidth"),
        ({"backend": "branch", "backend_config": {"branch_rw_source": "auto"}}, "branch:auto"),
        ({"backend": "branch", "backend_config": {"branch_rw_source": "none"}}, "branch:no-rankwidth"),
        ({"backend": "branch", "backend_config": {"branch_rw_source": "from-treewidth"}}, "branch:from-treewidth"),
    ]
    for rec, expected in cases:
        got = tool._canonical_backend(rec)
        if got != expected:
            raise AssertionError(f"_canonical_backend({rec}) = {got!r}, expected {expected!r}")


def test_survival_curves_empty(tool, tmp):
    curves = tool._survival_curves([], "FeynmanDD", ["treewidth", "branch:auto"])
    if curves != {}:
        raise AssertionError(f"expected empty curves, got {curves}")


def test_survival_curves_one_source(tool, tmp):
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000, source="FeynmanDD"),
        _make_record("i2", "0-32", "treewidth", 50_000_000, source="FeynmanDD"),
        _make_record("i3", "0-32", "treewidth", 1_000_000_000, source="PyZX"),
    ]
    curves = tool._survival_curves(records, "FeynmanDD", ["treewidth"])
    if "treewidth" not in curves:
        raise AssertionError("missing treewidth curve")
    budgets, fracs = curves["treewidth"]
    if len(budgets) != len(tool.NS_BUDGETS):
        raise AssertionError(f"expected {len(tool.NS_BUDGETS)} budget points, got {len(budgets)}")
    if fracs[0] != 0.5:
        raise AssertionError(f"expected 0.5 at 1ms budget, got {fracs[0]}")
    if fracs[4] != 1.0:
        raise AssertionError(f"expected 1.0 at 100ms budget, got {fracs[4]}")


def test_plot_survival_svg_outputs_file(tool, tmp):
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000, source="FeynmanDD"),
        _make_record("i2", "0-32", "branch", 2_000_000, source="FeynmanDD", rw_source="auto"),
    ]
    out = tmp / "survival-test.svg"
    tool.plot_survival_svg(records, "FeynmanDD", out)
    if not out.exists():
        raise AssertionError("survival SVG not created")
    content = out.read_text()
    if "<svg" not in content:
        raise AssertionError("missing <svg in output")
    if "FeynmanDD" not in content:
        raise AssertionError("missing source name in SVG")


def test_plot_survival_svg_no_data(tool, tmp):
    # When no data and the output file doesn't exist, a placeholder is written
    # so that scoreboard image references are not broken.
    out = tmp / "survival-empty.svg"
    tool.plot_survival_svg([], "FeynmanDD", out)
    if not out.exists():
        raise AssertionError("expected placeholder SVG when no data and file absent")
    content = out.read_text()
    if "<svg" not in content:
        raise AssertionError("placeholder must be a valid SVG element")
    # When the file already exists (e.g. committed with real data), it is not overwritten.
    original = out.read_text()
    tool.plot_survival_svg([], "FeynmanDD", out)
    if out.read_text() != original:
        raise AssertionError("existing SVG should not be overwritten when no data")


def test_plot_solver_time_by_tier(tool, tmp):
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000),
        _make_record("i2", "0-32", "branch", 2_000_000, rw_source="auto"),
        _make_record("i3", "33-64", "treewidth", 5_000_000),
    ]
    out = tmp / "tier-plot.svg"
    tool.plot_solver_time_by_tier_svg(records, out)
    if not out.exists():
        raise AssertionError("tier-time SVG not created")
    if "<svg" not in out.read_text():
        raise AssertionError("missing <svg in output")


def test_plot_speedup_vs_treewidth(tool, tmp):
    records = [
        _make_record("i1", "0-32", "treewidth", 2_000_000),
        _make_record("i1", "0-32", "branch", 1_000_000, rw_source="auto"),
    ]
    out = tmp / "speedup.svg"
    tool.plot_speedup_vs_treewidth_svg(records, out)
    if not out.exists():
        raise AssertionError("speedup SVG not created")


def test_plot_branch_dispatch(tool, tmp):
    records = [{
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
    }]
    out = tmp / "dispatch.svg"
    tool.plot_branch_dispatch_svg(records, out)
    if not out.exists():
        raise AssertionError("dispatch SVG not created")


def test_main_no_records(tool, tmp):
    out_dir = tmp / "plots"
    rc = tool.main([
        "--artifact-dir", str(tmp / "nonexistent"),
        "--output-dir", str(out_dir),
    ])
    if rc != 0:
        raise AssertionError(f"expected rc=0 with no records, got {rc}")


def test_main_with_records(tool, tmp):
    records = [
        _make_record("i1", "0-32", "treewidth", 1_000_000, source="FeynmanDD"),
        _make_record("i2", "0-32", "branch", 2_000_000, source="FeynmanDD", rw_source="auto"),
        _make_record("i3", "33-64", "treewidth", 5_000_000, source="MQT Bench"),
    ]
    art_dir = tmp / "artifacts"
    art_dir.mkdir()
    (art_dir / "data.jsonl").write_text(
        "\n".join(json.dumps(r) for r in records) + "\n"
    )
    out_dir = tmp / "plots"
    rc = tool.main([
        "--artifact-dir", str(art_dir),
        "--output-dir", str(out_dir),
        "--sources", "FeynmanDD",
        "--sources", "MQT Bench",
    ])
    if rc != 0:
        raise AssertionError(f"expected rc=0, got {rc}")
    if not out_dir.exists():
        raise AssertionError("output directory not created")


def main() -> int:
    tool = _load_tool()
    tests = [
        ("svg_doc_renders", test_svg_doc_renders),
        ("svg_text_escape", test_svg_text_escape),
        ("canonical_backend", test_canonical_backend),
        ("survival_curves_empty", test_survival_curves_empty),
        ("survival_curves_one_source", test_survival_curves_one_source),
        ("plot_survival_svg_outputs_file", test_plot_survival_svg_outputs_file),
        ("plot_survival_svg_no_data", test_plot_survival_svg_no_data),
        ("plot_solver_time_by_tier", test_plot_solver_time_by_tier),
        ("plot_speedup_vs_treewidth", test_plot_speedup_vs_treewidth),
        ("plot_branch_dispatch", test_plot_branch_dispatch),
        ("main_no_records", test_main_no_records),
        ("main_with_records", test_main_with_records),
    ]
    failed = []
    for name, fn in tests:
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            try:
                fn(tool, tmp)
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
