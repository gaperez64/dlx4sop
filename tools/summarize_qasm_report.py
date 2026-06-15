#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import sys
from typing import Iterable


DEFAULT_TIERS = (
    ("0-32", 0, 32),
    ("33-64", 33, 64),
    ("65-128", 65, 128),
    ("129-256", 129, 256),
    ("257+", 257, None),
)


def parse_tier(text: str) -> tuple[str, int, int | None]:
    parts = text.split(":")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("tier must be NAME:MIN:MAX, with empty MAX allowed")
    name, min_text, max_text = parts
    if not name:
        raise argparse.ArgumentTypeError("tier name must be non-empty")
    try:
        minimum = int(min_text)
        maximum = int(max_text) if max_text else None
    except ValueError as exc:
        raise argparse.ArgumentTypeError("tier bounds must be integers") from exc
    if minimum < 0:
        raise argparse.ArgumentTypeError("tier minimum must be non-negative")
    if maximum is not None and maximum < minimum:
        raise argparse.ArgumentTypeError("tier maximum must be greater than or equal to minimum")
    return name, minimum, maximum


def tier_label(value: int | None, tiers: Iterable[tuple[str, int, int | None]]) -> str:
    if value is None:
        return "unknown"
    for name, minimum, maximum in tiers:
        if value >= minimum and (maximum is None or value <= maximum):
            return name
    return "unclassified"


def tier_range(name: str, tiers: Iterable[tuple[str, int, int | None]]) -> str:
    if name == "unknown":
        return "unknown"
    if name == "unclassified":
        return "outside configured tiers"
    for tier_name, minimum, maximum in tiers:
        if tier_name == name:
            return f"{minimum}+" if maximum is None else f"{minimum}-{maximum}"
    return name


def markdown_escape(text: object) -> str:
    return str(text).replace("|", "\\|")


def record_source(record: dict, report: dict) -> str:
    return str(record.get("source") or report.get("source") or "unknown")


def record_mode(record: dict) -> str:
    return str(record.get("mode") or "unknown")


def summarize_reports(paths: list[pathlib.Path], tiers: list[tuple[str, int, int | None]]) -> dict:
    reports = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    sources: dict[str, dict] = {}
    tier_counts: collections.Counter[tuple[str, str, str]] = collections.Counter()
    too_large: collections.Counter[tuple[str, str, str]] = collections.Counter()
    diagnostics: collections.Counter[tuple[str, str]] = collections.Counter()
    total_records = 0

    for report in reports:
        for record in report.get("records", []):
            total_records += 1
            source = record_source(record, report)
            source_entry = sources.setdefault(
                source,
                {
                    "source": source,
                    "source_url": record.get("source_url") or report.get("source_url"),
                    "inputs": 0,
                    "statuses": collections.Counter(),
                    "modes": collections.Counter(),
                },
            )
            if source_entry.get("source_url") is None and record.get("source_url") is not None:
                source_entry["source_url"] = record["source_url"]
            status = str(record.get("status", "unknown"))
            mode = record_mode(record)
            max_nvars = record.get("max_imported_nvars")
            if not isinstance(max_nvars, int):
                max_nvars = None
            label = tier_label(max_nvars, tiers)

            source_entry["inputs"] += 1
            source_entry["statuses"][status] += 1
            source_entry["modes"][mode] += 1
            tier_counts[(label, status, mode)] += 1
            if status == "too_many_vars":
                too_large[(source, label, mode)] += 1
            diagnostic = record.get("diagnostic")
            if diagnostic:
                diagnostics[(status, str(diagnostic))] += 1

    source_rows = []
    status_rows = []
    for source in sorted(sources):
        entry = sources[source]
        statuses = entry["statuses"]
        unsupported = sum(
            count for status, count in statuses.items()
            if status not in ("ok", "below_min_vars", "too_many_vars")
        )
        source_rows.append(
            {
                "source": source,
                "source_url": entry.get("source_url"),
                "inputs": entry["inputs"],
                "ok": statuses.get("ok", 0),
                "below_min_vars": statuses.get("below_min_vars", 0),
                "too_many_vars": statuses.get("too_many_vars", 0),
                "unsupported": unsupported,
            }
        )
        for status, count in sorted(statuses.items()):
            status_rows.append({"source": source, "status": status, "records": count})

    tier_rows = []
    labels = [name for name, _, _ in tiers] + ["unknown", "unclassified"]
    for label in labels:
        entries = [(status, mode, count) for (tier, status, mode), count in tier_counts.items() if tier == label]
        if not entries:
            continue
        statuses = collections.Counter()
        modes = collections.Counter()
        total = 0
        for status, mode, count in entries:
            total += count
            statuses[status] += count
            modes[mode] += count
        tier_rows.append(
            {
                "tier": label,
                "range": tier_range(label, tiers),
                "records": total,
                "ok": statuses.get("ok", 0),
                "below_min_vars": statuses.get("below_min_vars", 0),
                "too_many_vars": statuses.get("too_many_vars", 0),
                "unsupported": sum(
                    count for status, count in statuses.items()
                    if status not in ("ok", "below_min_vars", "too_many_vars")
                ),
                "sign": modes.get("sign", 0),
                "labelled": modes.get("labelled", 0),
                "unknown_mode": modes.get("unknown", 0),
            }
        )

    too_large_rows = [
        {"source": source, "tier": label, "mode": mode, "records": count}
        for (source, label, mode), count in sorted(too_large.items())
    ]
    diagnostic_rows = [
        {"status": status, "diagnostic": diagnostic, "records": count}
        for (status, diagnostic), count in diagnostics.most_common()
    ]

    return {
        "reports": [str(path) for path in paths],
        "records": total_records,
        "tiers": [{"name": name, "min": minimum, "max": maximum} for name, minimum, maximum in tiers],
        "sources": source_rows,
        "status_summary": status_rows,
        "tier_summary": tier_rows,
        "too_large": too_large_rows,
        "diagnostics": diagnostic_rows,
    }


