#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import sys
from typing import Iterable, TextIO

from render_scoreboard import format_ns, read_jsonl, tier_path
from summarize_qasm_report import markdown_escape


RANKWIDTH_KERNEL_ELAPSED_FIELDS = (
    "rankwidth_join_map_elapsed_ns",
    "rankwidth_join_elapsed_ns",
    "rankwidth_fourier_join_map_elapsed_ns",
    "rankwidth_fourier_join_elapsed_ns",
)


def is_rankwidth_backend(record: dict) -> bool:
    backend = str(record.get("backend") or "")
    return backend == "rankwidth" or backend.startswith("rankwidth:")


def record_key(record: dict) -> tuple[str, str, str, str, str, str]:
    return (
        str(record.get("source") or "unknown"),
        str(record.get("source_relative_path") or ""),
        str(record.get("case") or ""),
        str(record.get("input") or ""),
        str(record.get("output") or ""),
        str(record.get("qsop_mode") or "unknown"),
    )


def rankwidth_config(record: dict) -> str:
    config = record.get("backend_config")
    config = config if isinstance(config, dict) else {}
    generator = (
        record.get("rankwidth_decomposition")
        or config.get("rankwidth_decomposition")
        or config.get("rankwidth_generate")
        or "left-deep"
    )
    mode = record.get("rankwidth_mode") or config.get("rankwidth_mode") or "count-table"
    return f"{generator}:{mode}"


def status(record: dict) -> str:
    return str(record.get("status") or "ok")


def metric(record: dict, key: str) -> int | None:
    value = record.get(key)
    if isinstance(value, int):
        return value
    stats = record.get("stats", {})
    value = stats.get(key) if isinstance(stats, dict) else None
    return value if isinstance(value, int) else None


def metric_or_zero(record: dict, key: str) -> int:
    value = metric(record, key)
    return value if value is not None else 0


def rankwidth_kernel_elapsed_ns(record: dict) -> int:
    return sum(metric_or_zero(record, key) for key in RANKWIDTH_KERNEL_ELAPSED_FIELDS)


def load_records(named_paths: Iterable[tuple[str, pathlib.Path]]) -> list[dict]:
    rows: list[dict] = []
    for tier, path in named_paths:
        for record in read_jsonl(path):
            if not is_rankwidth_backend(record):
                continue
            copied = dict(record)
            copied["_tier"] = tier
            rows.append(copied)
    return rows


def config_summary(records: list[dict]) -> list[dict]:
    grouped: dict[tuple[str, str, str], dict] = {}
    for record in records:
        key = (str(record["_tier"]), str(record.get("qsop_mode") or "unknown"), rankwidth_config(record))
        entry = grouped.setdefault(
            key,
            {
                "tier": key[0],
                "mode": key[1],
                "config": key[2],
                "records": 0,
                "ok": 0,
                "timeouts": 0,
                "memouts": 0,
                "elapsed_ns": 0,
                "kernel_elapsed_ns": 0,
                "max_width": 0,
                "max_table": 0,
                "max_signatures": 0,
                "join_pairs": 0,
                "join_signature_pairs": 0,
                "table_pressure": 0,
                "table_forecast_pressure": 0,
                "max_table_forecast": 0,
                "signature_pressure": 0,
                "join_pair_pressure": 0,
                "join_pair_forecast_pressure": 0,
                "max_join_pair_forecast": 0,
                "join_signature_pressure": 0,
            },
        )
        entry["records"] += 1
        if status(record) == "ok":
            entry["ok"] += 1
            entry["elapsed_ns"] += metric_or_zero(record, "solve_elapsed_ns")
            entry["kernel_elapsed_ns"] += rankwidth_kernel_elapsed_ns(record)
            entry["max_width"] = max(entry["max_width"], metric_or_zero(record, "rankwidth_width"))
            table = metric_or_zero(record, "rankwidth_max_table_entries")
            table_forecast = metric_or_zero(record, "rankwidth_table_forecast")
            signatures = metric_or_zero(record, "rankwidth_max_signature_entries")
            join_pair_forecast = metric_or_zero(record, "rankwidth_join_pair_forecast")
            join_pairs = metric_or_zero(record, "join_pairs")
            join_signature_pairs = metric_or_zero(record, "join_signature_pairs")
            entry["max_table"] = max(entry["max_table"], table)
            entry["max_table_forecast"] = max(entry["max_table_forecast"], table_forecast)
            entry["max_signatures"] = max(
                entry["max_signatures"], signatures
            )
            entry["max_join_pair_forecast"] = max(
                entry["max_join_pair_forecast"], join_pair_forecast
            )
            entry["join_pairs"] += join_pairs
            entry["join_signature_pairs"] += join_signature_pairs
            entry["table_pressure"] += table
            entry["table_forecast_pressure"] += table_forecast
            entry["signature_pressure"] += signatures
            entry["join_pair_pressure"] += join_pairs
            entry["join_pair_forecast_pressure"] += join_pair_forecast
            entry["join_signature_pressure"] += join_signature_pairs
        elif status(record) == "timeout":
            entry["timeouts"] += 1
        elif status(record) == "memout":
            entry["memouts"] += 1
    return [grouped[key] for key in sorted(grouped)]


