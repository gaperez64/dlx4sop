#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import sys
from typing import Iterable, TextIO

from summarize_qasm_report import DEFAULT_TIERS, markdown_escape, summarize_reports


def read_jsonl(path: pathlib.Path) -> list[dict]:
    records = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"{path}:{line_number}: invalid JSONL row") from exc
    return records


def labelled_path(text: str) -> tuple[str, pathlib.Path]:
    if "=" in text:
        label, path_text = text.split("=", 1)
        if not label:
            raise argparse.ArgumentTypeError("label before '=' must be non-empty")
        return label, pathlib.Path(path_text)
    path = pathlib.Path(text)
    return path.stem, path


def format_ns(ns: int) -> str:
    if ns >= 1_000_000_000:
        return f"{ns / 1_000_000_000:.2f} s"
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.1f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.1f} us"
    return f"{ns} ns"


def solver_config(record: dict) -> str:
    backend = record.get("backend", "")
    if backend == "branch":
        return f"branch --branch-heuristic {record.get('branch_heuristic') or 'split'}"
    if backend == "rankwidth":
        return (
            f"rankwidth --rankwidth-generate {record.get('rankwidth_decomposition') or 'linear'} "
            f"--rankwidth-mode {record.get('rankwidth_mode') or 'count-table'}"
        )
    if backend == "treewidth":
        return f"treewidth --treewidth-order {record.get('treewidth_order') or 'min-fill'}"
    return str(backend)


def stat_value(record: dict, key: str) -> int | None:
    value = record.get(key)
    if isinstance(value, int):
        return value
    stats = record.get("stats", {})
    value = stats.get(key) if isinstance(stats, dict) else None
    return value if isinstance(value, int) else None


def add_sum(counter: dict[str, int], key: str, value: int | None) -> None:
    if isinstance(value, int):
        counter[key] = counter.get(key, 0) + value


def add_max(counter: dict[str, int], key: str, value: int | None) -> None:
    if isinstance(value, int):
        counter[key] = max(counter.get(key, 0), value)


def summarize_solver_records(named_records: Iterable[tuple[str, list[dict]]]) -> list[dict]:
    grouped: dict[tuple[str, str], dict] = {}
    for tier, records in named_records:
        for record in records:
            key = (tier, solver_config(record))
            entry = grouped.setdefault(
                key,
                {
                    "tier": tier,
                    "config": key[1],
                    "records": 0,
                    "ok": 0,
                    "timeouts": 0,
                    "errors": 0,
                    "elapsed_ns": 0,
                    "sources": collections.Counter(),
                    "stats": {},
                },
            )
            entry["records"] += 1
            status = record.get("status", "ok")
            if status == "ok":
                entry["ok"] += 1
            elif status == "timeout":
                entry["timeouts"] += 1
            else:
                entry["errors"] += 1
            entry["elapsed_ns"] += int(record.get("solve_elapsed_ns") or 0)
            entry["sources"][record.get("source") or "unknown"] += 1
            if status != "ok":
                continue
            stats = entry["stats"]
            for stat in ("search_nodes", "leaf_assignments", "cache_hits", "cache_misses", "components"):
                add_sum(stats, stat, stat_value(record, stat))
            for stat in (
                "rankwidth_width",
                "rankwidth_max_table_entries",
                "rankwidth_max_signature_entries",
                "treewidth_width",
                "treewidth_max_table_entries",
                "decomposition_width",
                "max_table_entries",
                "max_signature_entries",
            ):
                add_max(stats, stat, stat_value(record, stat))
            for stat in ("join_pairs", "join_signature_pairs"):
                add_sum(stats, stat, stat_value(record, stat))
    return [grouped[key] for key in sorted(grouped)]


def key_stats(stats: dict[str, int]) -> str:
    parts = []
    if "search_nodes" in stats:
        parts.append(f"{stats['search_nodes']} nodes")
    if "leaf_assignments" in stats:
        parts.append(f"{stats['leaf_assignments']} leaves")
    if "cache_hits" in stats or "cache_misses" in stats:
        parts.append(f"cache {stats.get('cache_hits', 0)} / {stats.get('cache_misses', 0)}")
    if "components" in stats:
        parts.append(f"{stats['components']} components")
    if "rankwidth_width" in stats:
        parts.append(f"rw width {stats['rankwidth_width']}")
    if "treewidth_width" in stats:
        parts.append(f"tw width {stats['treewidth_width']}")
    table = max(
        stats.get("rankwidth_max_table_entries", 0),
        stats.get("treewidth_max_table_entries", 0),
        stats.get("max_table_entries", 0),
    )
    if table:
        parts.append(f"max table {table}")
    if "rankwidth_max_signature_entries" in stats or "max_signature_entries" in stats:
        parts.append(f"max signatures {max(stats.get('rankwidth_max_signature_entries', 0), stats.get('max_signature_entries', 0))}")
    if "join_pairs" in stats:
        parts.append(f"{stats['join_pairs']} join pairs")
    return "; ".join(parts) if parts else ""


