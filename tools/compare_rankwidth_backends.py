#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import sys
from typing import Iterable, TextIO

from render_scoreboard import format_ns, labelled_path, read_jsonl
from summarize_qasm_report import markdown_escape


RANKWIDTH_KERNEL_ELAPSED_FIELDS = (
    "rankwidth_join_map_elapsed_ns",
    "rankwidth_join_elapsed_ns",
    "rankwidth_labelled_join_map_elapsed_ns",
    "rankwidth_labelled_join_elapsed_ns",
    "rankwidth_fourier_join_map_elapsed_ns",
    "rankwidth_fourier_join_elapsed_ns",
    "rankwidth_labelled_fourier_join_map_elapsed_ns",
    "rankwidth_labelled_fourier_join_elapsed_ns",
)


def record_key(record: dict) -> tuple[str, str, str, str, str, str]:
    return (
        str(record.get("source") or "unknown"),
        str(record.get("source_relative_path") or ""),
        str(record.get("case") or ""),
        str(record.get("input") or ""),
        str(record.get("output") or ""),
        str(record.get("qsop_mode") or "unknown"),
    )


def backend_config(record: dict) -> str:
    backend = str(record.get("backend") or "unknown")
    solve_mode = record.get("solve_mode")
    solve_mode_suffix = (
        f":{solve_mode}"
        if isinstance(solve_mode, str) and solve_mode and solve_mode != "count-table"
        else ""
    )
    if backend == "rankwidth":
        generator = record.get("rankwidth_decomposition") or "left-deep"
        mode = record.get("rankwidth_mode") or "count-table"
        return f"rankwidth:{generator}:{mode}"
    if backend == "treewidth":
        return f"treewidth:{record.get('treewidth_order') or 'min-fill'}{solve_mode_suffix}"
    if backend == "branch":
        return f"branch:{record.get('branch_heuristic') or 'split'}{solve_mode_suffix}"
    return backend + solve_mode_suffix


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


def rankwidth_width(record: dict) -> int:
    return metric_or_zero(record, "rankwidth_width") or metric_or_zero(record, "decomposition_width")


def max_table(record: dict) -> int:
    backend = record.get("backend")
    if backend == "rankwidth":
        return metric_or_zero(record, "rankwidth_max_table_entries") or metric_or_zero(
            record, "max_table_entries"
        )
    if backend == "treewidth":
        return metric_or_zero(record, "treewidth_max_table_entries") or metric_or_zero(
            record, "max_table_entries"
        )
    return metric_or_zero(record, "max_table_entries")


def rankwidth_kernel_elapsed_ns(record: dict) -> int:
    return sum(metric_or_zero(record, key) for key in RANKWIDTH_KERNEL_ELAPSED_FIELDS)


def load_records(named_paths: Iterable[tuple[str, pathlib.Path]]) -> list[dict]:
    rows: list[dict] = []
    for tier, path in named_paths:
        for record in read_jsonl(path):
            copied = dict(record)
            copied["_tier"] = tier
            rows.append(copied)
    return rows


def backend_summary(records: list[dict]) -> list[dict]:
    grouped: dict[tuple[str, str, str], dict] = {}
    for record in records:
        key = (str(record["_tier"]), str(record.get("qsop_mode") or "unknown"), backend_config(record))
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
                "rankwidth_kernel_elapsed_ns": 0,
                "max_width": 0,
                "max_table": 0,
                "table_forecast_pressure": 0,
                "join_pair_forecast_pressure": 0,
            },
        )
        entry["records"] += 1
        if status(record) == "ok":
            entry["ok"] += 1
            entry["elapsed_ns"] += metric_or_zero(record, "solve_elapsed_ns")
            entry["rankwidth_kernel_elapsed_ns"] += rankwidth_kernel_elapsed_ns(record)
            entry["max_width"] = max(
                entry["max_width"],
                rankwidth_width(record)
                if record.get("backend") == "rankwidth"
                else metric_or_zero(record, "decomposition_width"),
            )
            entry["max_table"] = max(entry["max_table"], max_table(record))
            entry["table_forecast_pressure"] += metric_or_zero(record, "rankwidth_table_forecast")
            entry["join_pair_forecast_pressure"] += metric_or_zero(
                record, "rankwidth_join_pair_forecast"
            )
        elif status(record) == "timeout":
            entry["timeouts"] += 1
    return [grouped[key] for key in sorted(grouped)]


