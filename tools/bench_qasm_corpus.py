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
CSV_FIELDS = [
    "case",
    "boundary",
    "input",
    "output",
    "backend",
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
        stats[key] = value if key == "backend" else int(value)
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
            cmd = [str(args.sop_solve), "--backend", backend, "--format", "stats"]
            if args.trace:
                cmd += ["--trace", "csv"]
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
    parser.add_argument("--format", choices=("jsonl", "csv"), default="jsonl")
    parser.add_argument("--limit", type=int, help="Limit case-boundary pairs before backend expansion.")
    parser.add_argument("--trace", action="store_true", help="Collect and summarize sop-solve CSV trace rows.")
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
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
        write_csv(records, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