def common_row_summary(records: list[dict], baseline: str) -> list[dict]:
    grouped: dict[tuple[str, str, tuple[str, str, str, str, str, str]], dict[str, dict]] = {}
    for record in records:
        if status(record) != "ok":
            continue
        key = (str(record["_tier"]), str(record.get("qsop_mode") or "unknown"), record_key(record))
        grouped.setdefault(key, {})[rankwidth_config(record)] = record

    summary: dict[tuple[str, str], dict] = {}
    for (tier, mode, _identity), by_config in grouped.items():
        if len(by_config) < 2:
            continue
        entry = summary.setdefault(
            (tier, mode),
            {
                "tier": tier,
                "mode": mode,
                "common_rows": 0,
                "time_wins": collections.Counter(),
                "table_wins": collections.Counter(),
                "forecast_wins": collections.Counter(),
                "signature_wins": collections.Counter(),
                "kernel_wins": collections.Counter(),
                "baseline_rows": 0,
                "baseline_best_time": 0,
                "baseline_best_table": 0,
                "baseline_best_forecast": 0,
                "baseline_best_signatures": 0,
                "baseline_best_kernel": 0,
            },
        )
        entry["common_rows"] += 1
        fastest_configs = tied_winners(
            by_config,
            lambda record: (metric_or_zero(record, "solve_elapsed_ns"),),
        )
        table_configs = tied_winners(
            by_config,
            lambda record: (
                metric_or_zero(record, "rankwidth_max_table_entries"),
                metric_or_zero(record, "join_pairs"),
            ),
        )
        forecast_configs = tied_winners(
            by_config,
            lambda record: (
                metric_or_zero(record, "rankwidth_table_forecast"),
                metric_or_zero(record, "rankwidth_join_pair_forecast"),
                metric_or_zero(record, "rankwidth_max_table_entries"),
                metric_or_zero(record, "join_pairs"),
            ),
        )
        signature_configs = tied_winners(
            by_config,
            lambda record: (
                metric_or_zero(record, "rankwidth_max_signature_entries"),
                metric_or_zero(record, "join_signature_pairs"),
            ),
        )
        kernel_configs = tied_winners(
            by_config,
            lambda record: (rankwidth_kernel_elapsed_ns(record),),
        )
        for config in fastest_configs:
            entry["time_wins"][config] += 1
        for config in table_configs:
            entry["table_wins"][config] += 1
        for config in forecast_configs:
            entry["forecast_wins"][config] += 1
        for config in signature_configs:
            entry["signature_wins"][config] += 1
        for config in kernel_configs:
            entry["kernel_wins"][config] += 1
        if baseline in by_config:
            entry["baseline_rows"] += 1
            entry["baseline_best_time"] += int(baseline in fastest_configs)
            entry["baseline_best_table"] += int(baseline in table_configs)
            entry["baseline_best_forecast"] += int(baseline in forecast_configs)
            entry["baseline_best_signatures"] += int(baseline in signature_configs)
            entry["baseline_best_kernel"] += int(baseline in kernel_configs)
    return [summary[key] for key in sorted(summary)]