def compare_int(value: int) -> str:
    if value < 0:
        return "lower"
    if value > 0:
        return "higher"
    return "equal"


def comparison_summary(records: list[dict], rankwidth_config: str) -> list[dict]:
    grouped: dict[tuple[str, str, tuple[str, str, str, str, str, str]], dict[str, dict]] = {}
    for record in records:
        key = (str(record["_tier"]), str(record.get("qsop_mode") or "unknown"), record_key(record))
        grouped.setdefault(key, {})[backend_config(record)] = record

    summary: dict[tuple[str, str], dict] = {}
    for (tier, mode, _identity), by_config in grouped.items():
        rankwidth_record = by_config.get(rankwidth_config)
        competitors = [
            record
            for config, record in by_config.items()
            if config != rankwidth_config and status(record) == "ok"
        ]
        entry = summary.setdefault(
            (tier, mode),
            {
                "tier": tier,
                "mode": mode,
                "rows": 0,
                "rankwidth_ok": 0,
                "rankwidth_missing_or_failed": 0,
                "common_rows": 0,
                "rankwidth_fastest_or_tied": 0,
                "rankwidth_slower": 0,
                "rankwidth_elapsed_ns": 0,
                "best_competitor_elapsed_ns": 0,
                "elapsed_delta_vs_best_competitor": 0,
                "treewidth_common_rows": 0,
                "rankwidth_faster_than_treewidth": 0,
                "elapsed_delta_vs_treewidth": 0,
                "table_delta_vs_treewidth": 0,
                "forecast_delta_vs_treewidth_table": 0,
                "table_comparison": collections.Counter(),
                "forecast_comparison": collections.Counter(),
                "branch_common_rows": 0,
                "rankwidth_faster_than_branch": 0,
                "elapsed_delta_vs_branch": 0,
            },
        )
        entry["rows"] += 1
        if rankwidth_record is None or status(rankwidth_record) != "ok":
            entry["rankwidth_missing_or_failed"] += 1
            continue
        entry["rankwidth_ok"] += 1
        if not competitors:
            continue
        entry["common_rows"] += 1
        rankwidth_elapsed = metric_or_zero(rankwidth_record, "solve_elapsed_ns")
        best_competitor = min(competitors, key=lambda record: metric_or_zero(record, "solve_elapsed_ns"))
        best_competitor_elapsed = metric_or_zero(best_competitor, "solve_elapsed_ns")
        entry["rankwidth_elapsed_ns"] += rankwidth_elapsed
        entry["best_competitor_elapsed_ns"] += best_competitor_elapsed
        entry["elapsed_delta_vs_best_competitor"] += rankwidth_elapsed - best_competitor_elapsed
        if rankwidth_elapsed <= best_competitor_elapsed:
            entry["rankwidth_fastest_or_tied"] += 1
        else:
            entry["rankwidth_slower"] += 1

        treewidth_records = [
            record
            for config, record in by_config.items()
            if config.startswith("treewidth:") and status(record) == "ok"
        ]
        if treewidth_records:
            treewidth_record = min(
                treewidth_records, key=lambda record: metric_or_zero(record, "solve_elapsed_ns")
            )
            treewidth_elapsed = metric_or_zero(treewidth_record, "solve_elapsed_ns")
            table_delta = max_table(rankwidth_record) - max_table(treewidth_record)
            forecast_delta = (
                metric_or_zero(rankwidth_record, "rankwidth_table_forecast")
                - max_table(treewidth_record)
            )
            entry["treewidth_common_rows"] += 1
            entry["elapsed_delta_vs_treewidth"] += rankwidth_elapsed - treewidth_elapsed
            entry["table_delta_vs_treewidth"] += table_delta
            entry["forecast_delta_vs_treewidth_table"] += forecast_delta
            entry["table_comparison"][compare_int(table_delta)] += 1
            entry["forecast_comparison"][compare_int(forecast_delta)] += 1
            if rankwidth_elapsed <= treewidth_elapsed:
                entry["rankwidth_faster_than_treewidth"] += 1

        branch_records = [
            record
            for config, record in by_config.items()
            if config.startswith("branch:") and status(record) == "ok"
        ]
        if branch_records:
            branch_record = min(
                branch_records, key=lambda record: metric_or_zero(record, "solve_elapsed_ns")
            )
            branch_elapsed = metric_or_zero(branch_record, "solve_elapsed_ns")
            entry["branch_common_rows"] += 1
            entry["elapsed_delta_vs_branch"] += rankwidth_elapsed - branch_elapsed
            if rankwidth_elapsed <= branch_elapsed:
                entry["rankwidth_faster_than_branch"] += 1
    return [summary[key] for key in sorted(summary)]


