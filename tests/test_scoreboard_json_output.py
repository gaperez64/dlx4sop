#!/usr/bin/env python3
"""Tests for _write_scoreboard_json in tools/refresh_scoreboard.py (C1)."""

import importlib.util
import json
import pathlib
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    if str(TOOLS_DIR) not in sys.path:
        sys.path.insert(0, str(TOOLS_DIR))
    path = TOOLS_DIR / "refresh_scoreboard.py"
    spec = importlib.util.spec_from_file_location("refresh_scoreboard", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["refresh_scoreboard"] = module
    spec.loader.exec_module(module)
    return module


def _make_solver_record(backend: str, elapsed_ns: int, source: str = "Synthetic",
                         status: str = "ok") -> dict:
    return {
        "backend": backend,
        "treewidth_order": "min-fill",
        "source": source,
        "source_url": "https://example.invalid/synthetic",
        "source_relative_path": "bell.qasm",
        "case": "bell",
        "input": "00",
        "output": "00",
        "solve_elapsed_ns": elapsed_ns,
        "elapsed_ns": elapsed_ns,
        "amplitude_real": 1.0,
        "amplitude_imag": 0.0,
        "status": status,
        "stats": {
            "decomposition_width": 2,
            "max_table_entries": 16,
            "join_pairs": 32,
            "search_nodes": 4,
        },
        "treewidth_width": 2,
        "treewidth_max_table_entries": 16,
        "rankwidth_table_forecast": 256,
        "rankwidth_join_pair_forecast": 96,
    }


def test_write_scoreboard_json_creates_file(tmp_path):
    tool = _load_tool()

    tier_label = "0-32"
    records = [
        _make_solver_record("treewidth", 1_000_000),
        _make_solver_record("branch", 2_000_000),
    ]
    solver_records = [(tier_label, records)]
    native_records = []

    out_path = tmp_path / "scoreboard.json"
    tool._write_scoreboard_json(solver_records, native_records, out_path)

    assert out_path.exists()
    data = json.loads(out_path.read_text(encoding="utf-8"))
    assert "generated_at" in data
    assert "tiers" in data
    assert "solver_summary" in data
    assert isinstance(data["solver_summary"], list)


def test_write_scoreboard_json_multi_tier(tmp_path):
    tool = _load_tool()

    solver_records = [
        ("0-32", [_make_solver_record("treewidth", 500_000)]),
        ("33-64", [
            _make_solver_record("treewidth", 5_000_000),
            _make_solver_record("branch", 7_000_000),
        ]),
    ]
    native_records = []

    out_path = tmp_path / "sb.json"
    tool._write_scoreboard_json(solver_records, native_records, out_path)

    data = json.loads(out_path.read_text(encoding="utf-8"))
    tiers = {e["tier"] for e in data["solver_summary"]}
    assert "0-32" in tiers
    assert "33-64" in tiers


def test_write_scoreboard_json_empty(tmp_path):
    tool = _load_tool()

    out_path = tmp_path / "empty.json"
    tool._write_scoreboard_json([], [], out_path)

    assert out_path.exists()
    data = json.loads(out_path.read_text(encoding="utf-8"))
    assert data["solver_summary"] == []


def test_write_scoreboard_json_creates_parent_dir(tmp_path):
    tool = _load_tool()

    out_path = tmp_path / "subdir" / "deeper" / "scoreboard.json"
    tool._write_scoreboard_json([], [], out_path)

    assert out_path.exists()


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
