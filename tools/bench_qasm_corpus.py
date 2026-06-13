#!/usr/bin/env python3

import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import sys
import time
from typing import TextIO


BACKENDS = ("components", "brute-force", "branch")
BRANCH_HEURISTICS = ("split", "treewidth", "linear-rankwidth")
TOP_METRICS = (
    "solve_elapsed_ns",
    "import_elapsed_ns",
    "search_nodes",
    "leaf_assignments",
    "cache_hits",
    "cache_misses",
    "components",
)
CSV_FIELDS = [
    "case",
    "boundary",
    "input",
    "output",
    "backend",
    "branch_heuristic",
    "r",
    "nvars",
    "nedges",
    "import_elapsed_ns",
    "solve_elapsed_ns",
    "search_nodes",
    "leaf_assignments",
    "cache_hits",
    "cache_misses",
    "components",
    "qasm_sha256",
    "qsop_sha256",
    "trace_summary",
]


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def run_command(cmd: list[str], *, input_text: str | None = None) -> tuple[str, str, int]:
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
    return completed.stdout, completed.stderr, elapsed


def load_cases(path: pathlib.Path) -> list[dict]:
    return json.loads(path.read_text())


def case_qasm(case: dict) -> str:
    return "\n".join(case["qasm_lines"]) + "\n"


def qsop_header(qsop: str) -> dict[str, int]:
    for line in qsop.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop"]:
            return {
                "r": int(parts[2]),
                "nvars": int(parts[3]),
                "nedges": int(parts[4]),
            }
    raise RuntimeError(f"missing QSOP header:\n{qsop}")


def parse_stats(text: str) -> dict[str, int | str]:
    stats: dict[str, int | str] = {}
    for line in text.splitlines():
        if not line:
            continue
        key, value = line.split(": ", 1)
        stats[key] = value if key in {"backend", "branch_heuristic"} else int(value)
    return stats


def parse_trace_csv(text: str) -> dict[str, dict[str, int]]:
    rows = [line for line in text.splitlines() if line]
    if not rows:
        return {}
    if rows[0] != "phase,depth,items,elapsed_ns":
        raise RuntimeError(f"unexpected trace header:\n{text}")

    summary: dict[str, dict[str, int]] = {}
    for row in rows[1:]:
        phase, _depth, items, elapsed_ns = row.split(",", 3)
        entry = summary.setdefault(phase, {"events": 0, "items": 0, "elapsed_ns": 0})
        entry["events"] += 1
        entry["items"] += int(items)
        entry["elapsed_ns"] += int(elapsed_ns)
    return summary


def trace_summary_text(trace: dict[str, dict[str, int]]) -> str:
    return ";".join(
        f"{phase}:{values['events']}:{values['elapsed_ns']}" for phase, values in sorted(trace.items())
    )


def add_counter(total: dict[str, int], key: str, value: int | str | None) -> None:
    if isinstance(value, int):
        total[key] = total.get(key, 0) + value


def summarize_records(records: list[dict]) -> dict[str, dict]:
    summary: dict[str, dict] = {}
    for record in records:
        backend = record["backend"]
        entry = summary.setdefault(
            backend,
            {
                "records": 0,
                "solve_elapsed_ns": 0,
                "import_elapsed_ns": 0,
                "stats": {},
                "trace": {},
            },
        )
        entry["records"] += 1
        entry["solve_elapsed_ns"] += record["solve_elapsed_ns"]
        entry["import_elapsed_ns"] += record["import_elapsed_ns"]

        stats_total = entry["stats"]
        for key, value in record["stats"].items():
            add_counter(stats_total, key, value)

        trace_total = entry["trace"]
        for phase, values in record["trace"].items():
            phase_total = trace_total.setdefault(phase, {"events": 0, "items": 0, "elapsed_ns": 0})
            phase_total["events"] += values["events"]
            phase_total["items"] += values["items"]
            phase_total["elapsed_ns"] += values["elapsed_ns"]
    return summary