def top_rankwidth_wins(records: list[dict], rankwidth_config: str, limit: int) -> list[dict]:
    if limit <= 0:
        return []
    grouped: dict[tuple[str, str, tuple[str, str, str, str, str, str]], dict[str, dict]] = {}
    for record in records:
        key = (str(record["_tier"]), str(record.get("qsop_mode") or "unknown"), record_key(record))
        grouped.setdefault(key, {})[backend_config(record)] = record

    rows: list[dict] = []
    for (tier, mode, identity), by_config in grouped.items():
        rankwidth_record = by_config.get(rankwidth_config)
        if rankwidth_record is None or status(rankwidth_record) != "ok":
            continue
        competitors = [
            record
            for config, record in by_config.items()
            if config != rankwidth_config and status(record) == "ok"
        ]
        if not competitors:
            continue
        rankwidth_elapsed = metric_or_zero(rankwidth_record, "solve_elapsed_ns")
        best_competitor = min(competitors, key=lambda record: metric_or_zero(record, "solve_elapsed_ns"))
        best_elapsed = metric_or_zero(best_competitor, "solve_elapsed_ns")
        if rankwidth_elapsed > best_elapsed:
            continue
        rows.append(
            {
                "tier": tier,
                "mode": mode,
                "source": identity[0],
                "path": identity[1],
                "case": identity[2],
                "input": identity[3],
                "output": identity[4],
                "rankwidth_elapsed_ns": rankwidth_elapsed,
                "best_competitor": backend_config(best_competitor),
                "best_competitor_elapsed_ns": best_elapsed,
                "elapsed_win_ns": best_elapsed - rankwidth_elapsed,
                "rankwidth_table": max_table(rankwidth_record),
                "rankwidth_table_forecast": metric_or_zero(rankwidth_record, "rankwidth_table_forecast"),
                "rankwidth_join_pair_forecast": metric_or_zero(
                    rankwidth_record, "rankwidth_join_pair_forecast"
                ),
            }
        )
    rows.sort(key=lambda row: (row["elapsed_win_ns"], -row["rankwidth_elapsed_ns"]), reverse=True)
    return rows[:limit]


def counter_triplet(counter: collections.Counter) -> str:
    return f"{counter['lower']} / {counter['equal']} / {counter['higher']}"


def format_signed_ns(ns: int) -> str:
    if ns < 0:
        return f"-{format_ns(-ns)}"
    return format_ns(ns)


def rankwidth_configs(records: list[dict]) -> list[str]:
    configs = {
        backend_config(record)
        for record in records
        if record.get("backend") == "rankwidth" and status(record) == "ok"
    }
    return sorted(configs)


def markdown_heading(level: int, title: str) -> str:
    return f"{'#' * max(1, level)} {title}"


