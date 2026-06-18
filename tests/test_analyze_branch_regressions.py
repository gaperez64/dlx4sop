#!/usr/bin/env python3
"""Smoke tests for tools/analyze_branch_regressions.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    path = TOOLS_DIR / "analyze_branch_regressions.py"
    spec = importlib.util.spec_from_file_location("analyze_branch_regressions", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["analyze_branch_regressions"] = module
    spec.loader.exec_module(module)
    return module


def _make_record(instance_id, tier, backend, elapsed_ns, status="ok",
                 rw_delegations=0, rw_skips=0, tw_delegations=0,
                 fallthroughs=0, rw_source="auto"):
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


def test_load_and_group(tool, tmp):
    records_raw = [
        _make_record("inst1", "0-32", "treewidth", 1_000_000),
        _make_record("inst1", "0-32", "branch", 2_000_000, rw_source="auto"),
        _make_record("inst1", "0-32", "branch", 1_500_000, rw_source="none"),
        _make_record("inst2", "33-64", "treewidth", 5_000_000),
        _make_record("inst2", "33-64", "branch", 6_000_000, rw_source="auto"),
    ]
    jf = tmp / "test.jsonl"
    jf.write_text("\n".join(json.dumps(r) for r in records_raw) + "\n")
    records = tool.load_jsonl_dir(tmp)
    if len(records) != 5:
        raise AssertionError(f"expected 5 records, got {len(records)}")
    grouped = tool.group_by_tier_backend(records)
    if "0-32" not in grouped:
        raise AssertionError("missing tier 0-32 in grouped")
    if "33-64" not in grouped:
        raise AssertionError("missing tier 33-64 in grouped")


def test_top_offenders_vs_treewidth(tool, tmp):
    records = [
        tool.InstanceRecord("inst-A", "0-32", "treewidth", 1_000_000, "ok",
                            {"rankwidth_delegations": 0, "branch_rankwidth_skips": 0}),
        tool.InstanceRecord("inst-A", "0-32", "branch:auto", 5_000_000, "ok",
                            {"rankwidth_delegations": 0, "branch_rankwidth_skips": 10}),
        tool.InstanceRecord("inst-B", "0-32", "treewidth", 1_000_000, "ok", {}),
        tool.InstanceRecord("inst-B", "0-32", "branch:auto", 2_000_000, "ok", {}),
    ]
    offenders = tool.top_offenders_branch_vs_treewidth(records)
    if not offenders:
        raise AssertionError("expected offenders, got none")
    if offenders[0]["instance_id"] != "inst-A":
        raise AssertionError(f"expected inst-A first, got {offenders[0]['instance_id']}")
    if offenders[0]["ratio"] != 5.0:
        raise AssertionError(f"expected ratio 5.0, got {offenders[0]['ratio']}")


def test_top_offenders_vs_no_rankwidth(tool, tmp):
    records = [
        tool.InstanceRecord("inst-C", "65-128", "branch:auto", 10_000_000, "ok", {}),
        tool.InstanceRecord("inst-C", "65-128", "branch:no-rankwidth", 4_000_000, "ok", {}),
        tool.InstanceRecord("inst-D", "65-128", "branch:auto", 3_000_000, "ok", {}),
        tool.InstanceRecord("inst-D", "65-128", "branch:no-rankwidth", 2_500_000, "ok", {}),
    ]
    offenders = tool.top_offenders_branch_vs_no_rankwidth(records)
    if not offenders:
        raise AssertionError("expected offenders, got none")
    if offenders[0]["instance_id"] != "inst-C":
        raise AssertionError(f"expected inst-C first, got {offenders[0]['instance_id']}")


def test_tier_summary_rw_zero_delegations(tool, tmp):
    records_raw = [
        _make_record("i1", "0-32", "treewidth", 1_000_000),
        _make_record("i1", "0-32", "branch", 2_000_000, rw_source="auto",
                     rw_skips=5, rw_delegations=0),
    ]
    jf = tmp / "data.jsonl"
    jf.write_text("\n".join(json.dumps(r) for r in records_raw) + "\n")
    all_records = tool.load_jsonl_dir(tmp)
    grouped = tool.group_by_tier_backend(all_records)
    summary = tool.build_tier_summary(grouped)
    if not summary:
        raise AssertionError("expected tier summary rows")
    row = summary[0]
    if row["rw_delegations"] != 0:
        raise AssertionError(f"expected 0 rw_delegations, got {row['rw_delegations']}")
    if row["rw_skips"] != 5:
        raise AssertionError(f"expected 5 rw_skips, got {row['rw_skips']}")
    if row["br_vs_tw_ratio"] == "N/A":
        raise AssertionError("br_vs_tw_ratio should not be N/A")


def test_render_markdown_non_empty(tool, tmp):
    tier_summary = [{
        "tier": "0-32",
        "tw_ok": 100, "tw_ns": 1_000_000_000,
        "br_ok": 100, "br_ns": 1_500_000_000,
        "nr_ok": 100, "nr_ns": 1_200_000_000,
        "rw_delegations": 0, "rw_skips": 50,
        "tw_delegations": 100, "fallthroughs": 10,
        "br_vs_tw_ratio": "1.500x", "br_vs_nr_ratio": "1.250x",
    }]
    md = tool.render_markdown(tier_summary, [], [], [])
    if "# Branch Regression Analysis" not in md:
        raise AssertionError("missing heading")
    if "0-32" not in md:
        raise AssertionError("missing tier")
    if "rw_skips=50" not in md:
        raise AssertionError("missing rw_skips=50")


def test_main_with_empty_dir(tool, tmp):
    empty = tmp / "empty"
    empty.mkdir()
    out_md = tmp / "out.md"
    rc = tool.main([
        "--artifact-dir", str(empty),
        "--output", str(out_md),
    ])
    if rc != 1:
        raise AssertionError(f"expected rc=1 for empty dir, got rc={rc}")


def test_main_with_records(tool, tmp):
    records_raw = [
        _make_record("x1", "0-32", "treewidth", 1_000_000),
        _make_record("x1", "0-32", "branch", 2_000_000, rw_source="auto", rw_skips=3),
        _make_record("x1", "0-32", "branch", 1_800_000, rw_source="none"),
    ]
    art_dir = tmp / "artifacts"
    art_dir.mkdir()
    (art_dir / "data.jsonl").write_text(
        "\n".join(json.dumps(r) for r in records_raw) + "\n"
    )
    out_md = tmp / "report.md"
    out_json = tmp / "report.json"
    rc = tool.main([
        "--artifact-dir", str(art_dir),
        "--output", str(out_md),
        "--json", str(out_json),
    ])
    if rc != 0:
        raise AssertionError(f"expected rc=0, got rc={rc}")
    if not out_md.exists():
        raise AssertionError("output md not created")
    if "# Branch Regression" not in out_md.read_text():
        raise AssertionError("missing heading in output")
    if not out_json.exists():
        raise AssertionError("output json not created")
    data = json.loads(out_json.read_text())
    if "tier_summary" not in data:
        raise AssertionError("missing tier_summary in json output")


def main() -> int:
    tool = _load_tool()
    tests = [
        ("load_and_group", test_load_and_group),
        ("top_offenders_vs_treewidth", test_top_offenders_vs_treewidth),
        ("top_offenders_vs_no_rankwidth", test_top_offenders_vs_no_rankwidth),
        ("tier_summary_rw_zero_delegations", test_tier_summary_rw_zero_delegations),
        ("render_markdown_non_empty", test_render_markdown_non_empty),
        ("main_with_empty_dir", test_main_with_empty_dir),
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