def solver_status_stats(row: dict) -> str:
    parts = [key_stats(row["stats"])]
    if row.get("timeouts"):
        parts.append(f"{row['timeouts']} timeouts")
    if row.get("errors"):
        parts.append(f"{row['errors']} errors")
    return "; ".join(part for part in parts if part)


def summarize_native_records(named_records: Iterable[tuple[str, list[dict]]]) -> list[dict]:
    grouped: dict[tuple[str, str], dict] = {}
    for tier, records in named_records:
        for record in records:
            key = (tier, record.get("engine") or "unknown")
            entry = grouped.setdefault(
                key,
                {
                    "tier": tier,
                    "engine": key[1],
                    "records": 0,
                    "ok": 0,
                    "skipped": 0,
                    "elapsed_ns": 0,
                    "max_qubits": 0,
                    "errors": collections.Counter(),
                },
            )
            entry["records"] += 1
            if record.get("status") == "ok":
                entry["ok"] += 1
                entry["elapsed_ns"] += int(record.get("elapsed_ns") or 0)
                if isinstance(record.get("qubits"), int):
                    entry["max_qubits"] = max(entry["max_qubits"], int(record["qubits"]))
            else:
                entry["skipped"] += 1
                error = str(record.get("error") or "")
                if error:
                    entry["errors"][error] += 1
    return [grouped[key] for key in sorted(grouped)]


def write_import_tables(report_paths: list[pathlib.Path], file: TextIO) -> None:
    summary = summarize_reports(report_paths, list(DEFAULT_TIERS))
    print("## Import Coverage\n", file=file)
    print("| Source | Upstream | Inputs | OK | Below min | Too large | Other unsupported |", file=file)
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in summary["sources"]:
        print(
            f"| {markdown_escape(row['source'])} | {markdown_escape(row.get('source_url') or '')} | "
            f"{row['inputs']} | {row['ok']} | {row['below_min_vars']} | {row['too_many_vars']} | "
            f"{row['unsupported']} |",
            file=file,
        )
    print("\n| Tier | Imported variables | Records | OK | Below min | Too large | Other unsupported | Sign | Labelled |", file=file)
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in summary["tier_summary"]:
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['range'])} | {row['records']} | "
            f"{row['ok']} | {row['below_min_vars']} | {row['too_many_vars']} | {row['unsupported']} | "
            f"{row['sign']} | {row['labelled']} |",
            file=file,
        )


def write_solver_tables(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    rows = summarize_solver_records(named_records)
    if not rows:
        return
    print("\n## Solver Results\n", file=file)
    print("| Tier | Backend/configuration | Solved / records | Total solve time | Key stats |", file=file)
    print("| --- | --- | ---: | ---: | --- |", file=file)
    for row in rows:
        print(
            f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['config'])}` | "
            f"{row['ok']} / {row['records']} | {format_ns(row['elapsed_ns'])} | "
            f"{markdown_escape(solver_status_stats(row))} |",
            file=file,
        )


def write_native_tables(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    rows = summarize_native_records(named_records)
    if not rows:
        return
    print("\n## Native Simulator Results\n", file=file)
    print("| Tier | Engine | OK / records | Total elapsed | Max qubits | Main skip reason |", file=file)
    print("| --- | --- | ---: | ---: | ---: | --- |", file=file)
    for row in rows:
        reason = row["errors"].most_common(1)[0][0] if row["errors"] else ""
        print(
            f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['engine'])}` | "
            f"{row['ok']} / {row['records']} | {format_ns(row['elapsed_ns'])} | {row['max_qubits']} | "
            f"{markdown_escape(reason)} |",
            file=file,
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render scoreboard Markdown tables from structured benchmark outputs.")
    parser.add_argument("--import-report", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--solver-jsonl", action="append", type=labelled_path, default=[], metavar="LABEL=PATH")
    parser.add_argument("--native-jsonl", action="append", type=labelled_path, default=[], metavar="LABEL=PATH")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        solver_records = [(label, read_jsonl(path)) for label, path in args.solver_jsonl]
        native_records = [(label, read_jsonl(path)) for label, path in args.native_jsonl]
        if args.import_report:
            write_import_tables(args.import_report, sys.stdout)
        write_solver_tables(solver_records, sys.stdout)
        write_native_tables(native_records, sys.stdout)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
