#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import sys
from typing import Iterable, TextIO

from render_scoreboard import format_ns, labelled_path, read_jsonl
from summarize_qasm_report import markdown_escape


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
    generator = record.get("rankwidth_decomposition") or "left-deep"
    mode = record.get("rankwidth_mode") or "count-table"
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


def load_records(named_paths: Iterable[tuple[str, pathlib.Path]]) -> list[dict]:
    rows: list[dict] = []
    for tier, path in named_paths:
        for record in read_jsonl(path):
            if record.get("backend") != "rankwidth":
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
                "elapsed_ns": 0,
                "max_width": 0,
                "max_table": 0,
                "max_signatures": 0,
                "join_pairs": 0,
                "join_signature_pairs": 0,
            },
        )
        entry["records"] += 1
        if status(record) == "ok":
            entry["ok"] += 1
            entry["elapsed_ns"] += metric_or_zero(record, "solve_elapsed_ns")
            entry["max_width"] = max(entry["max_width"], metric_or_zero(record, "rankwidth_width"))
            entry["max_table"] = max(entry["max_table"], metric_or_zero(record, "rankwidth_max_table_entries"))
            entry["max_signatures"] = max(
                entry["max_signatures"], metric_or_zero(record, "rankwidth_max_signature_entries")
            )
            entry["join_pairs"] += metric_or_zero(record, "join_pairs")
            entry["join_signature_pairs"] += metric_or_zero(record, "join_signature_pairs")
        elif status(record) == "timeout":
            entry["timeouts"] += 1
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
                "signature_wins": collections.Counter(),
                "baseline_rows": 0,
                "baseline_best_time": 0,
                "baseline_best_table": 0,
                "baseline_best_signatures": 0,
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
        signature_configs = tied_winners(
            by_config,
            lambda record: (
                metric_or_zero(record, "rankwidth_max_signature_entries"),
                metric_or_zero(record, "join_signature_pairs"),
            ),
        )
        for config in fastest_configs:
            entry["time_wins"][config] += 1
        for config in table_configs:
            entry["table_wins"][config] += 1
        for config in signature_configs:
            entry["signature_wins"][config] += 1
        if baseline in by_config:
            entry["baseline_rows"] += 1
            entry["baseline_best_time"] += int(baseline in fastest_configs)
            entry["baseline_best_table"] += int(baseline in table_configs)
            entry["baseline_best_signatures"] += int(baseline in signature_configs)
    return [summary[key] for key in sorted(summary)]


def tied_winners(by_config: dict[str, dict], key_func) -> list[str]:
    best_key = min(key_func(record) for record in by_config.values())
    return sorted(config for config, record in by_config.items() if key_func(record) == best_key)


def counter_text(counter: collections.Counter) -> str:
    return "; ".join(f"{markdown_escape(key)} {value}" for key, value in counter.most_common())


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
        "| Tier | QSOP mode | Config | OK / records | Total solve time | Max width | "
        "Max table | Max signatures | Join pairs | Join signature pairs |",
        file=file,
    )
    print("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in config_summary(records):
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
            f"`{markdown_escape(row['config'])}` | {row['ok']} / {row['records']} | "
            f"{format_ns(row['elapsed_ns'])} | {row['max_width']} | {row['max_table']} | "
            f"{row['max_signatures']} | {row['join_pairs']} | {row['join_signature_pairs']} |",
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
        "| Tier | QSOP mode | Common rows | Time winners | Table winners | Signature winners | "
        "Baseline best time/table/signatures |",
        file=file,
    )
    print("| --- | --- | ---: | --- | --- | --- | ---: |", file=file)
    for row in rows:
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
            f"{row['common_rows']} | {counter_text(row['time_wins'])} | "
            f"{counter_text(row['table_wins'])} | {counter_text(row['signature_wins'])} | "
            f"{row['baseline_best_time']} / {row['baseline_best_table']} / "
            f"{row['baseline_best_signatures']} of {row['baseline_rows']} |",
            file=file,
        )


def serializable_row(row: dict) -> dict:
    out = dict(row)
    for key in ("time_wins", "table_wins", "signature_wins"):
        if key in out:
            out[key] = dict(out[key])
    return out


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare rankwidth generator benchmark JSONL artifacts.")
    parser.add_argument("--rankwidth-jsonl", action="append", type=labelled_path, default=[], metavar="TIER=PATH")
    parser.add_argument("--qsop-mode", choices=("all", "sign", "labelled"), default="all")
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
        }
        print(json.dumps(payload, sort_keys=True, indent=2))
    else:
        write_markdown(records, args.baseline, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