def cache_hit_rate(stats: dict[str, int]) -> str:
    hits = stats.get("cache_hits", 0)
    misses = stats.get("cache_misses", 0)
    total = hits + misses
    if total == 0:
        return "n/a"
    return f"{hits / total:.3f}"


def record_metric(record: dict, metric: str) -> int:
    value = record.get(metric)
    if isinstance(value, int):
        return value
    value = record["stats"].get(metric)
    return value if isinstance(value, int) else 0


def record_has_metric(record: dict, metric: str) -> bool:
    return isinstance(record.get(metric), int) or isinstance(record["stats"].get(metric), int)


def dominant_trace_phase(record: dict) -> str:
    trace = record["trace"]
    if not trace:
        return ""
    phase, values = max(trace.items(), key=lambda item: (item[1]["elapsed_ns"], item[0]))
    return (
        f"{phase}:events={values['events']}:items={values['items']}:"
        f"elapsed_ns={values['elapsed_ns']}"
    )


def iter_case_boundaries(cases: list[dict], limit: int | None):
    seen = 0
    for case in cases:
        qasm = case_qasm(case)
        for input_bits, output_bits in case["boundaries"]:
            if limit is not None and seen >= limit:
                return
            seen += 1
            yield case["name"], qasm, input_bits, output_bits


