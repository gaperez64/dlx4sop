#!/usr/bin/env python3
"""Smoke tests for _write_scoreboard_json in scripts/refresh_scoreboard.py (C1)."""

import importlib.util
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "scripts"


def _load_tool():
    if str(TOOLS_DIR) not in sys.path:
        sys.path.insert(0, str(TOOLS_DIR))
    path = TOOLS_DIR / "refresh_scoreboard.py"
    spec = importlib.util.spec_from_file_location("refresh_scoreboard", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["refresh_scoreboard"] = module
    spec.loader.exec_module(module)
    return module


def _make_solver_record(backend, elapsed_ns, source="Synthetic", status="ok"):
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


def test_write_scoreboard_json_creates_file(tool, tmp):
    solver_records = [
        ("0-32", [
            _make_solver_record("treewidth", 1_000_000),
            _make_solver_record("branch", 2_000_000),
        ])
    ]
    out_path = tmp / "scoreboard.json"
    tool._write_scoreboard_json(solver_records, [], out_path)
    if not out_path.exists():
        raise AssertionError("scoreboard.json not created")
    data = json.loads(out_path.read_text(encoding="utf-8"))
    for key in ("generated_at", "tiers", "solver_summary"):
        if key not in data:
            raise AssertionError(f"missing key {key!r} in output")
    if not isinstance(data["solver_summary"], list):
        raise AssertionError("solver_summary should be a list")


def test_write_scoreboard_json_multi_tier(tool, tmp):
    solver_records = [
        ("0-32", [_make_solver_record("treewidth", 500_000)]),
        ("33-64", [
            _make_solver_record("treewidth", 5_000_000),
            _make_solver_record("branch", 7_000_000),
        ]),
    ]
    out_path = tmp / "sb.json"
    tool._write_scoreboard_json(solver_records, [], out_path)
    data = json.loads(out_path.read_text(encoding="utf-8"))
    tiers = {e["tier"] for e in data["solver_summary"]}
    if "0-32" not in tiers:
        raise AssertionError("missing tier 0-32 in output")
    if "33-64" not in tiers:
        raise AssertionError("missing tier 33-64 in output")


def test_write_scoreboard_json_empty(tool, tmp):
    out_path = tmp / "empty.json"
    tool._write_scoreboard_json([], [], out_path)
    if not out_path.exists():
        raise AssertionError("json not created for empty input")
    data = json.loads(out_path.read_text(encoding="utf-8"))
    if data["solver_summary"] != []:
        raise AssertionError(f"expected empty solver_summary, got {data['solver_summary']}")


def test_write_scoreboard_json_creates_parent_dir(tool, tmp):
    out_path = tmp / "subdir" / "deeper" / "scoreboard.json"
    tool._write_scoreboard_json([], [], out_path)
    if not out_path.exists():
        raise AssertionError("json not created with nested parent dirs")


def main() -> int:
    tool = _load_tool()
    tests = [
        ("write_scoreboard_json_creates_file", test_write_scoreboard_json_creates_file),
        ("write_scoreboard_json_multi_tier", test_write_scoreboard_json_multi_tier),
        ("write_scoreboard_json_empty", test_write_scoreboard_json_empty),
        ("write_scoreboard_json_creates_parent_dir", test_write_scoreboard_json_creates_parent_dir),
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
