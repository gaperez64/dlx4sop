#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def rankwidth_record(
    generator: str,
    elapsed_ns: int,
    max_table: int,
    max_signatures: int,
    table_forecast: int,
    join_pair_forecast: int,
) -> dict:
    return {
        "backend": "rankwidth",
        "rankwidth_decomposition": generator,
        "rankwidth_mode": "count-table",
        "source": "Synthetic",
        "source_relative_path": "labelled.qasm",
        "case": "labelled",
        "input": "0",
        "output": "0",
        "qsop_mode": "labelled",
        "status": "ok",
        "solve_elapsed_ns": elapsed_ns,
        "rankwidth_width": 3,
        "rankwidth_max_table_entries": max_table,
        "rankwidth_table_forecast": table_forecast,
        "rankwidth_join_pair_forecast": join_pair_forecast,
        "rankwidth_max_signature_entries": max_signatures,
        "rankwidth_join_map_elapsed_ns": max_table,
        "rankwidth_join_elapsed_ns": elapsed_ns // 2,
        "stats": {
            "decomposition_width": 3,
            "max_table_entries": max_table,
            "max_signature_entries": max_signatures,
            "join_pairs": 100 + max_table,
            "join_signature_pairs": 10 + max_signatures,
        },
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_compare_rankwidth_generators.py COMPARE_TOOL", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    records = [
        rankwidth_record("min-fill-cut", 200, 64, 16, 1000, 2000),
        rankwidth_record("balanced", 300, 32, 8, 800, 1200),
        rankwidth_record("left-deep", 100, 128, 32, 1200, 2500),
    ]

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        jsonl_path = tmp_path / "rankwidth.jsonl"
        jsonl_path.write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        completed = subprocess.run(
            [str(tool), "--rankwidth-jsonl", f"33-64={jsonl_path}", "--qsop-mode", "labelled"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"rankwidth comparison failed:\n{completed.stdout}\n{completed.stderr}")
        for expected in (
            "# Rankwidth Generator Comparison",
            "`min-fill-cut:count-table`",
            "`balanced:count-table`",
            "Mean table",
            "Mean forecast table",
            "left-deep:count-table 1",
            "balanced:count-table 1",
            "min-fill-cut:count-table 1",
            "0 / 0 / 0 / 1 of 1",
            "## Common-Row Pressure",
            "`balanced:count-table` | 1 | 300 ns | 32 | 800 | 8 | 132 | 1200 | 18 | 182 ns | 1 | -32 / -200 / -8 / -800 / 100 ns / 18 ns",
            "`left-deep:count-table` | 1 | 100 ns | 128 | 1200 | 32 | 228 | 2500 | 42 | 178 ns | 1 | 64 / 200 / 16 / 500 / -100 ns / 14 ns",
        ):
            if expected not in completed.stdout:
                raise AssertionError(f"missing {expected!r} in:\n{completed.stdout}")

        completed = subprocess.run(
            [
                str(tool),
                "--rankwidth-jsonl",
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
            raise AssertionError(f"rankwidth json comparison failed:\n{completed.stdout}\n{completed.stderr}")
        payload = json.loads(completed.stdout)
        common = payload["common_row_summary"][0]
        if common["time_wins"] != {"left-deep:count-table": 1}:
            raise AssertionError(f"unexpected time winners: {common}")
        if common["table_wins"] != {"balanced:count-table": 1}:
            raise AssertionError(f"unexpected table winners: {common}")
        if common["kernel_wins"] != {"min-fill-cut:count-table": 1}:
            raise AssertionError(f"unexpected kernel winners: {common}")
        config_rows = {row["config"]: row for row in payload["config_summary"]}
        if config_rows["left-deep:count-table"]["table_pressure"] != 128:
            raise AssertionError(f"unexpected left-deep pressure: {config_rows['left-deep:count-table']}")
        if config_rows["left-deep:count-table"]["table_forecast_pressure"] != 1200:
            raise AssertionError(
                f"unexpected left-deep forecast pressure: {config_rows['left-deep:count-table']}"
            )
        if config_rows["balanced:count-table"]["join_pair_forecast_pressure"] != 1200:
            raise AssertionError(
                f"unexpected balanced join forecast pressure: {config_rows['balanced:count-table']}"
            )
        if config_rows["balanced:count-table"]["signature_pressure"] != 8:
            raise AssertionError(f"unexpected balanced pressure: {config_rows['balanced:count-table']}")
        if config_rows["min-fill-cut:count-table"]["kernel_elapsed_ns"] != 164:
            raise AssertionError(f"unexpected baseline kernel time: {config_rows['min-fill-cut:count-table']}")
        pressure_rows = {row["config"]: row for row in payload["common_pressure_summary"]}
        if pressure_rows["balanced:count-table"]["table_delta_vs_baseline"] != -32:
            raise AssertionError(f"unexpected balanced common pressure: {pressure_rows['balanced:count-table']}")
        if pressure_rows["balanced:count-table"]["table_forecast_delta_vs_baseline"] != -200:
            raise AssertionError(
                f"unexpected balanced forecast pressure: {pressure_rows['balanced:count-table']}"
            )
        if pressure_rows["left-deep:count-table"]["join_pair_forecast_delta_vs_baseline"] != 500:
            raise AssertionError(
                f"unexpected left-deep join forecast pressure: {pressure_rows['left-deep:count-table']}"
            )
        if pressure_rows["left-deep:count-table"]["elapsed_delta_vs_baseline"] != -100:
            raise AssertionError(f"unexpected left-deep common pressure: {pressure_rows['left-deep:count-table']}")
        if pressure_rows["balanced:count-table"]["kernel_delta_vs_baseline"] != 18:
            raise AssertionError(f"unexpected balanced kernel delta: {pressure_rows['balanced:count-table']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