def write_markdown(summary: dict, top_diagnostics: int, file) -> None:
    print("# QASM Import Report Summary\n", file=file)
    print("## Sources\n", file=file)
    print("| Source | Upstream | Inputs | OK | Below min | Too large | Other unsupported |", file=file)
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in summary["sources"]:
        url = row.get("source_url") or ""
        print(
            f"| {markdown_escape(row['source'])} | {markdown_escape(url)} | {row['inputs']} | "
            f"{row['ok']} | {row['below_min_vars']} | {row['too_many_vars']} | {row['unsupported']} |",
            file=file,
        )

    print("\n## Status Breakdown\n", file=file)
    print("| Source | Status | Records |", file=file)
    print("| --- | --- | ---: |", file=file)
    for row in summary["status_summary"]:
        print(
            f"| {markdown_escape(row['source'])} | {markdown_escape(row['status'])} | "
            f"{row['records']} |",
            file=file,
        )

    print("\n## Size Tiers\n", file=file)
    print("| Tier | Imported variables | Records | OK | Below min | Too large | Other unsupported | Sign | Labelled | Unknown mode |", file=file)
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in summary["tier_summary"]:
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['range'])} | {row['records']} | "
            f"{row['ok']} | {row['below_min_vars']} | {row['too_many_vars']} | {row['unsupported']} | "
            f"{row['sign']} | {row['labelled']} | {row['unknown_mode']} |",
            file=file,
        )

    print("\n## Too-Large Candidates\n", file=file)
    print("| Source | Tier | Mode | Records |", file=file)
    print("| --- | --- | --- | ---: |", file=file)
    for row in summary["too_large"]:
        print(
            f"| {markdown_escape(row['source'])} | {markdown_escape(row['tier'])} | "
            f"{markdown_escape(row['mode'])} | {row['records']} |",
            file=file,
        )

    if top_diagnostics > 0:
        print("\n## Top Diagnostics\n", file=file)
        print("| Status | Records | Diagnostic |", file=file)
        print("| --- | ---: | --- |", file=file)
        for row in summary["diagnostics"][:top_diagnostics]:
            print(
                f"| {markdown_escape(row['status'])} | {row['records']} | "
                f"{markdown_escape(row['diagnostic'])} |",
                file=file,
            )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize qasm2sop external import reports by benchmark tier.")
    parser.add_argument("reports", nargs="+", type=pathlib.Path)
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument(
        "--tier",
        action="append",
        type=parse_tier,
        help="Tier as NAME:MIN:MAX. Repeat to replace the default tier set; empty MAX means unbounded.",
    )
    parser.add_argument("--top-diagnostics", type=int, default=10)
    args = parser.parse_args(argv)
    if args.top_diagnostics < 0:
        parser.error("--top-diagnostics must be non-negative")
    args.tiers = args.tier or list(DEFAULT_TIERS)
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    summary = summarize_reports(args.reports, args.tiers)
    if args.format == "json":
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        write_markdown(summary, args.top_diagnostics, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
