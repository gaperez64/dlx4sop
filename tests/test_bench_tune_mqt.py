#!/usr/bin/env python3
"""Smoke tests for tools/bench.py (tune-mqt subcommand and _render_mqt_summary)."""

import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    path = TOOLS_DIR / "bench.py"
    spec = importlib.util.spec_from_file_location("bench", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["bench"] = module
    spec.loader.exec_module(module)
    return module


def _make_jsonl(records, path):
    path.write_text("\n".join(json.dumps(r) for r in records) + "\n", encoding="utf-8")


def test_render_mqt_summary_empty_records(tool, tmp):
    empty_jsonl = tmp / "empty.jsonl"
    empty_jsonl.write_text("", encoding="utf-8")
    out = tmp / "summary.md"
    tool._render_mqt_summary(empty_jsonl, out)
    if not out.exists():
        raise AssertionError("summary.md not created for empty input")
    if "No MQT tuning records" not in out.read_text():
        raise AssertionError("missing 'No MQT tuning records' in output")


def test_render_mqt_summary_with_records(tool, tmp):
    records = [
        {"backend": "treewidth", "status": "ok", "elapsed_ns": 1_000_000},
        {"backend": "treewidth", "status": "ok", "elapsed_ns": 2_000_000},
        {"backend": "branch:auto", "status": "ok", "elapsed_ns": 3_000_000},
        {"backend": "branch:auto", "status": "timeout", "elapsed_ns": 0},
        {"backend": "rankwidth:best", "status": "ok", "elapsed_ns": 5_000_000},
    ]
    jsonl = tmp / "data.jsonl"
    _make_jsonl(records, jsonl)
    out = tmp / "summary.md"
    tool._render_mqt_summary(jsonl, out)
    if not out.exists():
        raise AssertionError("summary not created")
    content = out.read_text()
    if "# MQT Tuning Summary" not in content:
        raise AssertionError("missing heading")
    if "treewidth" not in content:
        raise AssertionError("missing treewidth")
    if "branch:auto" not in content:
        raise AssertionError("missing branch:auto")


def test_render_mqt_summary_missing_file(tool, tmp):
    missing = tmp / "nonexistent.jsonl"
    out = tmp / "summary.md"
    tool._render_mqt_summary(missing, out)
    if out.exists():
        raise AssertionError("should not create summary for missing file")


def test_cmd_tune_mqt_missing_corpus(tool, tmp):
    args = argparse.Namespace(
        corpus=tmp / "nonexistent-corpus",
        artifact_dir=tmp / "artifacts",
        sop_solve=pathlib.Path("/nonexistent/sop-solve"),
        backends=None,
        timeout=5,
    )
    rc = tool.cmd_tune_mqt(args)
    if rc != 1:
        raise AssertionError(f"expected rc=1 for missing corpus, got {rc}")


def test_mqt_default_backends_non_empty(tool, tmp):
    if len(tool.MQT_DEFAULT_BACKENDS) < 3:
        raise AssertionError("expected at least 3 MQT default backends")
    for expected in ("treewidth", "branch:auto", "branch:no-rankwidth"):
        if expected not in tool.MQT_DEFAULT_BACKENDS:
            raise AssertionError(f"missing {expected!r} in MQT_DEFAULT_BACKENDS")


def test_render_mqt_summary_malformed_json(tool, tmp):
    jsonl = tmp / "malformed.jsonl"
    jsonl.write_text(
        '{"backend": "treewidth", "status": "ok", "elapsed_ns": 1000000}\n'
        "NOT VALID JSON\n"
        '{"backend": "branch:auto", "status": "ok", "elapsed_ns": 2000000}\n',
        encoding="utf-8",
    )
    out = tmp / "summary.md"
    tool._render_mqt_summary(jsonl, out)
    if not out.exists():
        raise AssertionError("should still create summary with malformed lines")
    content = out.read_text()
    if "treewidth" not in content:
        raise AssertionError("missing treewidth in summary")
    if "branch:auto" not in content:
        raise AssertionError("missing branch:auto in summary")


def main() -> int:
    tool = _load_tool()
    tests = [
        ("render_mqt_summary_empty_records", test_render_mqt_summary_empty_records),
        ("render_mqt_summary_with_records", test_render_mqt_summary_with_records),
        ("render_mqt_summary_missing_file", test_render_mqt_summary_missing_file),
        ("cmd_tune_mqt_missing_corpus", test_cmd_tune_mqt_missing_corpus),
        ("mqt_default_backends_non_empty", test_mqt_default_backends_non_empty),
        ("render_mqt_summary_malformed_json", test_render_mqt_summary_malformed_json),
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
