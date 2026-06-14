#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import subprocess
import sys
import time
from typing import TextIO


def run_command(cmd: list[str], *, input_text: str | None = None) -> tuple[str, int]:
    start = time.perf_counter_ns()
    completed = subprocess.run(
        cmd,
        input=input_text,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.perf_counter_ns() - start
    if completed.returncode != 0:
        raise RuntimeError(f"command failed: {cmd}\n{completed.stderr}")
    return completed.stdout, elapsed


def case_qasm(case: dict) -> str:
    return "\n".join(case["qasm_lines"]) + "\n"


def iter_case_boundaries(cases: list[dict], limit: int | None):
    seen = 0
    for case in cases:
        qasm = case_qasm(case)
        for boundary_index, (input_bits, output_bits) in enumerate(case["boundaries"]):
            if limit is not None and seen >= limit:
                return
            seen += 1
            yield case, boundary_index, qasm, input_bits, output_bits


def benchmark(args: argparse.Namespace) -> list[dict]:
    cases = json.loads(args.manifest.read_text(encoding="utf-8"))
    records = []
    for case, boundary_index, qasm, input_bits, output_bits in iter_case_boundaries(cases, args.limit):
        qsop, import_elapsed_ns = run_command(
            [str(args.qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
            input_text=qasm,
        )
        stats_text, stats_elapsed_ns = run_command(
            [
                str(args.sop_stats),
                "--format",
                "json",
                "--exact-widths",
                "--exact-width-max-vars",
                str(args.exact_width_max_vars),
                "-",
            ],
            input_text=qsop,
        )
        stats = json.loads(stats_text)
        records.append(
            {
                "case": case["name"],
                "source": case.get("source", "internal"),
                "source_url": case.get("source_url", ""),
                "source_relative_path": case.get("source_relative_path", ""),
                "boundary": boundary_index,
                "input": input_bits,
                "output": output_bits,
                "import_elapsed_ns": import_elapsed_ns,
                "stats_elapsed_ns": stats_elapsed_ns,
                "variables": stats["variables"],
                "quadratic_terms": stats["quadratic_terms"],
                "mode": stats["mode"],
                "components": stats["components"],
                "max_degree": stats["max_degree"],
                "min_fill_width": stats.get("min_fill_width"),
                "prefix_cut_rank": stats.get("prefix_cut_rank"),
                "exact_width_max_vars": stats.get("exact_width_max_vars"),
                "exact_widths_available": bool(stats.get("exact_widths_available", False)),
                "exact_treewidth": stats.get("exact_treewidth"),
                "exact_rankwidth": stats.get("exact_rankwidth"),
            }
        )
    return records


def write_jsonl(records: list[dict], file: TextIO) -> None:
    for record in records:
        print(json.dumps(record, sort_keys=True, separators=(",", ":")), file=file)


def distribution(records: list[dict], key: str) -> str:
    counts = collections.Counter(record.get(key) for record in records if isinstance(record.get(key), int))
    if not counts:
        return "n/a"
    return ", ".join(f"{value}:{counts[value]}" for value in sorted(counts))


def max_record(records: list[dict], key: str) -> dict | None:
    candidates = [record for record in records if isinstance(record.get(key), int)]
    if not candidates:
        return None
    return max(candidates, key=lambda record: (record[key], record["variables"], record["case"]))


def write_summary(records: list[dict], file: TextIO) -> None:
    exact = [record for record in records if record["exact_widths_available"]]
    skipped = [record for record in records if not record["exact_widths_available"]]
    print(f"records: {len(records)}", file=file)
    print(f"exact_available: {len(exact)}", file=file)
    print(f"exact_skipped: {len(skipped)}", file=file)
    print(f"exact_treewidth_distribution: {distribution(exact, 'exact_treewidth')}", file=file)
    print(f"exact_rankwidth_distribution: {distribution(exact, 'exact_rankwidth')}", file=file)
    print(f"min_fill_width_distribution: {distribution(records, 'min_fill_width')}", file=file)
    print(f"prefix_cut_rank_distribution: {distribution(records, 'prefix_cut_rank')}", file=file)
    for label, key in (
        ("largest_exact_treewidth", "exact_treewidth"),
        ("largest_exact_rankwidth", "exact_rankwidth"),
        ("largest_min_fill_width", "min_fill_width"),
        ("largest_prefix_cut_rank", "prefix_cut_rank"),
    ):
        record = max_record(records, key)
        if record is None:
            continue
        print(
            f"{label}: {record['source']}:{record['case']} {record['input']}->{record['output']} "
            f"value={record[key]} variables={record['variables']} terms={record['quadratic_terms']} "
            f"mode={record['mode']}",
            file=file,
        )
    if skipped:
        skipped_sources = collections.Counter(record["source"] for record in skipped)
        print("skipped_sources:", file=file)
        for source in sorted(skipped_sources):
            print(f"  {source}: {skipped_sources[source]}", file=file)


def parse_args(argv: list[str]) -> argparse.Namespace:
    source_root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Measure heuristic and exact support-graph widths on a QASM corpus.")
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("sop_stats", type=pathlib.Path)
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=source_root / "tests" / "qasm_solver_corpus.json",
        help="QASM corpus manifest.",
    )
    parser.add_argument("--limit", type=int)
    parser.add_argument("--exact-width-max-vars", type=int, default=12)
    parser.add_argument("--format", choices=("jsonl", "summary"), default="jsonl")
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.exact_width_max_vars < 0:
        parser.error("--exact-width-max-vars must be non-negative")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        records = benchmark(args)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.format == "jsonl":
        write_jsonl(records, sys.stdout)
    else:
        write_summary(records, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