def common_pressure_summary(records: list[dict], baseline: str) -> list[dict]:
    grouped: dict[tuple[str, str, tuple[str, str, str, str, str, str]], dict[str, dict]] = {}
    for record in records:
        if status(record) != "ok":
            continue
        key = (str(record["_tier"]), str(record.get("qsop_mode") or "unknown"), record_key(record))
        grouped.setdefault(key, {})[rankwidth_config(record)] = record

    summary: dict[tuple[str, str, str], dict] = {}
    for (tier, mode, _identity), by_config in grouped.items():
        if len(by_config) < 2:
            continue
        baseline_record = by_config.get(baseline)
        for config, record in by_config.items():
            key = (tier, mode, config)
            entry = summary.setdefault(
                key,
                {
                    "tier": tier,
                    "mode": mode,
                    "config": config,
                    "common_rows": 0,
                    "elapsed_ns": 0,
                    "kernel_elapsed_ns": 0,
                    "table_pressure": 0,
                    "table_forecast_pressure": 0,
                    "signature_pressure": 0,
                    "join_pair_pressure": 0,
                    "join_pair_forecast_pressure": 0,
                    "join_signature_pressure": 0,
                    "baseline_common_rows": 0,
                    "elapsed_delta_vs_baseline": 0,
                    "kernel_delta_vs_baseline": 0,
                    "table_delta_vs_baseline": 0,
                    "table_forecast_delta_vs_baseline": 0,
                    "signature_delta_vs_baseline": 0,
                    "join_pair_forecast_delta_vs_baseline": 0,
                },
            )
            table = metric_or_zero(record, "rankwidth_max_table_entries")
            table_forecast = metric_or_zero(record, "rankwidth_table_forecast")
            signatures = metric_or_zero(record, "rankwidth_max_signature_entries")
            join_pair_forecast = metric_or_zero(record, "rankwidth_join_pair_forecast")
            elapsed = metric_or_zero(record, "solve_elapsed_ns")
            kernel_elapsed = rankwidth_kernel_elapsed_ns(record)
            entry["common_rows"] += 1
            entry["elapsed_ns"] += elapsed
            entry["kernel_elapsed_ns"] += kernel_elapsed
            entry["table_pressure"] += table
            entry["table_forecast_pressure"] += table_forecast
            entry["signature_pressure"] += signatures
            entry["join_pair_pressure"] += metric_or_zero(record, "join_pairs")
            entry["join_pair_forecast_pressure"] += join_pair_forecast
            entry["join_signature_pressure"] += metric_or_zero(record, "join_signature_pairs")
            if baseline_record is not None:
                entry["baseline_common_rows"] += 1
                entry["elapsed_delta_vs_baseline"] += elapsed - metric_or_zero(
                    baseline_record, "solve_elapsed_ns"
                )
                entry["kernel_delta_vs_baseline"] += kernel_elapsed - rankwidth_kernel_elapsed_ns(
                    baseline_record
                )
                entry["table_delta_vs_baseline"] += table - metric_or_zero(
                    baseline_record, "rankwidth_max_table_entries"
                )
                entry["table_forecast_delta_vs_baseline"] += table_forecast - metric_or_zero(
                    baseline_record, "rankwidth_table_forecast"
                )
                entry["signature_delta_vs_baseline"] += signatures - metric_or_zero(
                    baseline_record, "rankwidth_max_signature_entries"
                )
                entry["join_pair_forecast_delta_vs_baseline"] += (
                    join_pair_forecast
                    - metric_or_zero(baseline_record, "rankwidth_join_pair_forecast")
                )
    return [summary[key] for key in sorted(summary)]


def tied_winners(by_config: dict[str, dict], key_func) -> list[str]:
    best_key = min(key_func(record) for record in by_config.values())
    return sorted(config for config, record in by_config.items() if key_func(record) == best_key)


def counter_text(counter: collections.Counter) -> str:
    return "; ".join(f"{markdown_escape(key)} {value}" for key, value in counter.most_common())


def format_signed_ns(ns: int) -> str:
    if ns < 0:
        return f"-{format_ns(-ns)}"
    return format_ns(ns)


