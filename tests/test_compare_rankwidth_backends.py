#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def base_record(backend: str, case: str, elapsed_ns: int) -> dict:
    return {
        "backend": backend,
        "source": "Synthetic",
        "source_relative_path": "labelled.qasm",
        "case": case,
        "input": "0",
        "output": "0",
        "qsop_mode": "labelled",
        "status": "ok",
        "solve_elapsed_ns": elapsed_ns,
    }


def rankwidth_record(case: str, elapsed_ns: int, max_table: int, table_forecast: int) -> dict:
    record = base_record("rankwidth", case, elapsed_ns)
    record.update(
        {
            "rankwidth_decomposition": "min-fill-cut",
            "rankwidth_mode": "count-table",
            "rankwidth_width": 2,
            "rankwidth_max_table_entries": max_table,
            "rankwidth_table_forecast": table_forecast,
            "rankwidth_join_pair_forecast": 80,
        }
    )
    return record


def treewidth_record(case: str, elapsed_ns: int, max_table: int) -> dict:
    record = base_record("treewidth", case, elapsed_ns)
    record.update(
        {
            "treewidth_order": "min-fill-max-degree",
            "decomposition_width": 3,
            "treewidth_max_table_entries": max_table,
        }
    )
    return record


def branch_record(case: str, elapsed_ns: int) -> dict:
    record = base_record("branch", case, elapsed_ns)
    record["branch_heuristic"] = "split"
    return record


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_compare_rankwidth_backends.py COMPARE_TOOL", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    records = [
        treewidth_record("rankwidth-win", 300, 64),
        branch_record("rankwidth-win", 250),
        rankwidth_record("rankwidth-win", 200, 32, 40),
        treewidth_record("rankwidth-loss", 100, 16),
        branch_record("rankwidth-loss", 120),
        rankwidth_record("rankwidth-loss", 150, 32, 20),
        treewidth_record("rankwidth-missing", 100, 8),
        branch_record("rankwidth-missing", 110),
    ]

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        jsonl_path = tmp_path / "comparison.jsonl"
        jsonl_path.write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        completed = subprocess.run(
            [
                str(tool),
                "--comparison-jsonl",
                f"33-64={jsonl_path}",
                "--qsop-mode",
                "labelled",
                "--top",
                "3",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"rankwidth backend comparison failed:\n{completed.stdout}\n{completed.stderr}")
        for expected in (
            "# Rankwidth Backend Comparison",
            "`rankwidth:min-fill-cut:count-table` | 2 / 2 | 350 ns | 2 | 32 | 60 | 160",
            "| 33-64 | labelled | 2 | 1 | 1 | 350 ns | 350 ns | 0 ns | 1 / 2 | 1 / 0 / 1 | 1 / 0 / 1 | 1 / 2 | -50 ns / -20 ns | 1 |",
            "Synthetic:rankwidth-win 0->0",
            "`branch:split` 250 ns",
        ):
            if expected not in completed.stdout:
                raise AssertionError(f"missing {expected!r} in:\n{completed.stdout}")

        completed = subprocess.run(
            [
                str(tool),
                "--comparison-jsonl",
                f"33-64={jsonl_path}",
                "--qsop-mode",
                "labelled",
                "--format",
                "json",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"rankwidth backend comparison json failed:\n{completed.stdout}\n{completed.stderr}")
        payload = json.loads(completed.stdout)
        summary = payload["comparison_summary"][0]
        if summary["rankwidth_fastest_or_tied"] != 1 or summary["rankwidth_slower"] != 1:
            raise AssertionError(f"unexpected win/loss summary: {summary}")
        if summary["table_comparison"] != {"higher": 1, "lower": 1}:
            raise AssertionError(f"unexpected table comparison: {summary}")
        if summary["forecast_comparison"] != {"higher": 1, "lower": 1}:
            raise AssertionError(f"unexpected forecast comparison: {summary}")
        if summary["rankwidth_missing_or_failed"] != 1:
            raise AssertionError(f"unexpected missing count: {summary}")
        wins = payload["top_rankwidth_wins"]
        if len(wins) != 1 or wins[0]["case"] != "rankwidth-win" or wins[0]["elapsed_win_ns"] != 50:
            raise AssertionError(f"unexpected top wins: {wins}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