def write_markdown(
    records: list[dict],
    rankwidth_config: str,
    top: int,
    file: TextIO,
    heading_level: int = 1,
) -> None:
    print(f"{markdown_heading(heading_level, 'Rankwidth Backend Comparison')}\n", file=file)
    print(
        "Rows compare rankwidth against non-rankwidth backends on common "
        "source/case/boundary/QSOP-mode keys. Best competitor is the fastest "
        "successful non-rankwidth backend for that row.",
        file=file,
    )

    print(f"\n{markdown_heading(heading_level + 1, 'Backend Summary')}\n", file=file)
    print(
        "| Tier | QSOP mode | Config | OK / records | Total solve time | Max width | "
        "Max table | Rankwidth kernel time | Forecast table pressure | Forecast join pressure |",
        file=file,
    )
    print("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in backend_summary(records):
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
            f"`{markdown_escape(row['config'])}` | {row['ok']} / {row['records']} | "
            f"{format_ns(row['elapsed_ns'])} | {row['max_width']} | {row['max_table']} | "
            f"{format_ns(row['rankwidth_kernel_elapsed_ns'])} | "
            f"{row['table_forecast_pressure']} | {row['join_pair_forecast_pressure']} |",
            file=file,
        )

    configs = rankwidth_configs(records) if rankwidth_config == "all" else [rankwidth_config]
    for config in configs:
        title = "Rankwidth Vs Competitors"
        if len(configs) > 1:
            title += f": {config}"
        print(f"\n{markdown_heading(heading_level + 1, title)}\n", file=file)
        print(
            f"Rankwidth config: `{markdown_escape(config)}`. "
            "Table and forecast comparisons are lower/equal/higher versus the fastest "
            "successful treewidth row.",
            file=file,
        )
        print(
            "| Tier | QSOP mode | Common rows | Fastest/tied | Slower | "
            "Rankwidth time | Best competitor time | Delta vs best | "
            "Vs treewidth faster | Table lower/equal/higher | Forecast lower/equal/higher | "
            "Vs branch faster | Delta vs treewidth/branch | Missing/failed |",
            file=file,
        )
        print(
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            file=file,
        )
        for row in comparison_summary(records, config):
            print(
                f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
                f"{row['common_rows']} | {row['rankwidth_fastest_or_tied']} | "
                f"{row['rankwidth_slower']} | {format_ns(row['rankwidth_elapsed_ns'])} | "
                f"{format_ns(row['best_competitor_elapsed_ns'])} | "
                f"{format_signed_ns(row['elapsed_delta_vs_best_competitor'])} | "
                f"{row['rankwidth_faster_than_treewidth']} / {row['treewidth_common_rows']} | "
                f"{counter_triplet(row['table_comparison'])} | "
                f"{counter_triplet(row['forecast_comparison'])} | "
                f"{row['rankwidth_faster_than_branch']} / {row['branch_common_rows']} | "
                f"{format_signed_ns(row['elapsed_delta_vs_treewidth'])} / "
                f"{format_signed_ns(row['elapsed_delta_vs_branch'])} | "
                f"{row['rankwidth_missing_or_failed']} |",
                file=file,
            )

        wins = top_rankwidth_wins(records, config, top)
        if not wins:
            continue
        wins_title = "Largest Rankwidth Time Wins"
        if len(configs) > 1:
            wins_title += f": {config}"
        print(f"\n{markdown_heading(heading_level + 1, wins_title)}\n", file=file)
        print(
            "| Tier | QSOP mode | Row | Rankwidth | Best competitor | Win | "
            "Table / forecast / join forecast |",
            file=file,
        )
        print("| --- | --- | --- | ---: | ---: | ---: | ---: |", file=file)
        for row in wins:
            label = f"{row['source']}:{row['case']} {row['input']}->{row['output']}"
            print(
                f"| {markdown_escape(row['tier'])} | {markdown_escape(row['mode'])} | "
                f"{markdown_escape(label)} | {format_ns(row['rankwidth_elapsed_ns'])} | "
                f"`{markdown_escape(row['best_competitor'])}` {format_ns(row['best_competitor_elapsed_ns'])} | "
                f"{format_ns(row['elapsed_win_ns'])} | "
                f"{row['rankwidth_table']} / {row['rankwidth_table_forecast']} / "
                f"{row['rankwidth_join_pair_forecast']} |",
                file=file,
            )


def serializable_row(row: dict) -> dict:
    copied = dict(row)
    for key in ("table_comparison", "forecast_comparison"):
        if key in copied:
            copied[key] = dict(copied[key])
    return copied


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare rankwidth against treewidth and branch JSONL rows.")
    parser.add_argument(
        "--comparison-jsonl",
        action="append",
        type=labelled_path,
        default=[],
        metavar="TIER=PATH",
        help="JSONL produced by bench_qasm_corpus.py --rankwidth-comparison.",
    )
    parser.add_argument("--qsop-mode", choices=("all", "sign", "labelled"), default="all")
    parser.add_argument("--rankwidth-config", default="rankwidth:min-fill-cut:count-table")
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.top < 0:
        print("error: --top must be non-negative", file=sys.stderr)
        return 2
    try:
        records = load_records(args.comparison_jsonl)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.qsop_mode != "all":
        records = [record for record in records if record.get("qsop_mode") == args.qsop_mode]
    if args.format == "json":
        payload = {
            "backend_summary": backend_summary(records),
            "comparison_summary": [
                serializable_row(row) for row in comparison_summary(records, args.rankwidth_config)
            ],
            "top_rankwidth_wins": top_rankwidth_wins(records, args.rankwidth_config, args.top),
        }
        print(json.dumps(payload, sort_keys=True, indent=2))
    else:
        write_markdown(records, args.rankwidth_config, args.top, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
