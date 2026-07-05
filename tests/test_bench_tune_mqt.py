#!/usr/bin/env python3
"""Smoke tests for MQT tuning: bench.py subcommand and render_scoreboard.write_mqt_tuning_summary."""

import argparse
import importlib.util
import io
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "scripts"


def _load_module(name: str) -> object:
    tools_str = str(TOOLS_DIR)
    if tools_str not in sys.path:
        sys.path.insert(0, tools_str)
    path = TOOLS_DIR / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _make_jsonl(records, path):
    path.write_text("\n".join(json.dumps(r) for r in records) + "\n", encoding="utf-8")


def test_write_mqt_tuning_summary_empty_records(renderer, tmp):
    buf = io.StringIO()
    renderer.write_mqt_tuning_summary([], buf)
    content = buf.getvalue()
    if "No MQT tuning records" not in content:
        raise AssertionError("missing 'No MQT tuning records' in output")


def test_write_mqt_tuning_summary_with_records(renderer, tmp):
    records = [
        {"backend": "treewidth", "status": "ok", "elapsed_ns": 1_000_000},
        {"backend": "treewidth", "status": "ok", "elapsed_ns": 2_000_000},
        {"backend": "branch:auto", "status": "ok", "elapsed_ns": 3_000_000},
        {"backend": "branch:auto", "status": "timeout", "elapsed_ns": 0},
        {"backend": "rankwidth:best", "status": "ok", "elapsed_ns": 5_000_000},
    ]
    buf = io.StringIO()
    renderer.write_mqt_tuning_summary(records, buf)
    content = buf.getvalue()
    if "# MQT Tuning Summary" not in content:
        raise AssertionError("missing heading")
    if "treewidth" not in content:
        raise AssertionError("missing treewidth")
    if "branch:auto" not in content:
        raise AssertionError("missing branch:auto")


def test_write_mqt_tuning_summary_missing_file_exits_nonzero(renderer, tmp):
    import subprocess
    missing = tmp / "nonexistent.jsonl"
    out = tmp / "summary.md"
    result = subprocess.run(
        [sys.executable, str(TOOLS_DIR / "render_scoreboard.py"),
         "--mqt-tuning-jsonl", str(missing),
         "--output", str(out)],
        capture_output=True,
    )
    if result.returncode == 0:
        raise AssertionError("expected non-zero exit for missing input file")


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


def main() -> int:
    tool = _load_module("bench")
    renderer = _load_module("render_scoreboard")
    tests = [
        ("write_mqt_tuning_summary_empty_records", lambda t: test_write_mqt_tuning_summary_empty_records(renderer, t)),
        ("write_mqt_tuning_summary_with_records", lambda t: test_write_mqt_tuning_summary_with_records(renderer, t)),
        ("write_mqt_tuning_summary_missing_file_exits_nonzero", lambda t: test_write_mqt_tuning_summary_missing_file_exits_nonzero(renderer, t)),
        ("cmd_tune_mqt_missing_corpus", lambda t: test_cmd_tune_mqt_missing_corpus(tool, t)),
        ("mqt_default_backends_non_empty", lambda t: test_mqt_default_backends_non_empty(tool, t)),
    ]
    failed = []
    for name, fn in tests:
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            try:
                fn(tmp)
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