def benchmark(args: argparse.Namespace) -> list[dict]:
    cases = load_cases(args.manifest)
    backends = args.backends or list(BACKENDS)
    records: list[dict] = []

    for case, qasm, input_bits, output_bits in iter_case_boundaries(cases, args.limit):
        qsop, _stderr, import_elapsed_ns = run_command(
            [str(args.qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
            input_text=qasm,
        )
        header = qsop_header(qsop)
        for backend in backends:
            cmd = [
                str(args.sop_solve),
                "--backend",
                backend,
                "--format",
                "stats",
                "--max-vars",
                str(args.max_vars),
            ]
            if args.trace:
                cmd += ["--trace", "csv"]
            if backend == "branch" and args.branch_heuristic != "split":
                cmd += ["--branch-heuristic", args.branch_heuristic]
            cmd.append("-")
            stats_text, trace_text, solve_elapsed_ns = run_command(cmd, input_text=qsop)
            stats = parse_stats(stats_text)
            trace = parse_trace_csv(trace_text) if args.trace else {}
            records.append(
                {
                    "case": case,
                    "boundary": f"{input_bits}->{output_bits}",
                    "input": input_bits,
                    "output": output_bits,
                    "backend": backend,
                    "branch_heuristic": args.branch_heuristic if backend == "branch" else "",
                    **header,
                    "import_elapsed_ns": import_elapsed_ns,
                    "solve_elapsed_ns": solve_elapsed_ns,
                    "qasm_sha256": sha256_text(qasm),
                    "qsop_sha256": sha256_text(qsop),
                    "stats": stats,
                    "trace": trace,
                }
            )
    return records


def write_jsonl(records: list[dict], file: TextIO) -> None:
    for record in records:
        print(json.dumps(record, sort_keys=True, separators=(",", ":")), file=file)


def write_csv(records: list[dict], file: TextIO) -> None:
    writer = csv.DictWriter(file, fieldnames=CSV_FIELDS)
    writer.writeheader()
    for record in records:
        stats = record["stats"]
        row = {field: record.get(field, "") for field in CSV_FIELDS}
        for key in ("search_nodes", "leaf_assignments", "cache_hits", "cache_misses", "components"):
            row[key] = stats.get(key, "")
        row["trace_summary"] = trace_summary_text(record["trace"])
        writer.writerow(row)


def write_top_records(records: list[dict], args: argparse.Namespace, file: TextIO) -> None:
    if args.top == 0:
        return

    print(f"top_records_by_{args.top_metric}:", file=file)
    for backend in sorted({record["backend"] for record in records}):
        backend_records = [
            record
            for record in records
            if record["backend"] == backend and record_has_metric(record, args.top_metric)
        ]
        ranked = sorted(
            backend_records,
            key=lambda record: (
                record_metric(record, args.top_metric),
                record["solve_elapsed_ns"],
                record["case"],
                record["boundary"],
            ),
            reverse=True,
        )
        print(f"  backend: {backend}", file=file)
        if not ranked:
            print("    no records report this metric", file=file)
            continue
        for record in ranked[: args.top]:
            stats = record["stats"]
            line = (
                f"    {record['case']} {record['boundary']} "
                f"value={record_metric(record, args.top_metric)} "
                f"nvars={record['nvars']} nedges={record['nedges']} "
                f"solve_elapsed_ns={record['solve_elapsed_ns']}"
            )
            if "search_nodes" in stats:
                line += f" search_nodes={stats['search_nodes']}"
            if "leaf_assignments" in stats:
                line += f" leaf_assignments={stats['leaf_assignments']}"
            if "components" in stats:
                line += f" components={stats['components']}"
            if "cache_hits" in stats or "cache_misses" in stats:
                line += f" cache={stats.get('cache_hits', 0)}/{stats.get('cache_misses', 0)}"
            trace_phase = dominant_trace_phase(record)
            if trace_phase:
                line += f" top_trace={trace_phase}"
            print(line, file=file)


def write_summary(records: list[dict], args: argparse.Namespace, file: TextIO) -> None:
    print(f"records: {len(records)}", file=file)
    summary = summarize_records(records)
    for backend in sorted(summary):
        entry = summary[backend]
        stats = entry["stats"]
        print(f"backend: {backend}", file=file)
        print(f"  records: {entry['records']}", file=file)
        print(f"  import_elapsed_ns: {entry['import_elapsed_ns']}", file=file)
        print(f"  solve_elapsed_ns: {entry['solve_elapsed_ns']}", file=file)
        for key in ("search_nodes", "leaf_assignments", "components", "cache_hits", "cache_misses"):
            if key in stats:
                print(f"  {key}: {stats[key]}", file=file)
        if "cache_hits" in stats or "cache_misses" in stats:
            print(f"  cache_hit_rate: {cache_hit_rate(stats)}", file=file)

        trace = entry["trace"]
        if trace:
            print("  trace:", file=file)
            for phase in sorted(trace):
                values = trace[phase]
                print(
                    f"    {phase}: events={values['events']} "
                    f"items={values['items']} elapsed_ns={values['elapsed_ns']}",
                    file=file,
                )
    write_top_records(records, args, file)


def parse_args(argv: list[str]) -> argparse.Namespace:
    source_root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Benchmark the QASM solver corpus.")
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("sop_solve", type=pathlib.Path)
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=source_root / "tests" / "qasm_solver_corpus.json",
        help="QASM corpus manifest.",
    )
    parser.add_argument(
        "--backend",
        dest="backends",
        action="append",
        choices=BACKENDS,
        help="Backend to benchmark. May be repeated; defaults to all backends.",
    )
    parser.add_argument("--format", choices=("jsonl", "csv", "summary"), default="jsonl")
    parser.add_argument("--limit", type=int, help="Limit case-boundary pairs before backend expansion.")
    parser.add_argument("--max-vars", type=int, default=24, help="Pass-through solver variable guard.")
    parser.add_argument("--trace", action="store_true", help="Collect and summarize sop-solve CSV trace rows.")
    parser.add_argument(
        "--branch-heuristic",
        choices=BRANCH_HEURISTICS,
        default="split",
        help="Variable-choice heuristic used by the branch backend.",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=0,
        help="With --format summary, print this many largest case-boundary records per backend.",
    )
    parser.add_argument(
        "--top-metric",
        choices=TOP_METRICS,
        default="solve_elapsed_ns",
        help="Metric used by --top ranking.",
    )
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_vars < 0:
        parser.error("--max-vars must be non-negative")
    if args.top < 0:
        parser.error("--top must be non-negative")
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
    elif args.format == "csv":
        write_csv(records, sys.stdout)
    else:
        write_summary(records, args, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
