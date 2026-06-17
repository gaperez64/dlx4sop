#!/usr/bin/env python3
"""Analyse branch-calibrate-backends JSONL output and suggest policy thresholds.

Usage:
    tune_branch_thresholds.py [--win-rate FRAC] [--format text|json] FILE [FILE ...]

Each FILE must be a JSONL file produced by:
    sop-solve --backend branch --branch-calibrate-backends --stats-jsonl FILE ...

Records where both treewidth_actual_ms and rankwidth_actual_ms are present
(calibration records) are used to estimate per-width win rates.  The tool
then recommends values for BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH and
BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH in branch.c.
"""

import argparse
import json
import pathlib
import sys
from collections import defaultdict

SCHEMA = "sop_solve_backend_stats_v1"


def read_jsonl(path: pathlib.Path):
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    pass


def load_calibration_records(paths: list[pathlib.Path]) -> list[dict]:
    """Return records where both backends were actually timed."""
    records = []
    for path in paths:
        for rec in read_jsonl(path):
            if (rec.get("schema") == SCHEMA
                    and rec.get("treewidth_actual_ms") is not None
                    and rec.get("rankwidth_actual_ms") is not None):
                records.append(rec)
    return records


def analyse_by_width(records: list[dict], width_field: str,
                     speed_field: str, baseline_field: str) -> dict:
    """Return per-width statistics for speed_field vs baseline_field."""
    buckets: dict[int, dict] = defaultdict(
        lambda: {"total": 0, "wins": 0, "speed_ms": 0.0, "baseline_ms": 0.0}
    )
    for rec in records:
        width = rec.get(width_field)
        speed = rec.get(speed_field)
        baseline = rec.get(baseline_field)
        if width is None or not isinstance(width, int):
            continue
        if speed is None or baseline is None:
            continue
        b = buckets[width]
        b["total"] += 1
        b["speed_ms"] += float(speed)
        b["baseline_ms"] += float(baseline)
        if float(speed) < float(baseline):
            b["wins"] += 1
    return dict(buckets)


def threshold_recommendation(buckets: dict, win_rate_threshold: float = 0.6) -> int | None:
    """Return the maximum width where win_rate >= threshold, or None."""
    best: int | None = None
    for width in sorted(buckets):
        b = buckets[width]
        if b["total"] == 0:
            continue
        if b["wins"] / b["total"] >= win_rate_threshold:
            best = width
    return best


def format_text_report(records: list[dict], rw_buckets: dict, tw_buckets: dict,
                        rw_threshold: int | None, tw_threshold: int | None,
                        win_rate: float, file=sys.stdout) -> None:
    print(f"Calibration records: {len(records)}", file=file)
    print(file=file)
    print("Rankwidth win rate by labelled width (rankwidth_actual_ms < treewidth_actual_ms):",
          file=file)
    print(f"  {'width':>6}  {'total':>6}  {'wins':>6}  {'win_rate':>8}  {'rw_ms/tw_ms':>12}",
          file=file)
    for w in sorted(rw_buckets):
        b = rw_buckets[w]
        rate = b["wins"] / b["total"] if b["total"] else 0.0
        speedup = (b["baseline_ms"] / b["speed_ms"]) if b["speed_ms"] > 0 else float("inf")
        print(f"  {w:>6}  {b['total']:>6}  {b['wins']:>6}  {rate:>8.3f}  {speedup:>12.2f}x",
              file=file)
    print(file=file)
    print("Treewidth win rate by treewidth (treewidth_actual_ms < rankwidth_actual_ms):",
          file=file)
    print(f"  {'width':>6}  {'total':>6}  {'wins':>6}  {'win_rate':>8}  {'tw_ms/rw_ms':>12}",
          file=file)
    for w in sorted(tw_buckets):
        b = tw_buckets[w]
        rate = b["wins"] / b["total"] if b["total"] else 0.0
        speedup = (b["baseline_ms"] / b["speed_ms"]) if b["speed_ms"] > 0 else float("inf")
        print(f"  {w:>6}  {b['total']:>6}  {b['wins']:>6}  {rate:>8.3f}  {speedup:>12.2f}x",
              file=file)
    print(file=file)
    print(f"Suggestions (win_rate >= {win_rate:.0%}):", file=file)
    if rw_threshold is not None:
        print(f"  BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH = {rw_threshold}", file=file)
    else:
        print(f"  BRANCH_RANKWIDTH_DELEGATE_MAX_WIDTH: no threshold found", file=file)
    if tw_threshold is not None:
        print(f"  BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH = {tw_threshold}", file=file)
    else:
        print(f"  BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH: no threshold found", file=file)


def format_json_report(records: list[dict], rw_buckets: dict, tw_buckets: dict,
                        rw_threshold: int | None, tw_threshold: int | None) -> dict:
    def bucket_entry(b: dict) -> dict:
        return {
            "total": b["total"],
            "wins": b["wins"],
            "win_rate": round(b["wins"] / b["total"], 4) if b["total"] else 0.0,
        }

    return {
        "calibration_records": len(records),
        "rankwidth_max_width_recommendation": rw_threshold,
        "treewidth_max_width_recommendation": tw_threshold,
        "rankwidth_win_rate_by_width": {
            str(w): bucket_entry(b) for w, b in sorted(rw_buckets.items())
        },
        "treewidth_win_rate_by_width": {
            str(w): bucket_entry(b) for w, b in sorted(tw_buckets.items())
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("jsonl", nargs="+", type=pathlib.Path,
                        help="JSONL files from --branch-calibrate-backends runs")
    parser.add_argument("--win-rate", type=float, default=0.6, metavar="FRAC",
                        help="Min win-rate to include a width in recommendation (default: 0.6)")
    parser.add_argument("--format", choices=["text", "json"], default="text",
                        dest="output_format")
    args = parser.parse_args(argv)

    records = load_calibration_records(args.jsonl)
    if not records:
        print("No calibration records found.  Run sop-solve with "
              "--branch-calibrate-backends --stats-jsonl to generate them.",
              file=sys.stderr)
        return 1

    rw_buckets = analyse_by_width(
        records,
        width_field="rankwidth_labelled_width",
        speed_field="rankwidth_actual_ms",
        baseline_field="treewidth_actual_ms",
    )
    tw_buckets = analyse_by_width(
        records,
        width_field="treewidth_width",
        speed_field="treewidth_actual_ms",
        baseline_field="rankwidth_actual_ms",
    )

    rw_threshold = threshold_recommendation(rw_buckets, args.win_rate)
    tw_threshold = threshold_recommendation(tw_buckets, args.win_rate)

    if args.output_format == "json":
        result = format_json_report(records, rw_buckets, tw_buckets, rw_threshold, tw_threshold)
        print(json.dumps(result, indent=2))
    else:
        format_text_report(records, rw_buckets, tw_buckets, rw_threshold, tw_threshold,
                           args.win_rate)
    return 0


if __name__ == "__main__":
    sys.exit(main())
