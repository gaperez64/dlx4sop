#!/usr/bin/env python3
"""Tests for tools/tune_branch_thresholds.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile


def load_tool(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("tune_branch_thresholds", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_calib_record(*, rw_lw: int | None, tw_width: int | None,
                       rw_ms: float, tw_ms: float) -> str:
    rec = {
        "schema": "sop_solve_backend_stats_v1",
        "n_active_vars": 40,
        "n_active_terms": 60,
        "modulus_r": 8,
        "backend_chosen": "rankwidth" if rw_ms < tw_ms else "treewidth",
        "veto_reason": None,
        "treewidth_probe_ms": 1.0,
        "treewidth_width": tw_width,
        "treewidth_forecast_entries": 512,
        "treewidth_forecast_join_pairs": 256,
        "treewidth_actual_ms": tw_ms,
        "rankwidth_generation_ms": 0.5,
        "rankwidth_support_width": rw_lw,
        "rankwidth_labelled_width": rw_lw,
        "rankwidth_forecast_entries": 128,
        "rankwidth_forecast_join_pairs": 64,
        "rankwidth_actual_ms": rw_ms,
    }
    return json.dumps(rec)


def write_jsonl(lines: list[str]) -> str:
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        return f.name


def test_load_calibration_records(tool) -> None:
    lines = [
        make_calib_record(rw_lw=3, tw_width=10, rw_ms=1.0, tw_ms=5.0),
        make_calib_record(rw_lw=3, tw_width=10, rw_ms=2.0, tw_ms=4.0),
        make_calib_record(rw_lw=6, tw_width=12, rw_ms=8.0, tw_ms=3.0),
        # Record without rw_actual_ms should be filtered out
        json.dumps({"schema": "sop_solve_backend_stats_v1", "treewidth_actual_ms": 2.0}),
    ]
    path = pathlib.Path(write_jsonl(lines))
    records = tool.load_calibration_records([path])
    assert len(records) == 3, f"expected 3 calibration records, got {len(records)}"


def test_analyse_by_width(tool) -> None:
    lines = [
        # rw_lw=3 wins twice (rw_ms < tw_ms)
        make_calib_record(rw_lw=3, tw_width=10, rw_ms=1.0, tw_ms=5.0),
        make_calib_record(rw_lw=3, tw_width=10, rw_ms=2.0, tw_ms=4.0),
        # rw_lw=6 loses once
        make_calib_record(rw_lw=6, tw_width=12, rw_ms=8.0, tw_ms=3.0),
    ]
    path = pathlib.Path(write_jsonl(lines))
    records = tool.load_calibration_records([path])
    rw_buckets = tool.analyse_by_width(
        records,
        width_field="rankwidth_labelled_width",
        speed_field="rankwidth_actual_ms",
        baseline_field="treewidth_actual_ms",
    )
    assert 3 in rw_buckets
    assert rw_buckets[3]["total"] == 2
    assert rw_buckets[3]["wins"] == 2
    assert 6 in rw_buckets
    assert rw_buckets[6]["total"] == 1
    assert rw_buckets[6]["wins"] == 0


def test_threshold_recommendation(tool) -> None:
    buckets = {
        3: {"total": 10, "wins": 9, "speed_ms": 1.0, "baseline_ms": 5.0},   # 90% win
        4: {"total": 10, "wins": 7, "speed_ms": 2.0, "baseline_ms": 4.0},   # 70% win
        5: {"total": 10, "wins": 4, "speed_ms": 4.0, "baseline_ms": 3.0},   # 40% win
    }
    rec = tool.threshold_recommendation(buckets, win_rate_threshold=0.6)
    assert rec == 4, f"expected max width 4, got {rec}"

    rec_none = tool.threshold_recommendation({5: {"total": 10, "wins": 2, "speed_ms": 1.0, "baseline_ms": 0.5}},
                                             win_rate_threshold=0.6)
    assert rec_none is None


def test_json_output(tool) -> None:
    lines = [
        make_calib_record(rw_lw=3, tw_width=10, rw_ms=1.0, tw_ms=5.0),
        make_calib_record(rw_lw=3, tw_width=11, rw_ms=1.5, tw_ms=3.0),
        make_calib_record(rw_lw=5, tw_width=10, rw_ms=9.0, tw_ms=2.0),
    ]
    path = write_jsonl(lines)
    result = tool.main(["--format", "json", path])
    assert result == 0


def test_empty_input_returns_error(tool) -> None:
    lines = [json.dumps({"schema": "sop_solve_backend_stats_v1", "treewidth_actual_ms": 1.0})]
    path = write_jsonl(lines)
    result = tool.main(["--format", "json", path])
    assert result != 0, "expected non-zero return for no calibration records"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(f"usage: {argv[0]} <tune_branch_thresholds.py>", file=sys.stderr)
        return 2
    tool = load_tool(pathlib.Path(argv[1]))

    test_load_calibration_records(tool)
    test_analyse_by_width(tool)
    test_threshold_recommendation(tool)
    test_json_output(tool)
    test_empty_input_returns_error(tool)

    print("all tune_branch_thresholds tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