def write_markdown(records: list[dict], baseline: str, file: TextIO) -> None:
    print("# Rankwidth Generator Comparison\n", file=file)
    print(
        "Rows compare rankwidth generator artifacts on common source/case/boundary/QSOP-mode keys. "
        "Table winners minimize max table entries and join pairs; signature winners minimize max "
        "signature entries and join-signature pairs. Ties are counted for every tied config.",
        file=file,
    )
    print("\n## Config Summary\n", file=file)
    print(
        "| Tier | QSOP mode | Config | OK / records | Total solve time | Kernel time | Max width | "
        "Max table | Forecast table | Max signatures | Join pairs | Forecast join pairs | "
        "Join signature pairs | Mean table | Mean forecast table | Mean signatures |",
        file=file,
    )
    print(
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        file=file,
    )
    for row in config_summary(records):
        mean_table = row["table_pressure"] / row["ok"] if row["ok"] else 0.0
        mean_table_forecast = row["table_forecast_pressure"] / row["ok"] if row["ok"] else 0.0
        mean_signatures = row["signature_pressure"] / row["ok"] if row["ok"] else 0.0
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
            f"`{markdown_escape(row['config'])}` | {row['ok']} / {row['records']} | "
            f"{format_ns(row['elapsed_ns'])} | {format_ns(row['kernel_elapsed_ns'])} | "
            f"{row['max_width']} | {row['max_table']} | {row['max_table_forecast']} | "
            f"{row['max_signatures']} | {row['join_pairs']} | "
            f"{row['max_join_pair_forecast']} | {row['join_signature_pairs']} | "
            f"{mean_table:.1f} | {mean_table_forecast:.1f} | {mean_signatures:.1f} |",
            file=file,
        )

    rows = common_row_summary(records, baseline)
    if not rows:
        return
    print("\n## Common-Row Winners\n", file=file)
    print(
        f"Baseline `{markdown_escape(baseline)}` is counted as best whenever it ties for the metric.\n",
        file=file,
    )
    print(
        "| Tier | QSOP mode | Common rows | Time winners | Table winners | Forecast winners | "
        "Signature winners | Kernel winners | Baseline best time/table/forecast/signatures/kernel |",
        file=file,
    )
    print("| --- | --- | ---: | --- | --- | --- | --- | --- | ---: |", file=file)
    for row in rows:
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
            f"{row['common_rows']} | {counter_text(row['time_wins'])} | "
            f"{counter_text(row['table_wins'])} | {counter_text(row['forecast_wins'])} | "
            f"{counter_text(row['signature_wins'])} | {counter_text(row['kernel_wins'])} | "
            f"{row['baseline_best_time']} / {row['baseline_best_table']} / "
            f"{row['baseline_best_forecast']} / {row['baseline_best_signatures']} / "
            f"{row['baseline_best_kernel']} "
            f"of {row['baseline_rows']} |",
            file=file,
        )

    pressure_rows = common_pressure_summary(records, baseline)
    if not pressure_rows:
        return
    print("\n## Common-Row Pressure\n", file=file)
    print(
        "Pressure totals are restricted to rows solved by at least two rankwidth configs. "
        "Deltas are relative to the baseline on common rows where the baseline also solved.",
        file=file,
    )
    print(
        "| Tier | QSOP mode | Config | Common rows | Total solve time | Table pressure | "
        "Forecast table | Signature pressure | Join pairs | Forecast join pairs | "
        "Join signature pairs | Kernel time | Baseline rows | "
        "Delta table/forecast table/signature/forecast joins/time/kernel |",
        file=file,
    )
    print(
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        file=file,
    )
    for row in pressure_rows:
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
            f"`{markdown_escape(row['config'])}` | {row['common_rows']} | "
            f"{format_ns(row['elapsed_ns'])} | {row['table_pressure']} | "
            f"{row['table_forecast_pressure']} | {row['signature_pressure']} | "
            f"{row['join_pair_pressure']} | {row['join_pair_forecast_pressure']} | "
            f"{row['join_signature_pressure']} | {format_ns(row['kernel_elapsed_ns'])} | "
            f"{row['baseline_common_rows']} | "
            f"{row['table_delta_vs_baseline']} / "
            f"{row['table_forecast_delta_vs_baseline']} / "
            f"{row['signature_delta_vs_baseline']} / "
            f"{row['join_pair_forecast_delta_vs_baseline']} / "
            f"{format_signed_ns(row['elapsed_delta_vs_baseline'])} / "
            f"{format_signed_ns(row['kernel_delta_vs_baseline'])} |",
            file=file,
        )


def serializable_row(row: dict) -> dict:
    out = dict(row)
    for key in ("time_wins", "table_wins", "forecast_wins", "signature_wins", "kernel_wins"):
        if key in out:
            out[key] = dict(out[key])
    return out


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare rankwidth generator benchmark JSONL artifacts.")
    parser.add_argument("--rankwidth-jsonl", action="append", type=tier_path, default=[], metavar="TIER=PATH")
    parser.add_argument("--qsop-mode", choices=("all", "sign"), default="all")
    parser.add_argument("--baseline", default="min-fill-cut:count-table")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        records = load_records(args.rankwidth_jsonl)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.qsop_mode != "all":
        records = [record for record in records if record.get("qsop_mode") == args.qsop_mode]
    if args.format == "json":
        payload = {
            "config_summary": config_summary(records),
            "common_row_summary": [serializable_row(row) for row in common_row_summary(records, args.baseline)],
            "common_pressure_summary": common_pressure_summary(records, args.baseline),
        }
        print(json.dumps(payload, sort_keys=True, indent=2))
    else:
        write_markdown(records, args.baseline, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
