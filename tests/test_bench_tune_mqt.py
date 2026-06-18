#!/usr/bin/env python3
"""Tests for tools/bench.py (tune-mqt subcommand and _render_mqt_summary)."""

import importlib.util
import json
import pathlib
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    path = TOOLS_DIR / "bench.py"
    spec = importlib.util.spec_from_file_location("bench", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _make_jsonl(records: list[dict], path: pathlib.Path) -> None:
    path.write_text(
        "\n".join(json.dumps(r) for r in records) + "\n", encoding="utf-8"
    )


def test_render_mqt_summary_empty_records(tmp_path):
    tool = _load_tool()
    empty_jsonl = tmp_path / "empty.jsonl"
    empty_jsonl.write_text("", encoding="utf-8")
    out = tmp_path / "summary.md"
    tool._render_mqt_summary(empty_jsonl, out)
    assert out.exists()
    content = out.read_text()
    assert "No MQT tuning records" in content


def test_render_mqt_summary_with_records(tmp_path):
    tool = _load_tool()
    records = [
        {"backend": "treewidth", "status": "ok", "elapsed_ns": 1_000_000, "tier": "0-32"},
        {"backend": "treewidth", "status": "ok", "elapsed_ns": 2_000_000, "tier": "0-32"},
        {"backend": "branch:auto", "status": "ok", "elapsed_ns": 3_000_000, "tier": "0-32"},
        {"backend": "branch:auto", "status": "timeout", "elapsed_ns": 0, "tier": "33-64"},
        {"backend": "rankwidth:best", "status": "ok", "elapsed_ns": 5_000_000, "tier": "0-32"},
    ]
    jsonl = tmp_path / "mqt-tuning.jsonl"
    _make_jsonl(records, jsonl)
    out = tmp_path / "summary.md"
    tool._render_mqt_summary(jsonl, out)
    assert out.exists()
    content = out.read_text()
    assert "# MQT Tuning Summary" in content
    assert "treewidth" in content
    assert "branch:auto" in content
    assert "| 2 |" in content  # treewidth solved 2


def test_render_mqt_summary_missing_file(tmp_path):
    """Should silently do nothing if JSONL doesn't exist."""
    tool = _load_tool()
    missing = tmp_path / "nonexistent.jsonl"
    out = tmp_path / "summary.md"
    tool._render_mqt_summary(missing, out)
    assert not out.exists()


def test_cmd_tune_mqt_missing_corpus(tmp_path):
    """Should return error code 1 if corpus dir doesn't exist."""
    tool = _load_tool()
    import argparse
    args = argparse.Namespace(
        corpus=tmp_path / "nonexistent-corpus",
        artifact_dir=tmp_path / "artifacts",
        sop_solve=pathlib.Path("/nonexistent/sop-solve"),
        backends=None,
        timeout=5,
    )
    rc = tool.cmd_tune_mqt(args)
    assert rc == 1


def test_mqt_default_backends_non_empty():
    tool = _load_tool()
    assert len(tool.MQT_DEFAULT_BACKENDS) >= 3
    assert "treewidth" in tool.MQT_DEFAULT_BACKENDS
    assert "branch:auto" in tool.MQT_DEFAULT_BACKENDS
    assert "branch:no-rankwidth" in tool.MQT_DEFAULT_BACKENDS


def test_render_mqt_summary_malformed_json(tmp_path):
    """Should skip malformed JSON lines gracefully."""
    tool = _load_tool()
    jsonl = tmp_path / "malformed.jsonl"
    jsonl.write_text(
        '{"backend": "treewidth", "status": "ok", "elapsed_ns": 1000000}\n'
        "NOT VALID JSON\n"
        '{"backend": "branch:auto", "status": "ok", "elapsed_ns": 2000000}\n',
        encoding="utf-8",
    )
    out = tmp_path / "summary.md"
    tool._render_mqt_summary(jsonl, out)
    assert out.exists()
    content = out.read_text()
    assert "treewidth" in content
    assert "branch:auto" in content


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
